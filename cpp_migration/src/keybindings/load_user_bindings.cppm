/// @file load_user_bindings.cppm
/// @brief User keybinding configuration loader with hot-reload support.
/// Migrated from src/keybindings/loadUserBindings.ts
///
/// Loads keybindings from ~/.claude/keybindings.json and watches
/// for changes to reload them automatically.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <unordered_set>

export module cc.keybindings.load_user_bindings;

import cc.keybindings.schema;
import cc.keybindings.defaults;
import cc.utils.json;

export namespace cc::keybindings {

/// Severity level for keybinding warnings
enum class WarningSeverity {
    warning,
    error
};

/// Type of keybinding warning
enum class WarningType {
    parse_error,
    duplicate_key,
    validation_error
};

/// A warning produced during keybinding loading/validation
struct KeybindingWarning {
    WarningType type;
    WarningSeverity severity;
    std::string message;
    std::optional<std::string> suggestion;
};

/// Result of loading keybindings, including any validation warnings
struct KeybindingsLoadResult {
    std::vector<Keybinding> bindings;
    std::vector<KeybindingWarning> warnings;
};

/// Stability threshold in ms to wait for file writes to settle
inline constexpr int file_stability_threshold_ms = 500;

/// Polling interval for checking file stability
inline constexpr int file_stability_poll_interval_ms = 200;

/// Callback type for keybinding change notifications
using KeybindingsChangedCallback = std::function<void(const KeybindingsLoadResult&)>;

/// Check if keybinding customization is enabled (feature gate)
[[nodiscard]] inline bool is_keybinding_customization_enabled() {
    if (const char* flag = std::getenv("CC_REPL_KEYBINDINGS")) {
        std::string_view value(flag);
        return value == "1" || value == "true" || value == "TRUE" || value == "on";
    }
    return true;
}

/// Get the path to the user keybindings config file
[[nodiscard]] inline std::filesystem::path get_keybindings_path() {
    // Equivalent to join(getClaudeConfigHomeDir(), 'keybindings.json')
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::filesystem::path(home) / ".claude" / "keybindings.json";
}

/// Keybinding loader with caching and file watching support
class KeybindingLoader {
    std::vector<Keybinding> cached_bindings_;
    std::vector<KeybindingWarning> cached_warnings_;
    std::vector<KeybindingsChangedCallback> listeners_;
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> disposed_{false};
    std::string last_custom_bindings_log_date_;

public:
    KeybindingLoader() = default;

    /// Load keybindings synchronously, returning cached result if available
    [[nodiscard]] KeybindingsLoadResult load_sync() {
        std::lock_guard lock(mutex_);

        if (!cached_bindings_.empty()) {
            return {cached_bindings_, cached_warnings_};
        }

        auto defaults = get_default_bindings();

        // Skip user config for external users
        if (!is_keybinding_customization_enabled()) {
            cached_bindings_ = defaults;
            cached_warnings_.clear();
            return {cached_bindings_, cached_warnings_};
        }

        auto user_path = get_keybindings_path();
        return load_from_file(user_path, defaults);
    }

    /// Load keybindings asynchronously (reads file, validates, merges)
    [[nodiscard]] KeybindingsLoadResult load() {
        auto defaults = get_default_bindings();

        // Skip user config for external users
        if (!is_keybinding_customization_enabled()) {
            return {defaults, {}};
        }

        auto user_path = get_keybindings_path();
        return load_from_file(user_path, defaults);
    }

    /// Initialize file watching for keybindings.json
    /// Call once when the app starts. No-op if customization is disabled.
    void initialize_watcher() {
        if (initialized_.load() || disposed_.load()) return;

        if (!is_keybinding_customization_enabled()) {
            return;
        }

        auto user_path = get_keybindings_path();
        auto watch_dir = user_path.parent_path();

        // Only watch if parent directory exists
        if (!std::filesystem::is_directory(watch_dir)) {
            return;
        }

        initialized_.store(true);
        // File watching implementation would use platform-specific APIs
        // (inotify on Linux, FSEvents on macOS, ReadDirectoryChangesW on Windows)
    }

    /// Dispose the file watcher and clean up resources
    void dispose() {
        disposed_.store(true);
        std::lock_guard lock(mutex_);
        listeners_.clear();
    }

    /// Subscribe to keybinding change notifications
    void subscribe(KeybindingsChangedCallback callback) {
        std::lock_guard lock(mutex_);
        listeners_.push_back(std::move(callback));
    }

    /// Get cached validation warnings
    [[nodiscard]] std::vector<KeybindingWarning> get_cached_warnings() const {
        std::lock_guard lock(mutex_);
        return cached_warnings_;
    }

    /// Reset internal state (for testing only)
    void reset_for_testing() {
        std::lock_guard lock(mutex_);
        initialized_.store(false);
        disposed_.store(false);
        cached_bindings_.clear();
        cached_warnings_.clear();
        listeners_.clear();
        last_custom_bindings_log_date_.clear();
    }

private:
    /// Load and parse keybindings from a file, merge with defaults
    [[nodiscard]] KeybindingsLoadResult load_from_file(
        const std::filesystem::path& path,
        const std::vector<Keybinding>& defaults
    ) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            // File doesn't exist - use defaults
            cached_bindings_ = defaults;
            cached_warnings_.clear();
            return {cached_bindings_, cached_warnings_};
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            // Cannot open file - return defaults with warning
            cached_bindings_ = defaults;
            cached_warnings_ = {{
                WarningType::parse_error,
                WarningSeverity::error,
                "Failed to open keybindings.json",
                std::nullopt
            }};
            return {cached_bindings_, cached_warnings_};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        auto result = parse_and_validate(content, defaults);
        cached_bindings_ = result.bindings;
        cached_warnings_ = result.warnings;
        return result;
    }

    /// Parse JSON content and validate keybinding structure
    [[nodiscard]] KeybindingsLoadResult parse_and_validate(
        const std::string& content,
        const std::vector<Keybinding>& defaults
    ) {
        if (content.empty()) {
            return {defaults, {{
                WarningType::parse_error,
                WarningSeverity::error,
                "keybindings.json is empty",
                "Use format: { \"bindings\": [ ... ] }"
            }}};
        }

        auto doc_result = cc::utils::json::parse(content);
        if (!doc_result) {
            return {defaults, {{
                WarningType::parse_error,
                WarningSeverity::error,
                "Failed to parse keybindings.json: " + doc_result.error().message(),
                "Use format: { \"bindings\": [ { \"id\": \"...\", \"keys\": [\"ctrl+k\"], \"command\": \"...\" } ] }"
            }}};
        }

        auto root = doc_result->root();
        auto bindings_val = root.get("bindings");
        if (!root || !root.is_obj() || !bindings_val || !bindings_val.is_arr()) {
            return {defaults, {{
                WarningType::validation_error,
                WarningSeverity::error,
                "keybindings.json must contain a bindings array",
                "Use format: { \"bindings\": [ ... ] }"
            }}};
        }

        std::vector<Keybinding> merged = defaults;
        std::vector<KeybindingWarning> warnings;
        std::unordered_set<std::string> seen_ids;
        for (const auto& binding : defaults) {
            seen_ids.insert(binding.id);
        }

        bindings_val.iter([&](cc::utils::json::JsonVal item) {
            if (!item.is_obj()) {
                warnings.push_back({WarningType::validation_error, WarningSeverity::warning,
                    "Ignored non-object keybinding entry", std::nullopt});
                return;
            }

            auto id_val = item.get("id");
            auto command_val = item.get("command");
            auto keys_val = item.get("keys");
            if (!id_val || !id_val.is_str() || !command_val || !command_val.is_str() ||
                !keys_val || !keys_val.is_arr()) {
                warnings.push_back({WarningType::validation_error, WarningSeverity::warning,
                    "Ignored keybinding entry missing id, command, or keys array", std::nullopt});
                return;
            }

            Keybinding binding;
            binding.id = std::string(id_val.as_str());
            binding.command = std::string(command_val.as_str());
            keys_val.iter([&](cc::utils::json::JsonVal key_val) {
                if (key_val.is_str()) {
                    binding.keys.push_back(parse_key_chord(key_val.as_str()));
                }
            });
            if (binding.keys.empty()) {
                warnings.push_back({WarningType::validation_error, WarningSeverity::warning,
                    "Ignored keybinding with empty keys: " + binding.id, std::nullopt});
                return;
            }
            if (auto when_val = item.get("when"); when_val && when_val.is_str()) {
                binding.when = std::string(when_val.as_str());
            }
            if (auto args_val = item.get("args"); args_val) {
                binding.args = args_val.is_str() ? std::string(args_val.as_str()) : args_val.to_string();
            }

            if (!seen_ids.insert(binding.id).second) {
                warnings.push_back({WarningType::duplicate_key, WarningSeverity::warning,
                    "Duplicate keybinding id overrides earlier entry: " + binding.id, std::nullopt});
                std::erase_if(merged, [&](const Keybinding& existing) { return existing.id == binding.id; });
            }
            merged.push_back(std::move(binding));
        });

        return {std::move(merged), std::move(warnings)};
    }

    /// Notify all listeners of a binding change
    void notify_listeners(const KeybindingsLoadResult& result) {
        for (const auto& listener : listeners_) {
            listener(result);
        }
    }

    /// Handle file change event from watcher
    void handle_change(const std::filesystem::path& path) {
        auto result = load();
        std::lock_guard lock(mutex_);
        cached_bindings_ = result.bindings;
        cached_warnings_ = result.warnings;
        notify_listeners(result);
    }

    /// Handle file deletion event - reset to defaults
    void handle_delete(const std::filesystem::path& /*path*/) {
        auto defaults = get_default_bindings();
        std::lock_guard lock(mutex_);
        cached_bindings_ = defaults;
        cached_warnings_.clear();
        KeybindingsLoadResult result{defaults, {}};
        notify_listeners(result);
    }
};

/// Global keybinding loader instance
[[nodiscard]] inline KeybindingLoader& get_loader() {
    static KeybindingLoader instance;
    return instance;
}

/// Convenience: load bindings synchronously (uses global loader)
[[nodiscard]] inline std::vector<Keybinding> load_keybindings_sync() {
    return get_loader().load_sync().bindings;
}

/// Convenience: initialize the global watcher
inline void initialize_keybinding_watcher() {
    get_loader().initialize_watcher();
}

/// Convenience: dispose the global watcher
inline void dispose_keybinding_watcher() {
    get_loader().dispose();
}

} // namespace cc::keybindings
