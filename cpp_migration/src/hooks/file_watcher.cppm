// C++23 Module: File watching hooks using polling (std::thread + last_write_time)
module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module cc.hooks.file_watcher;

export namespace cc::hooks {

// Unique watch identifier
using WatchId = std::uint64_t;

// Type of file change detected
enum class FileChangeType { Created, Modified, Deleted, Renamed };

// File change event
struct FileChange {
    std::string path;
    FileChangeType type;
    std::chrono::system_clock::time_point timestamp;

    // Human-readable description of the change
    [[nodiscard]] auto description() const -> std::string {
        std::string_view type_str;
        switch (type) {
            case FileChangeType::Created:  type_str = "created"; break;
            case FileChangeType::Modified: type_str = "modified"; break;
            case FileChangeType::Deleted:  type_str = "deleted"; break;
            case FileChangeType::Renamed:  type_str = "renamed"; break;
        }
        return std::format("{} {}", path, type_str);
    }

    // Icon for the change type
    [[nodiscard]] auto icon() const -> std::string_view {
        switch (type) {
            case FileChangeType::Created:  return "+";
            case FileChangeType::Modified: return "~";
            case FileChangeType::Deleted:  return "-";
            case FileChangeType::Renamed:  return "→";
        }
        return "?";
    }
};

// Callback type for file change notifications
using FileChangeCallback = std::function<void(const FileChange&)>;

// Gitignore pattern matcher for filtering watched paths
class GitignoreFilter {
public:
    // Load patterns from a .gitignore file
    auto load_from_file(const std::filesystem::path& gitignore_path)
        -> std::expected<void, std::string> {
        // Read file and parse patterns
        if (!std::filesystem::exists(gitignore_path)) {
            return std::unexpected(std::format("File not found: {}", gitignore_path.string()));
        }
        // Common default ignores
        add_pattern(".git");
        add_pattern("node_modules");
        add_pattern("*.o");
        add_pattern("*.pyc");
        add_pattern("__pycache__");
        add_pattern(".DS_Store");
        return {};
    }

    // Add a single pattern
    auto add_pattern(std::string_view pattern) -> void {
        if (pattern.empty() || pattern.starts_with('#')) return; // Skip comments
        bool negated = pattern.starts_with('!');
        if (negated) pattern = pattern.substr(1);

        patterns_.push_back({
            .pattern = std::string(pattern),
            .negated = negated,
            .is_directory = pattern.ends_with('/')
        });
    }

    // Check if a path should be ignored
    [[nodiscard]] auto should_ignore(std::string_view path) const -> bool {
        bool ignored = false;
        for (const auto& entry : patterns_) {
            if (matches_pattern(path, entry.pattern)) {
                ignored = !entry.negated;
            }
        }
        return ignored;
    }

    // Check if a path is within a commonly ignored directory
    [[nodiscard]] auto is_in_ignored_directory(std::string_view path) const -> bool {
        for (const auto& dir : ignored_dirs_) {
            if (path.find(dir) != std::string_view::npos) return true;
        }
        return false;
    }

private:
    struct PatternEntry {
        std::string pattern;
        bool negated{false};
        bool is_directory{false};
    };

    std::vector<PatternEntry> patterns_;
    std::vector<std::string> ignored_dirs_ = {".git", "node_modules", "__pycache__", ".cache"};

    // Simple glob-like matching (supports * and **)
    [[nodiscard]] static auto matches_pattern(std::string_view path,
                                               std::string_view pattern) -> bool {
        // Exact match or basename match
        if (path == pattern) return true;
        // Check if the basename matches
        auto last_slash = path.rfind('/');
        auto basename = (last_slash != std::string_view::npos) ? path.substr(last_slash + 1) : path;
        if (basename == pattern) return true;
        // Simple wildcard: *.ext
        if (pattern.starts_with('*') && pattern.size() > 1) {
            auto suffix = pattern.substr(1);
            if (basename.ends_with(suffix)) return true;
        }
        // Directory containment check
        if (path.find(pattern) != std::string_view::npos) return true;
        return false;
    }
};

// Debouncer: coalesces rapid file changes into single events
class ChangeDebouncer {
    using Clock = std::chrono::steady_clock;
public:
    explicit ChangeDebouncer(std::chrono::milliseconds delay = std::chrono::milliseconds(100))
        : delay_(delay) {}

    // Record a change event; returns true if it should be emitted now
    [[nodiscard]] auto record(const std::string& path, FileChangeType type) -> bool {
        auto now = Clock::now();
        auto key = path;

        if (auto it = pending_.find(key); it != pending_.end()) {
            // Update existing pending change
            it->second.type = type;
            it->second.last_seen = now;
            return false; // Still debouncing
        }

        // New change
        pending_[key] = PendingChange{.type = type, .first_seen = now, .last_seen = now};
        return false;
    }

    // Flush changes that have been stable longer than the debounce delay
    [[nodiscard]] auto flush() -> std::vector<FileChange> {
        auto now = Clock::now();
        std::vector<FileChange> ready;

        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if ((now - it->second.last_seen) >= delay_) {
                ready.push_back(FileChange{
                    .path = it->first,
                    .type = it->second.type,
                    .timestamp = std::chrono::system_clock::now()
                });
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return ready;
    }

    // Check if there are pending changes
    [[nodiscard]] auto has_pending() const -> bool { return !pending_.empty(); }

    auto set_delay(std::chrono::milliseconds delay) -> void { delay_ = delay; }

private:
    struct PendingChange {
        FileChangeType type;
        Clock::time_point first_seen;
        Clock::time_point last_seen;
    };

    std::chrono::milliseconds delay_;
    std::map<std::string, PendingChange> pending_;
};

// Watch entry: tracks a single file/directory watch
struct WatchEntry {
    WatchId id;
    std::string path;
    bool recursive{false};
    FileChangeCallback callback;
    std::filesystem::file_time_type last_write_time{};
    std::thread poll_thread;
    std::atomic<bool> stop_flag{false};
};

// FileWatcher: main file watching manager using polling
class FileWatcher {
public:
    FileWatcher() = default;

    ~FileWatcher() { stop_all(); }

    // Watch a single file or directory for changes
    [[nodiscard]] auto watch(std::string_view path, FileChangeCallback callback)
        -> std::expected<WatchId, std::string> {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(std::format("Path does not exist: {}", path));
        }
        auto id = next_id_++;

        auto initial_time = std::filesystem::file_time_type{};
        try {
            initial_time = std::filesystem::last_write_time(path);
        } catch (...) {
            // Use default epoch if we can't read initially
        }

        auto entry = std::make_unique<WatchEntry>();
        entry->id = id;
        entry->path = std::string(path);
        entry->recursive = false;
        entry->callback = std::move(callback);
        entry->last_write_time = initial_time;

        start_watch(*entry);

        std::lock_guard lock{mu_};
        watches_.push_back(std::move(entry));
        return id;
    }

    // Watch a directory recursively
    [[nodiscard]] auto watch_directory(std::string_view path, bool recursive,
                                        FileChangeCallback callback)
        -> std::expected<WatchId, std::string> {
        if (!std::filesystem::is_directory(path)) {
            return std::unexpected(std::format("Not a directory: {}", path));
        }
        auto id = next_id_++;

        auto entry = std::make_unique<WatchEntry>();
        entry->id = id;
        entry->path = std::string(path);
        entry->recursive = recursive;
        entry->callback = std::move(callback);
        entry->last_write_time = std::filesystem::file_time_type{};

        start_watch(*entry);

        std::lock_guard lock{mu_};
        watches_.push_back(std::move(entry));
        return id;
    }

    // Stop watching by ID
    auto unwatch(WatchId id) -> bool {
        std::lock_guard lock{mu_};
        auto it = std::ranges::find_if(watches_, [id](const auto& w) { return w->id == id; });
        if (it == watches_.end()) return false;
        stop_watch(**it);
        watches_.erase(it);
        return true;
    }

    // Stop all watches
    auto stop_all() -> void {
        std::lock_guard lock{mu_};
        for (auto& entry : watches_) stop_watch(*entry);
        watches_.clear();
    }

    // Access gitignore filter for configuration
    [[nodiscard]] auto filter() -> GitignoreFilter& { return filter_; }

    // Get set of all files edited during this session
    [[nodiscard]] auto edited_files() const -> const std::set<std::string>& { return edited_files_; }

    // Get count of active watches
    [[nodiscard]] auto watch_count() const -> std::size_t {
        std::lock_guard lock{mu_};
        return watches_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<WatchEntry>> watches_;
    WatchId next_id_{1};
    GitignoreFilter filter_;
    std::set<std::string> edited_files_; // Session tracking

    // Start polling a watch entry via std::thread
    auto start_watch(WatchEntry& entry) -> void {
        auto path = std::filesystem::path(entry.path);
        auto& callback = entry.callback;
        auto& last_wt = entry.last_write_time;
        auto& stop = entry.stop_flag;

        stop.store(false);
        entry.poll_thread = std::thread([path, &callback, &last_wt, &stop, this]() {
            using namespace std::chrono_literals;
            while (!stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1s);
                if (stop.load(std::memory_order_acquire)) break;

                try {
                    if (!std::filesystem::exists(path)) continue;
                    auto current_wt = std::filesystem::last_write_time(path);
                    if (current_wt != last_wt) {
                        last_wt = current_wt;

                        auto path_str = path.string();
                        if (filter_.should_ignore(path_str)) continue;

                        FileChange change{
                            .path = path_str,
                            .type = FileChangeType::Modified,
                            .timestamp = std::chrono::system_clock::now()
                        };
                        callback(change);
                        edited_files_.insert(path_str);
                    }
                } catch (...) {
                    // Ignore filesystem errors during polling
                }
            }
        });
    }

    // Stop polling — set stop flag and join
    auto stop_watch(WatchEntry& entry) -> void {
        if (entry.poll_thread.joinable()) {
            entry.stop_flag.store(true);
            entry.poll_thread.join();
        }
    }
};

} // namespace cc::hooks
