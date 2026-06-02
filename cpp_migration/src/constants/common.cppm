/// @file common.cppm
/// @brief Common utility functions for date handling.
/// Migrated from src/constants/common.ts
module;

#include <string>
#include <chrono>
#include <format>
#include <cstdlib>
#include <optional>

export module cc.constants.common;

export namespace cc::constants::common {

/// Get current local date in ISO format (YYYY-MM-DD)
[[nodiscard]] inline std::string get_local_iso_date() {
    // Check for date override env var
    if (const char* override_date = std::getenv("CLAUDE_CODE_OVERRIDE_DATE")) {
        return std::string(override_date);
    }
    
    auto now = std::chrono::system_clock::now();
    auto days = std::chrono::floor<std::chrono::days>(now);
    std::chrono::year_month_day ymd{days};
    
    return std::format("{:04d}-{:02d}-{:02d}",
        static_cast<int>(ymd.year()),
        static_cast<unsigned>(ymd.month()),
        static_cast<unsigned>(ymd.day()));
}

/// Memoized session start date (captures date once at first call)
[[nodiscard]] inline const std::string& get_session_start_date() {
    static const std::string date = get_local_iso_date();
    return date;
}

/// Returns "Month YYYY" in user's local timezone
[[nodiscard]] inline std::string get_local_month_year() {
    auto now = std::chrono::system_clock::now();
    auto days = std::chrono::floor<std::chrono::days>(now);
    std::chrono::year_month_day ymd{days};
    
    static constexpr std::array<std::string_view, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    
    auto month_idx = static_cast<unsigned>(ymd.month()) - 1;
    return std::format("{} {:04d}", months[month_idx], static_cast<int>(ymd.year()));
}

} // namespace cc::constants::common
