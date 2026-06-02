// C++23 Settings Manager Module
// Merges: settings.ts, settingsCache.ts, applySettingsChange.ts,
//         changeDetector.ts, internalWrites.ts
module;

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

export module cc.utils.settings_manager;

import cc.utils.settings_merge;
import cc.utils.settings_paths;
import cc.utils.settings_sources;
import cc.utils.settings_validation;

export namespace cc::utils::settings_manager {

namespace fs = std::filesystem;
using namespace std::chrono;

// ============================================================================
// Types & Enums
// ============================================================================

/// Setting source types in priority order (low to high)
enum class SettingSource {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    FlagSettings,
    PolicySettings,
};

/// Origin of the highest-priority active policy settings source
enum class PolicyOrigin {
    Remote,
    Plist,
    Hklm,
    File,
    Hkcu,
    None,
};

/// Editable setting sources (user can write to these)
enum class EditableSource {
    UserSettings,
    ProjectSettings,
    LocalSettings,
};

/// Represents a JSON settings value (simplified)
using SettingsValue = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    std::string,
    std::vector<std::string>,
    std::map<std::string, std::string>
>;

/// Flat settings map (key -> value)
using SettingsJson = std::map<std::string, SettingsValue>;

/// Validation error from parsing settings
struct ValidationError {
    std::string file;
    std::string path;
    std::string message;
};

/// Result of loading settings with potential errors
struct SettingsWithErrors {
    SettingsJson settings;
    std::vector<ValidationError> errors;
};

/// Result of a settings update operation
struct UpdateResult {
    bool success = true;
    std::string error_message;
};

// ============================================================================
// SettingsCache — Per-session and per-source caching
// ============================================================================

/// Caches parsed settings to avoid repeated file I/O within a session.
/// Mirrors settingsCache.ts: session cache + per-source cache + parse-file cache.
class SettingsCache {
public:
    /// Get the session-level merged settings cache
    [[nodiscard]] std::optional<SettingsWithErrors> get_session_cache() const {
        std::lock_guard lock(mutex_);
        return session_cache_;
    }

    /// Set the session-level merged settings cache
    void set_session_cache(SettingsWithErrors value) {
        std::lock_guard lock(mutex_);
        session_cache_ = std::move(value);
    }

    /// Get cached settings for a specific source (nullopt = cache miss)
    [[nodiscard]] std::optional<std::optional<SettingsJson>>
    get_source_cache(SettingSource source) const {
        std::lock_guard lock(mutex_);
        auto it = per_source_cache_.find(source);
        if (it == per_source_cache_.end()) return std::nullopt;
        return it->second;
    }

    /// Set cached settings for a specific source
    void set_source_cache(SettingSource source, std::optional<SettingsJson> value) {
        std::lock_guard lock(mutex_);
        per_source_cache_[source] = std::move(value);
    }

    /// Get cached parsed file result
    [[nodiscard]] std::optional<SettingsWithErrors>
    get_parsed_file(const fs::path& path) const {
        std::lock_guard lock(mutex_);
        auto it = parse_file_cache_.find(path);
        if (it == parse_file_cache_.end()) return std::nullopt;
        return it->second;
    }

    /// Set cached parsed file result
    void set_parsed_file(const fs::path& path, SettingsWithErrors value) {
        std::lock_guard lock(mutex_);
        parse_file_cache_[path] = std::move(value);
    }

    /// Reset all caches (called when settings files change)
    void reset() {
        std::lock_guard lock(mutex_);
        session_cache_.reset();
        per_source_cache_.clear();
        parse_file_cache_.clear();
    }

    /// Plugin settings base layer (lowest priority)
    void set_plugin_base(SettingsJson settings) {
        std::lock_guard lock(mutex_);
        plugin_base_ = std::move(settings);
    }

    [[nodiscard]] std::optional<SettingsJson> get_plugin_base() const {
        std::lock_guard lock(mutex_);
        return plugin_base_;
    }

    void clear_plugin_base() {
        std::lock_guard lock(mutex_);
        plugin_base_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<SettingsWithErrors> session_cache_;
    std::map<SettingSource, std::optional<SettingsJson>> per_source_cache_;
    std::map<fs::path, SettingsWithErrors> parse_file_cache_;
    std::optional<SettingsJson> plugin_base_;
};

// ============================================================================
// InternalWrites — Track writes made by Claude Code itself
// ============================================================================

/// Tracks file writes made by the application to distinguish them from
/// external changes during file-watching.
class InternalWriteTracker {
public:
    /// Mark a file path as being written internally
    void mark(const fs::path& path) {
        std::lock_guard lock(mutex_);
        writes_[path] = steady_clock::now();
    }

    /// Consume an internal write marker if it's within the time window.
    /// Returns true if the write was internal (should be ignored by watcher).
    bool consume(const fs::path& path, milliseconds window = milliseconds{5000}) {
        std::lock_guard lock(mutex_);
        auto it = writes_.find(path);
        if (it == writes_.end()) return false;

        auto elapsed = steady_clock::now() - it->second;
        if (elapsed <= window) {
            writes_.erase(it);
            return true;
        }
        writes_.erase(it);
        return false;
    }

    /// Clear all tracked writes
    void clear() {
        std::lock_guard lock(mutex_);
        writes_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::map<fs::path, steady_clock::time_point> writes_;
};

// ============================================================================
// ChangeDetector — File-system watching for settings changes
// ============================================================================

/// Subscription handle returned from subscribe()
using UnsubscribeFn = std::function<void()>;
/// Callback type for settings change notifications
using ChangeCallback = std::function<void(SettingSource)>;

/// Watches settings files for changes and notifies subscribers.
/// Port of changeDetector.ts with chokidar-equivalent logic.
class ChangeDetector {
public:
    /// Timing constants (overridable for testing)
    struct Config {
        milliseconds stability_threshold{1000};
        milliseconds poll_interval{500};
        milliseconds deletion_grace{1700};
        milliseconds internal_write_window{5000};
        minutes mdm_poll_interval{30};
    };

    ChangeDetector() : ChangeDetector(Config{}) {}
    explicit ChangeDetector(Config config)
        : config_(std::move(config)) {}

    /// Initialize file watching for all settings sources
    bool initialize() {
        if (initialized_ || disposed_) return false;
        initialized_ = true;
        // In real implementation: set up platform file watchers
        // (inotify on Linux, FSEvents on macOS, ReadDirectoryChangesW on Windows)
        return true;
    }

    /// Subscribe to settings change notifications
    [[nodiscard]] UnsubscribeFn subscribe(ChangeCallback callback) {
        std::lock_guard lock(mutex_);
        auto id = next_id_++;
        subscribers_[id] = std::move(callback);
        return [this, id]() {
            std::lock_guard lock(mutex_);
            subscribers_.erase(id);
        };
    }

    /// Manually notify listeners of a programmatic settings change
    void notify_change(SettingSource source) {
        fan_out(source);
    }

    /// Clean up file watcher resources
    void dispose() {
        disposed_ = true;
        std::lock_guard lock(mutex_);
        subscribers_.clear();
    }

    /// Check if the detector is currently active
    [[nodiscard]] bool is_active() const { return initialized_ && !disposed_; }

    /// Reset for testing
    void reset_for_testing() { reset_for_testing(Config{}); }
    void reset_for_testing(Config config) {
        dispose();
        initialized_ = false;
        disposed_ = false;
        config_ = std::move(config);
    }

private:
    void fan_out(SettingSource source) {
        std::lock_guard lock(mutex_);
        for (const auto& [_, callback] : subscribers_) {
            callback(source);
        }
    }

    Config config_;
    bool initialized_ = false;
    bool disposed_ = false;
    mutable std::mutex mutex_;
    uint64_t next_id_ = 0;
    std::map<uint64_t, ChangeCallback> subscribers_;
};

// ============================================================================
// SettingsApplier — Applies settings changes (merge, write, notify)
// ============================================================================

/// Handles applying settings changes: merge with existing, write to file,
/// invalidate caches, and notify listeners.
class SettingsApplier {
public:
    SettingsApplier(SettingsCache& cache,
                    InternalWriteTracker& write_tracker,
                    ChangeDetector& change_detector)
        : cache_(cache)
        , write_tracker_(write_tracker)
        , change_detector_(change_detector) {}

    /// Apply a settings change to the specified source.
    /// Merges new settings with existing, writes to disk, and resets caches.
    [[nodiscard]] UpdateResult apply(EditableSource source,
                                     const SettingsJson& new_settings) {
        auto file_path = get_file_path_for_editable_source(source);
        if (file_path.empty()) {
            return {.success = true, .error_message = {}};
        }

        // Ensure parent directory exists
        auto parent = fs::path(file_path).parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            return {.success = false,
                    .error_message = "Failed to create directory: " + ec.message()};
        }

        // Read existing settings (bypass cache to avoid stale state)
        auto existing = load_from_file(file_path);

        // Merge: arrays replace, objects deep-merge
        auto merged = merge_settings(existing, new_settings);

        // Mark as internal write before writing
        write_tracker_.mark(file_path);

        // Write to file
        if (!write_settings_file(file_path, merged)) {
            return {.success = false,
                    .error_message = "Failed to write settings file: " + file_path};
        }

        // Invalidate caches
        cache_.reset();

        // Notify if local settings, add to gitignore
        if (source == EditableSource::LocalSettings) {
            // In real impl: async add to .gitignore
        }

        return {.success = true, .error_message = {}};
    }

private:
    [[nodiscard]] static std::string get_file_path_for_editable_source(
        EditableSource source) {
        // Delegates to settings_paths module in real implementation
        switch (source) {
            case EditableSource::UserSettings:
                return "~/.claude/settings.json";
            case EditableSource::ProjectSettings:
                return ".claude/settings.json";
            case EditableSource::LocalSettings:
                return ".claude/settings.local.json";
        }
        return {};
    }

    [[nodiscard]] static SettingsJson load_from_file(
        [[maybe_unused]] const std::string& path) {
        // In real implementation: read and parse JSON file
        return {};
    }

    [[nodiscard]] static SettingsJson merge_settings(
        const SettingsJson& existing, const SettingsJson& incoming) {
        SettingsJson result = existing;
        for (const auto& [key, value] : incoming) {
            // For arrays: replace entirely (caller computes final state)
            // For objects: deep merge
            // For primitives: overwrite
            result[key] = value;
        }
        return result;
    }

    [[nodiscard]] static bool write_settings_file(
        [[maybe_unused]] const std::string& path,
        [[maybe_unused]] const SettingsJson& settings) {
        // In real implementation: serialize to JSON and write atomically
        return true;
    }

    SettingsCache& cache_;
    InternalWriteTracker& write_tracker_;
    ChangeDetector& change_detector_;
};

// ============================================================================
// SettingsManager — Top-level facade
// ============================================================================

/// Main entry point for settings management.
/// Coordinates cache, change detection, and settings loading.
class SettingsManager {
public:
    SettingsManager()
        : applier_(cache_, write_tracker_, change_detector_) {}

    /// Initialize the settings manager (loads settings, starts watching)
    void initialize() {
        change_detector_.initialize();
        // Pre-load settings into cache
        [[maybe_unused]] auto _ = get_settings_with_errors();
    }

    /// Get merged settings from all sources (cached)
    [[nodiscard]] SettingsJson get_initial_settings() {
        return get_settings_with_errors().settings;
    }

    /// Get merged settings with validation errors
    [[nodiscard]] SettingsWithErrors get_settings_with_errors() {
        auto cached = cache_.get_session_cache();
        if (cached) return *cached;

        auto result = load_settings_from_disk();
        cache_.set_session_cache(result);
        return result;
    }

    /// Get settings for a specific source (cached per-source)
    [[nodiscard]] std::optional<SettingsJson> get_settings_for_source(
        SettingSource source) {
        auto cached = cache_.get_source_cache(source);
        if (cached) return *cached;

        auto result = load_settings_for_source(source);
        cache_.set_source_cache(source, result);
        return result;
    }

    /// Update settings for an editable source
    [[nodiscard]] UpdateResult update_settings(EditableSource source,
                                               const SettingsJson& settings) {
        return applier_.apply(source, settings);
    }

    /// Get the policy settings origin
    [[nodiscard]] PolicyOrigin get_policy_origin() const {
        // In real implementation: check remote > mdm > file > hkcu
        return PolicyOrigin::None;
    }

    /// Check if bypass permissions dialog has been accepted
    [[nodiscard]] bool has_skip_dangerous_mode_prompt() {
        // Check user, local, flag, policy sources (not project — RCE risk)
        return false;
    }

    /// Subscribe to settings change events
    [[nodiscard]] UnsubscribeFn on_change(ChangeCallback callback) {
        return change_detector_.subscribe(std::move(callback));
    }

    /// Force reset all caches (used after programmatic changes)
    void reset_cache() {
        cache_.reset();
    }

    /// Dispose all resources
    void dispose() {
        change_detector_.dispose();
    }

    /// Access underlying components (for testing)
    SettingsCache& cache() { return cache_; }
    ChangeDetector& change_detector() { return change_detector_; }
    InternalWriteTracker& write_tracker() { return write_tracker_; }

private:
    /// Load and merge settings from all sources on disk
    [[nodiscard]] SettingsWithErrors load_settings_from_disk() {
        SettingsJson merged;
        std::vector<ValidationError> all_errors;
        std::set<std::string> seen_error_keys;

        // Start with plugin base (lowest priority)
        if (auto plugin = cache_.get_plugin_base()) {
            for (const auto& [k, v] : *plugin) {
                merged[k] = v;
            }
        }

        // Merge sources in priority order
        static constexpr SettingSource sources[] = {
            SettingSource::UserSettings,
            SettingSource::ProjectSettings,
            SettingSource::LocalSettings,
            SettingSource::FlagSettings,
            SettingSource::PolicySettings,
        };

        for (auto source : sources) {
            auto settings = load_settings_for_source(source);
            if (settings) {
                for (const auto& [k, v] : *settings) {
                    merged[k] = v;
                }
            }
        }

        return {.settings = std::move(merged), .errors = std::move(all_errors)};
    }

    /// Load settings for a single source from disk
    [[nodiscard]] std::optional<SettingsJson> load_settings_for_source(
        [[maybe_unused]] SettingSource source) {
        // In real implementation: resolve file path, parse JSON, validate schema
        return std::nullopt;
    }

    SettingsCache cache_;
    InternalWriteTracker write_tracker_;
    ChangeDetector change_detector_;
    SettingsApplier applier_;
};

} // namespace cc::utils::settings_manager
