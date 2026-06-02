// C++23 Module: Auto-save and session persistence with crash recovery
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <algorithm>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module cc.hooks.auto_save;

import cc.utils.json;

export namespace cc::hooks {

[[nodiscard]] inline std::chrono::system_clock::time_point file_time_to_system_time(
    std::filesystem::file_time_type file_time
) {
    const auto now_file = std::filesystem::file_time_type::clock::now();
    const auto now_system = std::chrono::system_clock::now();
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - now_file + now_system
    );
}

// Auto-save trigger conditions
struct AutoSaveConfig {
    std::chrono::milliseconds interval_ms{30000};   // Save every 30s
    bool on_exit{true};                              // Save on graceful exit
    bool on_tool_use{true};                          // Save after each tool use
    bool on_message_complete{true};                  // Save after each message
    std::size_t max_undo_stack{50};                  // Max undo entries to persist
    std::filesystem::path save_directory;            // Where to store session files
    std::size_t max_session_files{10};               // Max concurrent session saves
};

// Session metadata for persistence
struct SessionMetadata {
    std::string session_id;
    std::string model_name;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point last_saved_at;
    std::size_t message_count{0};
    std::size_t total_tokens{0};
    std::string working_directory;
    std::vector<std::string> active_files;   // Files referenced in session
    std::optional<std::string> project_name;

    // Generate filename for this session's save file
    [[nodiscard]] auto save_filename() const -> std::string {
        return std::format("session_{}.json", session_id);
    }

    // Calculate session duration
    [[nodiscard]] auto duration_seconds() const -> std::size_t {
        auto now = std::chrono::system_clock::now();
        return static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count());
    }
};

// Undo stack entry for operation reversal
struct UndoEntry {
    std::string operation;       // e.g., "file_write", "file_edit", "delete"
    std::string target_path;     // File path affected
    std::string previous_content; // Content before the operation
    std::chrono::system_clock::time_point timestamp;

    [[nodiscard]] auto description() const -> std::string {
        return std::format("{} on {}", operation, target_path);
    }
};

// Conversation message for serialization
struct SavedMessage {
    std::string role;        // "user", "assistant", "system", "tool"
    std::string content;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_input;
    std::optional<std::string> tool_output;
};

// Session save state (what gets persisted)
struct SessionState {
    SessionMetadata metadata;
    std::vector<SavedMessage> messages;
    std::vector<UndoEntry> undo_stack;
    std::map<std::string, std::string> settings;  // Current settings snapshot
};

// Recovery information from a crashed session
struct RecoveryInfo {
    std::string session_id;
    std::filesystem::path save_path;
    std::chrono::system_clock::time_point last_saved;
    std::size_t message_count{0};
    bool is_corrupted{false};
    std::optional<std::string> error_detail;

    [[nodiscard]] auto age_description() const -> std::string {
        auto now = std::chrono::system_clock::now();
        auto hours = std::chrono::duration_cast<std::chrono::hours>(now - last_saved).count();
        if (hours < 1) return "less than an hour ago";
        if (hours < 24) return std::format("{} hours ago", hours);
        return std::format("{} days ago", hours / 24);
    }
};

// Save result
enum class SaveStatus { Success, PartialFailure, Error };

struct SaveResult {
    SaveStatus status;
    std::filesystem::path path;
    std::size_t bytes_written{0};
    std::optional<std::string> error;
};

// AutoSaveManager: handles periodic and event-driven session persistence
class AutoSaveManager {
public:
    explicit AutoSaveManager(AutoSaveConfig config = {}) : config_(config) {
        ensure_save_directory();
    }

    // Enable auto-save with given configuration
    auto enable(AutoSaveConfig config) -> void {
        config_ = config;
        enabled_ = true;
        ensure_save_directory();
    }

    // Disable auto-save
    auto disable() -> void { enabled_ = false; }

    // Force immediate save
    [[nodiscard]] auto save_now() -> std::expected<SaveResult, std::string> {
        if (!state_provider_) {
            return std::unexpected("No state provider registered");
        }
        auto state = state_provider_();
        return write_session(state);
    }

    // Called on state change; triggers debounced save
    auto on_state_change() -> void {
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
        if (!last_save_ || (now - *last_save_) >= config_.interval_ms) {
            auto result = save_now();
            if (result) last_save_ = now;
        }
    }

    // Called after tool use completes
    auto on_tool_use() -> void {
        if (enabled_ && config_.on_tool_use) {
            save_now(); // Ignore result for background saves
            last_save_ = std::chrono::steady_clock::now();
        }
    }

    // Called after a complete message exchange
    auto on_message_complete() -> void {
        if (enabled_ && config_.on_message_complete) {
            save_now();
            last_save_ = std::chrono::steady_clock::now();
        }
    }

    // Called on graceful exit
    auto on_exit() -> void {
        if (enabled_ && config_.on_exit) {
            save_now();
            cleanup_lock_file();
        }
    }

    // Register state provider (returns current session state)
    auto set_state_provider(std::function<SessionState()> provider) -> void {
        state_provider_ = std::move(provider);
    }

    // ─── Recovery ─────────────────────────────────────────────

    // Detect crashed sessions (lock file exists but process is gone)
    [[nodiscard]] auto detect_crash_recovery() const -> std::vector<RecoveryInfo> {
        std::vector<RecoveryInfo> recoverable;
        if (!std::filesystem::exists(config_.save_directory)) return recoverable;

        for (const auto& entry : std::filesystem::directory_iterator(config_.save_directory)) {
            if (entry.path().extension() == ".json" &&
                entry.path().stem().string().starts_with("session_")) {
                // Check for corresponding lock file (indicates unclean shutdown)
                auto lock_path = entry.path();
                lock_path.replace_extension(".lock");
                if (std::filesystem::exists(lock_path)) {
                    auto last_write = std::filesystem::last_write_time(entry.path());
                    auto sctp = file_time_to_system_time(last_write);
                    recoverable.push_back(RecoveryInfo{
                        .session_id = entry.path().stem().string().substr(8), // strip "session_"
                        .save_path = entry.path(),
                        .last_saved = sctp,
                        .is_corrupted = false
                    });
                }
            }
        }
        return recoverable;
    }

    // Restore a session from saved state
    [[nodiscard]] auto restore_session(const RecoveryInfo& info)
        -> std::expected<SessionState, std::string> {
        return read_session(info.save_path);
    }

    // Clean up recovery files after successful restore
    auto cleanup_recovery(const RecoveryInfo& info) -> void {
        auto lock_path = info.save_path;
        lock_path.replace_extension(".lock");
        std::filesystem::remove(lock_path);
    }

    // ─── Undo Stack Management ────────────────────────────────

    // Push an undo entry
    auto push_undo(UndoEntry entry) -> void {
        undo_stack_.push_back(std::move(entry));
        if (undo_stack_.size() > config_.max_undo_stack) {
            undo_stack_.erase(undo_stack_.begin());
        }
    }

    // Pop and return the most recent undo entry
    [[nodiscard]] auto pop_undo() -> std::optional<UndoEntry> {
        if (undo_stack_.empty()) return std::nullopt;
        auto entry = std::move(undo_stack_.back());
        undo_stack_.pop_back();
        return entry;
    }

    // Get undo stack for inspection
    [[nodiscard]] auto undo_stack() const -> const std::vector<UndoEntry>& { return undo_stack_; }

    // Accessors
    [[nodiscard]] auto enabled() const -> bool { return enabled_; }
    [[nodiscard]] auto config() const -> const AutoSaveConfig& { return config_; }

private:
    AutoSaveConfig config_;
    bool enabled_{false};
    std::optional<std::chrono::steady_clock::time_point> last_save_;
    std::function<SessionState()> state_provider_;
    std::vector<UndoEntry> undo_stack_;

    // Ensure save directory exists
    auto ensure_save_directory() -> void {
        if (config_.save_directory.empty()) {
            config_.save_directory = std::filesystem::temp_directory_path() / "cc-repl-sessions";
        }
        std::filesystem::create_directories(config_.save_directory);
    }

    // Write session state to disk as JSON
    [[nodiscard]] auto write_session(const SessionState& state)
        -> std::expected<SaveResult, std::string> {
        auto path = config_.save_directory / state.metadata.save_filename();
        // Create lock file to indicate active write
        auto lock_path = path;
        lock_path.replace_extension(".lock");
        { std::ofstream lock(lock_path); lock << "locked"; }

        std::string json = serialize_state(state);
        std::ofstream file(path, std::ios::trunc);
        if (!file) {
            return std::unexpected(std::format("Failed to open file: {}", path.string()));
        }
        file << json;
        auto bytes = static_cast<std::size_t>(json.size());
        file.close();

        // Rotate old session files if needed
        rotate_sessions();

        return SaveResult{
            .status = SaveStatus::Success,
            .path = path,
            .bytes_written = bytes
        };
    }

    // Read session state from disk
    [[nodiscard]] auto read_session(const std::filesystem::path& path)
        -> std::expected<SessionState, std::string> {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(std::format("Session file not found: {}", path.string()));
        }
        std::ifstream file(path);
        if (!file) {
            return std::unexpected(std::format("Failed to open: {}", path.string()));
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        return deserialize_state(content);
    }

    // Serialize state to JSON string
    [[nodiscard]] auto serialize_state(const SessionState& state) const -> std::string {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        root.add("session_id", doc.string(state.metadata.session_id));
        root.add("model", doc.string(state.metadata.model_name));
        root.add("message_count", doc.number(static_cast<int64_t>(state.metadata.message_count)));
        root.add("total_tokens", doc.number(static_cast<int64_t>(state.metadata.total_tokens)));
        root.add("working_dir", doc.string(state.metadata.working_directory));
        root.add("project_name", state.metadata.project_name ? doc.string(*state.metadata.project_name) : doc.null());

        auto active_files = doc.array();
        for (const auto& path : state.metadata.active_files) active_files.append(doc.string(path));
        root.add("active_files", active_files);

        auto messages = doc.array();
        for (const auto& msg : state.messages) {
            auto item = doc.object();
            item.add("role", doc.string(msg.role));
            item.add("content", doc.string(msg.content));
            if (msg.tool_name) item.add("tool_name", doc.string(*msg.tool_name));
            if (msg.tool_input) item.add("tool_input", doc.string(*msg.tool_input));
            if (msg.tool_output) item.add("tool_output", doc.string(*msg.tool_output));
            messages.append(item);
        }
        root.add("messages", messages);

        auto settings = doc.object();
        for (const auto& [key, value] : state.settings) settings.add(key, doc.string(value));
        root.add("settings", settings);

        doc.set_root(root);
        return doc.to_pretty_string();
    }

    // Deserialize state from JSON string
    [[nodiscard]] auto deserialize_state(std::string_view json)
        -> std::expected<SessionState, std::string> {
        if (json.empty()) return std::unexpected("Empty session data");
        auto doc_result = cc::utils::json::parse(json);
        if (!doc_result) return std::unexpected(doc_result.error().message());

        auto root = doc_result->root();
        if (!root || !root.is_obj()) return std::unexpected("Session state root must be an object");

        SessionState state;
        if (auto v = root.get("session_id"); v && v.is_str()) state.metadata.session_id = std::string(v.as_str());
        if (auto v = root.get("model"); v && v.is_str()) state.metadata.model_name = std::string(v.as_str());
        if (auto v = root.get("message_count"); v && v.is_num()) state.metadata.message_count = static_cast<std::size_t>(v.as_int());
        if (auto v = root.get("total_tokens"); v && v.is_num()) state.metadata.total_tokens = static_cast<std::size_t>(v.as_int());
        if (auto v = root.get("working_dir"); v && v.is_str()) state.metadata.working_directory = std::string(v.as_str());
        if (auto v = root.get("project_name"); v && v.is_str()) state.metadata.project_name = std::string(v.as_str());

        if (auto files = root.get("active_files"); files && files.is_arr()) {
            files.iter([&](cc::utils::json::JsonVal item) {
                if (item.is_str()) state.metadata.active_files.emplace_back(item.as_str());
            });
        }
        if (auto messages = root.get("messages"); messages && messages.is_arr()) {
            messages.iter([&](cc::utils::json::JsonVal item) {
                if (!item.is_obj()) return;
                SavedMessage msg;
                if (auto v = item.get("role"); v && v.is_str()) msg.role = std::string(v.as_str());
                if (auto v = item.get("content"); v && v.is_str()) msg.content = std::string(v.as_str());
                if (auto v = item.get("tool_name"); v && v.is_str()) msg.tool_name = std::string(v.as_str());
                if (auto v = item.get("tool_input"); v && v.is_str()) msg.tool_input = std::string(v.as_str());
                if (auto v = item.get("tool_output"); v && v.is_str()) msg.tool_output = std::string(v.as_str());
                state.messages.push_back(std::move(msg));
            });
        }
        if (auto settings = root.get("settings"); settings && settings.is_obj()) {
            settings.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
                if (key.is_str() && value.is_str()) state.settings.emplace(std::string(key.as_str()), std::string(value.as_str()));
            });
        }
        return state;
    }

    // Remove old session files beyond max limit
    auto rotate_sessions() -> void {
        std::vector<std::filesystem::path> session_files;
        for (const auto& entry : std::filesystem::directory_iterator(config_.save_directory)) {
            if (entry.path().extension() == ".json") {
                session_files.push_back(entry.path());
            }
        }
        // Sort by last write time (newest first)
        std::sort(session_files.begin(), session_files.end(), [](const auto& a, const auto& b) {
            return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
        });
        // Remove excess files
        if (session_files.size() > config_.max_session_files) {
            for (auto it = session_files.begin() + static_cast<long>(config_.max_session_files);
                 it != session_files.end(); ++it) {
                std::filesystem::remove(*it);
                auto lock = *it;
                lock.replace_extension(".lock");
                std::filesystem::remove(lock);
            }
        }
    }

    // Remove lock file for current session
    auto cleanup_lock_file() -> void {
        if (!state_provider_) return;
        auto state = state_provider_();
        auto path = config_.save_directory / state.metadata.save_filename();
        path.replace_extension(".lock");
        std::filesystem::remove(path);
    }
};

} // namespace cc::hooks
