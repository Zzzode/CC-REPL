module;
#include <array>
#include <chrono>
#include <expected>
#include <string>
#include <string_view>

export module cc.utils.cron_utils;

export namespace cc::utils {

struct CronExpression {
    // Each field stores parsed values; -1 means wildcard (*)
    // Fields: minute, hour, day-of-month, month, day-of-week
    std::array<int, 5> values;       // Single value or -1 for wildcard
    std::array<int, 5> step_values;  // For */N patterns, 0 means no step
    bool valid = false;
};

namespace detail {
    inline bool parse_field(std::string_view field, int& value, int& step, int min_val, int max_val) {
        step = 0;
        if (field == "*") {
            value = -1;
            return true;
        }
        // Handle */N
        if (field.starts_with("*/")) {
            value = -1;
            auto step_sv = field.substr(2);
            try {
                step = std::stoi(std::string(step_sv));
                return step > 0 && step <= max_val;
            } catch (...) {
                return false;
            }
        }
        // Single numeric value
        try {
            value = std::stoi(std::string(field));
            return value >= min_val && value <= max_val;
        } catch (...) {
            return false;
        }
    }

    inline std::string_view next_token(std::string_view& sv) {
        auto space = sv.find(' ');
        std::string_view token;
        if (space == std::string_view::npos) {
            token = sv;
            sv = {};
        } else {
            token = sv.substr(0, space);
            sv = sv.substr(space + 1);
        }
        return token;
    }
} // namespace detail

// Parse a 5-field cron expression
std::expected<CronExpression, std::string> parse_cron(std::string_view expr) {
    CronExpression cron{};

    // min_values and max_values for: minute, hour, day-of-month, month, day-of-week
    constexpr int mins[] = {0, 0, 1, 1, 0};
    constexpr int maxs[] = {59, 23, 31, 12, 6};

    std::string_view remaining = expr;
    for (int i = 0; i < 5; ++i) {
        auto token = detail::next_token(remaining);
        if (token.empty() && i < 5) {
            return std::unexpected("Cron expression requires 5 fields, got " + std::to_string(i));
        }
        if (!detail::parse_field(token, cron.values[i], cron.step_values[i], mins[i], maxs[i])) {
            return std::unexpected("Invalid cron field " + std::to_string(i) + ": " + std::string(token));
        }
    }

    cron.valid = true;
    return cron;
}

// Calculate the next run time after 'from'
std::chrono::system_clock::time_point next_run(
    const CronExpression& cron,
    std::chrono::system_clock::time_point from) {

    // Start from the next minute
    auto t = std::chrono::system_clock::to_time_t(from);
    std::tm tm{};
    localtime_r(&t, &tm);
    tm.tm_sec = 0;
    tm.tm_min += 1; // Start checking from next minute

    // Iterate up to one year forward to find a match
    for (int attempts = 0; attempts < 525600; ++attempts) { // 365 * 24 * 60
        std::mktime(&tm); // Normalize

        bool match = true;

        // Check minute
        if (cron.values[0] != -1 && tm.tm_min != cron.values[0]) match = false;
        if (cron.step_values[0] > 0 && tm.tm_min % cron.step_values[0] != 0) match = false;

        // Check hour
        if (cron.values[1] != -1 && tm.tm_hour != cron.values[1]) match = false;
        if (cron.step_values[1] > 0 && tm.tm_hour % cron.step_values[1] != 0) match = false;

        // Check day of month
        if (cron.values[2] != -1 && tm.tm_mday != cron.values[2]) match = false;

        // Check month (tm_mon is 0-based)
        if (cron.values[3] != -1 && (tm.tm_mon + 1) != cron.values[3]) match = false;

        // Check day of week
        if (cron.values[4] != -1 && tm.tm_wday != cron.values[4]) match = false;

        if (match) {
            return std::chrono::system_clock::from_time_t(std::mktime(&tm));
        }

        tm.tm_min += 1;
    }

    // Fallback: return from + 1 day
    return from + std::chrono::hours(24);
}

// Check if the cron expression matches the current time
bool matches_now(const CronExpression& cron) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);

    if (cron.values[0] != -1 && tm.tm_min != cron.values[0]) return false;
    if (cron.step_values[0] > 0 && tm.tm_min % cron.step_values[0] != 0) return false;
    if (cron.values[1] != -1 && tm.tm_hour != cron.values[1]) return false;
    if (cron.step_values[1] > 0 && tm.tm_hour % cron.step_values[1] != 0) return false;
    if (cron.values[2] != -1 && tm.tm_mday != cron.values[2]) return false;
    if (cron.values[3] != -1 && (tm.tm_mon + 1) != cron.values[3]) return false;
    if (cron.values[4] != -1 && tm.tm_wday != cron.values[4]) return false;

    return true;
}

// Serialize cron expression back to string
std::string to_string(const CronExpression& cron) {
    std::string result;
    for (int i = 0; i < 5; ++i) {
        if (i > 0) result += ' ';
        if (cron.values[i] == -1) {
            if (cron.step_values[i] > 0) {
                result += "*/" + std::to_string(cron.step_values[i]);
            } else {
                result += '*';
            }
        } else {
            result += std::to_string(cron.values[i]);
        }
    }
    return result;
}

} // namespace cc::utils
