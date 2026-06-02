/// @file persistence.cppm
/// @brief State persistence module for the Claude Code REPL.
/// Handles saving and loading AppState to/from disk.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <expected>
#include <format>
#include <functional>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <shared_mutex>

export module cc.state.persistence;

import cc.state.app_state;
import cc.utils.json;
import cc.utils.error;

export namespace cc::state::persistence {

namespace fs = std::filesystem;
using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::VoidResult;

// ============================================================
// Serialization Functions
// ============================================================

/// Serialize AppState to JSON string
[[nodiscard]] inline std::expected<std::string, Error> serialize_state(const AppState& state) {
    try {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        root.add("verbose", doc.boolean(state.verbose));
        root.add("compact_mode", doc.boolean(state.compact_mode));
        root.add("show_thinking", doc.boolean(state.show_thinking));
        root.add("fast_mode", doc.boolean(state.fast_mode));
        root.add("thinking_enabled", doc.boolean(state.thinking_enabled));
        root.add("prompt_suggestion_enabled", doc.boolean(state.prompt_suggestion_enabled));
        root.add("kairos_enabled", doc.boolean(state.kairos_enabled));
        root.add("is_ultraplan_mode", doc.boolean(state.is_ultraplan_mode));
        root.add("ultraplan_launching", doc.boolean(state.ultraplan_launching));
        root.add("working_directory", doc.string(state.working_directory));
        root.add("selected_ip_agent_index", doc.number(static_cast<int64_t>(state.selected_ip_agent_index)));
        root.add("coordinator_task_index", doc.number(static_cast<int64_t>(state.coordinator_task_index)));
        root.add("auth_version", doc.number(static_cast<int64_t>(state.auth_version)));
        root.add("schema_version", doc.number(static_cast<int64_t>(1)));
        doc.set_root(root);
        return doc.to_string();
    } catch (const std::exception& e) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::internal_error,
            std::format("Failed to serialize state: {}", e.what())
        ));
    }
}

/// Deserialize JSON string to AppState
[[nodiscard]] inline std::expected<AppState, Error> deserialize_state(const std::string& json_str) {
    try {
        auto json_result = cc::utils::json::parse(json_str);
        if (!json_result) {
            return std::unexpected(json_result.error());
        }
        
        AppState state = get_default_app_state();
        auto root = json_result->root();
        if (auto value = root.get("verbose"); value && value.is_bool()) state.verbose = value.as_bool();
        if (auto value = root.get("compact_mode"); value && value.is_bool()) state.compact_mode = value.as_bool();
        if (auto value = root.get("show_thinking"); value && value.is_bool()) state.show_thinking = value.as_bool();
        if (auto value = root.get("fast_mode"); value && value.is_bool()) state.fast_mode = value.as_bool();
        if (auto value = root.get("working_directory"); value && value.is_str()) state.working_directory = std::string(value.as_str());
        
        return state;
    } catch (const std::exception& e) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::internal_error,
            std::format("Failed to deserialize state: {}", e.what())
        ));
    }
}

// ============================================================
// Persistence Manager
// ============================================================

/// Persistence manager for saving/loading state
class StatePersistence {
    fs::path state_file_path_;
    mutable std::shared_mutex mutex_;
    bool auto_save_enabled_ = true;
    std::chrono::milliseconds auto_save_interval_ = std::chrono::milliseconds(5000);
    std::optional<std::chrono::system_clock::time_point> last_save_time_;

public:
    /// Construct persistence manager with state file path
    explicit StatePersistence(fs::path state_file_path)
        : state_file_path_(std::move(state_file_path)) {}

    /// Get the state file path
    [[nodiscard]] const fs::path& get_state_file_path() const { return state_file_path_; }

    /// Save state to disk
    [[nodiscard]] VoidResult save_state(const AppState& state) {
        try {
            std::unique_lock lock(mutex_);
            
            // Create parent directories if needed
            auto parent_path = state_file_path_.parent_path();
            if (!parent_path.empty() && !fs::exists(parent_path)) {
                fs::create_directories(parent_path);
            }
            
            // Serialize state
            auto serialized = serialize_state(state);
            if (!serialized) {
                return std::unexpected(serialized.error());
            }
            
            // Write to file atomically using temp file
            auto temp_path = state_file_path_;
            temp_path += ".tmp";
            
            {
                std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    return std::unexpected(cc::utils::make_error(
                        ErrorCode::internal_error,
                        std::format("Failed to open state file for writing: {}", temp_path.string())
                    ));
                }
                file << *serialized;
                file.flush();
                if (file.fail()) {
                    return std::unexpected(cc::utils::make_error(
                        ErrorCode::internal_error,
                        "Failed to write state file"
                    ));
                }
            }
            
            // Atomic rename
            fs::rename(temp_path, state_file_path_);
            
            last_save_time_ = std::chrono::system_clock::now();
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(cc::utils::make_error(
                ErrorCode::internal_error,
                std::format("Failed to save state: {}", e.what())
            ));
        }
    }

    /// Load state from disk
    [[nodiscard]] std::expected<AppState, Error> load_state() {
        try {
            std::shared_lock lock(mutex_);
            
            if (!fs::exists(state_file_path_)) {
                // Return default state if file doesn't exist
                return get_default_app_state();
            }
            
            // Read file
            std::ifstream file(state_file_path_, std::ios::binary);
            if (!file) {
                return std::unexpected(cc::utils::make_error(
                    ErrorCode::internal_error,
                    std::format("Failed to open state file for reading: {}", state_file_path_.string())
                ));
            }
            
            std::string json_str((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            
            return deserialize_state(json_str);
        } catch (const std::exception& e) {
            return std::unexpected(cc::utils::make_error(
                ErrorCode::internal_error,
                std::format("Failed to load state: {}", e.what())
            ));
        }
    }

    /// Check if auto-save is enabled
    [[nodiscard]] bool is_auto_save_enabled() const {
        std::shared_lock lock(mutex_);
        return auto_save_enabled_;
    }

    /// Enable/disable auto-save
    void set_auto_save_enabled(bool enabled) {
        std::unique_lock lock(mutex_);
        auto_save_enabled_ = enabled;
    }

    /// Get auto-save interval
    [[nodiscard]] std::chrono::milliseconds get_auto_save_interval() const {
        std::shared_lock lock(mutex_);
        return auto_save_interval_;
    }

    /// Set auto-save interval
    void set_auto_save_interval(std::chrono::milliseconds interval) {
        std::unique_lock lock(mutex_);
        auto_save_interval_ = interval;
    }

    /// Check if it's time to auto-save
    [[nodiscard]] bool should_auto_save() const {
        std::shared_lock lock(mutex_);
        if (!auto_save_enabled_) {
            return false;
        }
        if (!last_save_time_) {
            return true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - *last_save_time_
        );
        return elapsed >= auto_save_interval_;
    }

    /// Delete state file
    [[nodiscard]] VoidResult delete_state() {
        try {
            std::unique_lock lock(mutex_);
            if (fs::exists(state_file_path_)) {
                fs::remove(state_file_path_);
            }
            last_save_time_.reset();
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(cc::utils::make_error(
                ErrorCode::internal_error,
                std::format("Failed to delete state file: {}", e.what())
            ));
        }
    }
};

// ============================================================
// Helper Functions
// ============================================================

/// Get default state file path
[[nodiscard]] inline fs::path get_default_state_file_path() {
    auto home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
    return home / ".claude" / "state" / "app_state.json";
}

} // namespace cc::state::persistence
