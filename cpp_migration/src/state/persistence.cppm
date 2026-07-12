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
#include <cerrno>
#include <fstream>
// POSIX headers for crash-safe atomic writes (fsync the temp file and its
// parent directory before rename, mirroring the TS reference's pattern in
// utils/statsCache.ts which calls handle.sync() + fs.rename()).
#include <unistd.h>
#include <fcntl.h>

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
// Schema versioning, migration & validation
// ============================================================

/// Current on-disk schema version. Bump when the serialized shape changes and
/// add a corresponding step in apply_state_migrations().
inline constexpr int kCurrentStateSchemaVersion = 2;

/// Read the schema_version field from a parsed root, defaulting to 1 for
/// legacy blobs that predate versioning.
[[nodiscard]] inline int detected_schema_version(cc::utils::json::JsonVal root) noexcept {
    auto v = root.get("schema_version");
    if (v && v.is_num()) return static_cast<int>(v.as_int());
    return 1;
}

/// Apply the migration chain to a deserialised AppState, bringing it from
/// `from_version` up to kCurrentStateSchemaVersion. Returns the version
/// reached. Each step is a focused, idempotent transform; new steps are added
/// here whenever the on-disk shape evolves (open/closed: extend, never edit an
/// existing step).
inline int apply_state_migrations(AppState& state, int from_version) {
    int version = from_version;
    // v1 -> v2: normalise the legacy view_selection_mode sentinel. Early
    // builds could persist an empty string here; v2 canonicalises to "none".
    if (version < 2) {
        if (state.view_selection_mode.empty()) {
            state.view_selection_mode = "none";
        }
        version = 2;
    }
    (void)state;
    return version;
}

/// Validate structural invariants on a deserialised AppState. Returns the
/// state on success or an error describing the first violation.
[[nodiscard]] inline std::expected<AppState, Error>
validate_state(const AppState& state) {
    if (state.selected_ip_agent_index < -1) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::invalid_argument,
            std::format("selected_ip_agent_index {} is below -1", state.selected_ip_agent_index)));
    }
    if (state.coordinator_task_index < -1) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::invalid_argument,
            std::format("coordinator_task_index {} is below -1", state.coordinator_task_index)));
    }
    if (state.total_cost_usd < 0.0) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::invalid_argument,
            std::format("total_cost_usd {} is negative", state.total_cost_usd)));
    }
    return state;
}

// ============================================================
// Serialization Functions
// ============================================================

/// Serialize AppState to JSON string.
/// Writes a flat, forward/backward-compatible object (missing keys on read
/// fall back to defaults; extra keys are ignored). The persisted set covers
/// the user-preferences class of AppState fields; runtime/transient flags
/// (is_loading, is_streaming, error_message, pending_*) and conversation
/// history (covered by cc::session::history) are deliberately not persisted.
[[nodiscard]] inline std::expected<std::string, Error> serialize_state(const AppState& state) {
    try {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();

        auto add_opt_str = [&doc, &root](const char* key, const std::optional<std::string>& val) {
            if (val.has_value()) root.add(key, doc.string(*val));
        };

        root.add("verbose", doc.boolean(state.verbose));
        root.add("compact_mode", doc.boolean(state.compact_mode));
        root.add("show_thinking", doc.boolean(state.show_thinking));
        root.add("fast_mode", doc.boolean(state.fast_mode));
        root.add("thinking_enabled", doc.boolean(state.thinking_enabled));
        root.add("prompt_suggestion_enabled", doc.boolean(state.prompt_suggestion_enabled));
        root.add("kairos_enabled", doc.boolean(state.kairos_enabled));
        root.add("is_ultraplan_mode", doc.boolean(state.is_ultraplan_mode));
        root.add("ultraplan_launching", doc.boolean(state.ultraplan_launching));
        root.add("is_brief_only", doc.boolean(state.is_brief_only));
        root.add("show_teammate_message_preview", doc.boolean(state.show_teammate_message_preview));
        root.add("working_directory", doc.string(state.working_directory));
        if (!state.allowed_directories.empty()) {
            auto arr = doc.array();
            for (const auto& d : state.allowed_directories) {
                arr.append(doc.string(d));
            }
            root.add("allowed_directories", arr);
        }
        root.add("view_selection_mode", doc.string(state.view_selection_mode));
        root.add("selected_ip_agent_index", doc.number(static_cast<int64_t>(state.selected_ip_agent_index)));
        root.add("coordinator_task_index", doc.number(static_cast<int64_t>(state.coordinator_task_index)));
        root.add("auth_version", doc.number(static_cast<int64_t>(state.auth_version)));
        root.add("remote_background_task_count", doc.number(static_cast<int64_t>(state.remote_background_task_count)));
        add_opt_str("main_loop_model", state.main_loop_model);
        add_opt_str("advisor_model", state.advisor_model);
        add_opt_str("effort_value", state.effort_value);
        add_opt_str("status_line_text", state.status_line_text);
        root.add("schema_version", doc.number(static_cast<int64_t>(kCurrentStateSchemaVersion)));
        doc.set_root(root);
        return doc.to_string();
    } catch (const std::exception& e) {
        return std::unexpected(cc::utils::make_error(
            ErrorCode::internal_error,
            std::format("Failed to serialize state: {}", e.what())
        ));
    }
}

/// Deserialize JSON string to AppState.
/// Reads every field written by serialize_state (no write-only fields); any
/// absent key falls back to the default from get_default_app_state().
[[nodiscard]] inline std::expected<AppState, Error> deserialize_state(const std::string& json_str) {
    try {
        auto json_result = cc::utils::json::parse(json_str);
        if (!json_result) {
            return std::unexpected(json_result.error());
        }

        AppState state = get_default_app_state();
        auto root = json_result->root();

        if (auto v = root.get("verbose"); v && v.is_bool()) state.verbose = v.as_bool();
        if (auto v = root.get("compact_mode"); v && v.is_bool()) state.compact_mode = v.as_bool();
        if (auto v = root.get("show_thinking"); v && v.is_bool()) state.show_thinking = v.as_bool();
        if (auto v = root.get("fast_mode"); v && v.is_bool()) state.fast_mode = v.as_bool();
        if (auto v = root.get("thinking_enabled"); v && v.is_bool()) state.thinking_enabled = v.as_bool();
        if (auto v = root.get("prompt_suggestion_enabled"); v && v.is_bool()) state.prompt_suggestion_enabled = v.as_bool();
        if (auto v = root.get("kairos_enabled"); v && v.is_bool()) state.kairos_enabled = v.as_bool();
        if (auto v = root.get("is_ultraplan_mode"); v && v.is_bool()) state.is_ultraplan_mode = v.as_bool();
        if (auto v = root.get("ultraplan_launching"); v && v.is_bool()) state.ultraplan_launching = v.as_bool();
        if (auto v = root.get("is_brief_only"); v && v.is_bool()) state.is_brief_only = v.as_bool();
        if (auto v = root.get("show_teammate_message_preview"); v && v.is_bool()) state.show_teammate_message_preview = v.as_bool();
        if (auto v = root.get("working_directory"); v && v.is_str()) state.working_directory = std::string(v.as_str());
        if (auto v = root.get("allowed_directories"); v.is_arr()) {
            v.iter([&](auto item) {
                if (item.is_str()) state.allowed_directories.push_back(std::string(item.as_str()));
            });
        }
        if (auto v = root.get("view_selection_mode"); v && v.is_str()) state.view_selection_mode = std::string(v.as_str());
        if (auto v = root.get("selected_ip_agent_index"); v && v.is_num()) state.selected_ip_agent_index = static_cast<std::int32_t>(v.as_int());
        if (auto v = root.get("coordinator_task_index"); v && v.is_num()) state.coordinator_task_index = static_cast<std::int32_t>(v.as_int());
        if (auto v = root.get("auth_version"); v && v.is_num()) state.auth_version = static_cast<std::uint32_t>(v.as_int());
        if (auto v = root.get("remote_background_task_count"); v && v.is_num()) state.remote_background_task_count = static_cast<std::uint32_t>(v.as_int());
        if (auto v = root.get("main_loop_model"); v && v.is_str()) state.main_loop_model = std::string(v.as_str());
        if (auto v = root.get("advisor_model"); v && v.is_str()) state.advisor_model = std::string(v.as_str());
        if (auto v = root.get("effort_value"); v && v.is_str()) state.effort_value = std::string(v.as_str());
        if (auto v = root.get("status_line_text"); v && v.is_str()) state.status_line_text = std::string(v.as_str());

        // Bring the loaded state up to the current schema, then enforce
        // structural invariants before handing it back to the caller.
        apply_state_migrations(state, detected_schema_version(root));
        auto validated = validate_state(state);
        if (!validated) return std::unexpected(validated.error());
        return *validated;
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

            // Open the temp file with a raw POSIX fd so we can fsync it. The
            // TS reference (utils/statsCache.ts) opens the file with mode
            // 0o600, calls handle.writeFile, then handle.sync() (= fsync),
            // then fs.rename for crash-safety. We mirror that here: an ofstream
            // alone only flushes userspace buffers and is not durable across a
            // crash that happens between flush and rename.
            const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) {
                return std::unexpected(cc::utils::make_error(
                    ErrorCode::internal_error,
                    std::format("Failed to open state file for writing: {}", temp_path.string())
                ));
            }

            const std::string& payload = *serialized;
            const char* data = payload.data();
            std::size_t remaining = payload.size();
            bool write_failed = false;
            while (remaining > 0) {
                const ssize_t n = ::write(fd, data, remaining);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    write_failed = true;
                    break;
                }
                data += n;
                remaining -= static_cast<std::size_t>(n);
            }

            // fsync the file contents to durable storage before we rename it
            // into place; without this a crash after rename can leave a torn
            // or empty state file.
            if (!write_failed && ::fsync(fd) != 0) {
                write_failed = true;
            }
            ::close(fd);

            if (write_failed) {
                std::error_code ignore_ec;
                fs::remove(temp_path, ignore_ec);
                return std::unexpected(cc::utils::make_error(
                    ErrorCode::internal_error,
                    "Failed to write/fsync state file"
                ));
            }

            // fsync the parent directory so the rename is durable too. This is
            // best-effort: some filesystems/OSes reject fsync on directories,
            // and a failure here does not corrupt the data already written.
            if (auto parent = state_file_path_.parent_path(); !parent.empty()) {
                const int dir_fd = ::open(parent.c_str(), O_RDONLY);
                if (dir_fd >= 0) {
                    (void)::fsync(dir_fd);
                    ::close(dir_fd);
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
