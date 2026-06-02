// cc.hooks.update_notification — notifies user about available updates
// Migrated from: useUpdateNotification.ts
module;

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <format>

export module cc.hooks.update_notification;

export namespace cc::hooks::update_notification {

struct UpdateInfo {
    std::string current_version;
    std::string latest_version;
    std::string release_url;
    std::string changelog_summary;
    bool is_breaking;
    bool is_security;
};

enum class UpdateCheckResult {
    UpToDate,
    UpdateAvailable,
    CheckFailed
};

enum class UpdateUrgency {
    Low,        // Minor version, no rush
    Medium,     // New features available
    High,       // Security fix or breaking change
    Critical    // Critical security vulnerability
};

namespace detail {

struct UpdateNotificationState {
    std::mutex mutex;
    std::optional<UpdateInfo> cached_info;
    std::optional<std::chrono::system_clock::time_point> last_check;
    bool dismissed{false};
    std::chrono::hours check_interval{24}; // Check once per day
};

inline auto get_state() -> UpdateNotificationState& {
    static UpdateNotificationState state;
    return state;
}

/// Compare two semver strings. Returns: -1 if a < b, 0 if equal, 1 if a > b.
inline auto compare_versions(std::string_view a, std::string_view b) -> int {
    auto parse_part = [](std::string_view& s) -> int {
        int val = 0;
        size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            val = val * 10 + (s[i] - '0');
            ++i;
        }
        if (i < s.size() && s[i] == '.') ++i;
        s = s.substr(i);
        return val;
    };

    std::string_view sa = a, sb = b;
    // Skip 'v' prefix if present
    if (!sa.empty() && sa[0] == 'v') sa = sa.substr(1);
    if (!sb.empty() && sb[0] == 'v') sb = sb.substr(1);

    for (int i = 0; i < 3; ++i) {
        int pa = parse_part(sa);
        int pb = parse_part(sb);
        if (pa < pb) return -1;
        if (pa > pb) return 1;
    }
    return 0;
}

/// Get the path to the update check timestamp file.
inline auto get_check_timestamp_path() -> std::filesystem::path {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "claude-code" / ".last-update-check";
    }
    return std::filesystem::temp_directory_path() / "claude-code-last-update-check";
}

/// Read the last check timestamp from disk.
inline auto read_last_check_time() -> std::optional<std::chrono::system_clock::time_point> {
    auto path = get_check_timestamp_path();
    if (!std::filesystem::exists(path)) return std::nullopt;

    std::ifstream ifs(path);
    std::int64_t epoch_seconds = 0;
    if (ifs >> epoch_seconds) {
        return std::chrono::system_clock::time_point{std::chrono::seconds{epoch_seconds}};
    }
    return std::nullopt;
}

/// Write the current check timestamp to disk.
inline auto write_check_timestamp() -> void {
    auto path = get_check_timestamp_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    ofs << epoch;
}

} // namespace detail

/// Check for available updates.
/// Fetches the update manifest from the release server via curl.
inline auto check_for_updates() -> std::expected<UpdateInfo, std::string> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    // Return cached info if available and recent
    if (state.cached_info.has_value() && state.last_check.has_value()) {
        auto elapsed = std::chrono::system_clock::now() - *state.last_check;
        if (elapsed < state.check_interval) {
            return *state.cached_info;
        }
    }

    // Fetch latest version info via curl
    std::array<char, 4096> buffer{};
    std::string response;

    FILE* pipe = popen(
        "curl -sf --max-time 5 "
        "https://api.github.com/repos/anthropics/claude-code/releases/latest "
        "2>/dev/null", "r");
    if (!pipe) {
        return std::unexpected(std::string{"Unable to check for updates (curl failed)"});
    }

    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        response += buffer.data();
    }
    int status = pclose(pipe);

    if (status != 0 || response.empty()) {
        state.last_check = std::chrono::system_clock::now();
        detail::write_check_timestamp();
        return std::unexpected(std::string{"Unable to check for updates (no network)"});
    }

    // Parse tag_name from JSON response (simple extraction)
    std::string latest_version;
    auto tag_pos = response.find("\"tag_name\"");
    if (tag_pos != std::string::npos) {
        auto colon = response.find(':', tag_pos);
        auto quote_start = response.find('"', colon + 1);
        auto quote_end = response.find('"', quote_start + 1);
        if (quote_start != std::string::npos && quote_end != std::string::npos) {
            latest_version = response.substr(quote_start + 1, quote_end - quote_start - 1);
        }
    }

    if (latest_version.empty()) {
        state.last_check = std::chrono::system_clock::now();
        detail::write_check_timestamp();
        return std::unexpected(std::string{"Unable to parse update info"});
    }

    // Extract release URL
    std::string release_url;
    auto url_pos = response.find("\"html_url\"");
    if (url_pos != std::string::npos) {
        auto colon = response.find(':', url_pos + 10);
        auto quote_start = response.find('"', colon);
        auto quote_end = response.find('"', quote_start + 1);
        if (quote_start != std::string::npos && quote_end != std::string::npos) {
            release_url = response.substr(quote_start + 1, quote_end - quote_start - 1);
        }
    }

    // Determine current version (from env or compile-time constant)
    const char* current = std::getenv("CC_REPL_VERSION");
    std::string current_version = current ? current : "0.0.0";

    UpdateInfo info{
        .current_version = current_version,
        .latest_version = latest_version,
        .release_url = release_url,
        .changelog_summary = {},
        .is_breaking = detail::compare_versions(current_version, latest_version) < -1,
        .is_security = false,
    };

    state.cached_info = info;
    state.last_check = std::chrono::system_clock::now();
    state.dismissed = false;
    detail::write_check_timestamp();

    return info;
}

/// Determine if update notification should be displayed.
inline auto should_show_update_notification() -> bool {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.dismissed) return false;
    if (!state.cached_info.has_value()) return false;

    // Don't show if versions are the same
    if (detail::compare_versions(
        state.cached_info->current_version,
        state.cached_info->latest_version) >= 0) {
        return false;
    }

    return true;
}

/// Dismiss the update notification until next check cycle.
inline auto dismiss_update_notification() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.dismissed = true;
}

/// Get the current update check result.
inline auto get_update_check_result() -> UpdateCheckResult {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!state.cached_info.has_value()) return UpdateCheckResult::CheckFailed;

    if (detail::compare_versions(
        state.cached_info->current_version,
        state.cached_info->latest_version) < 0) {
        return UpdateCheckResult::UpdateAvailable;
    }

    return UpdateCheckResult::UpToDate;
}

/// Format the update notification message for display.
inline auto format_update_message(const UpdateInfo& info) -> std::string {
    std::string msg = std::format(
        "Update available: {} -> {}", info.current_version, info.latest_version);

    if (info.is_security) {
        msg += " (SECURITY FIX)";
    } else if (info.is_breaking) {
        msg += " (BREAKING CHANGES)";
    }

    if (!info.changelog_summary.empty()) {
        msg += "\n  " + info.changelog_summary;
    }

    if (!info.release_url.empty()) {
        msg += "\n  Details: " + info.release_url;
    }

    return msg;
}

/// Get the urgency level of the available update.
inline auto get_update_urgency(const UpdateInfo& info) -> UpdateUrgency {
    if (info.is_security) return UpdateUrgency::Critical;
    if (info.is_breaking) return UpdateUrgency::High;

    // Check if it's a major version bump
    if (detail::compare_versions(info.current_version, info.latest_version) < -1) {
        return UpdateUrgency::Medium;
    }

    return UpdateUrgency::Low;
}

/// Get the last time an update check was performed.
inline auto get_last_check_time() -> std::optional<std::chrono::system_clock::time_point> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (state.last_check.has_value()) return *state.last_check;

    // Try reading from disk
    return detail::read_last_check_time();
}

/// Set the check interval.
inline auto set_check_interval(std::chrono::hours interval) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.check_interval = interval;
}

/// Manually set update info (e.g., from external check or test).
inline auto set_update_info(UpdateInfo info) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.cached_info = std::move(info);
    state.last_check = std::chrono::system_clock::now();
    state.dismissed = false;
}

} // namespace cc::hooks::update_notification
