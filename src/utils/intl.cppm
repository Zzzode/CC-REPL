module;
#include <string>
#include <string_view>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <ctime>
#include <array>
#include <iomanip>
#include <sstream>

export module cc.utils.intl;

export namespace cc::utils {


[[nodiscard]] inline std::string format_number(int64_t value) {
    bool negative = value < 0;
    uint64_t abs_val = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

    std::string digits = std::to_string(abs_val);
    std::string result;
    result.reserve(digits.size() + digits.size() / 3 + 1);

    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count > 0 && count % 3 == 0) {
            result += ',';
        }
        result += *it;
        ++count;
    }
    if (negative) result += '-';


    std::reverse(result.begin(), result.end());
    return result;
}


[[nodiscard]] inline std::string format_bytes(size_t bytes) {
    constexpr std::array<const char*, 6> units = {"B", "KB", "MB", "GB", "TB", "PB"};
    if (bytes == 0) return "0 B";

    double size = static_cast<double>(bytes);
    size_t unit_idx = 0;

    while (size >= 1024.0 && unit_idx < units.size() - 1) {
        size /= 1024.0;
        ++unit_idx;
    }


    std::ostringstream oss;
    if (unit_idx == 0) {
        oss << bytes << " " << units[unit_idx];
    } else if (size < 10.0) {
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit_idx];
    } else if (size < 100.0) {
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit_idx];
    } else {
        oss << static_cast<int64_t>(std::round(size)) << " " << units[unit_idx];
    }
    return oss.str();
}


[[nodiscard]] inline std::string format_duration(std::chrono::milliseconds ms) {
    using namespace std::chrono;

    if (ms.count() < 0) return "0ms";

    auto total_ms = ms.count();
    if (total_ms < 1000) {
        return std::to_string(total_ms) + "ms";
    }

    auto total_sec = total_ms / 1000;
    if (total_sec < 60) {
        auto remaining_ms = total_ms % 1000;
        if (remaining_ms > 0) {
            return std::to_string(total_sec) + "." + std::to_string(remaining_ms / 100) + "s";
        }
        return std::to_string(total_sec) + "s";
    }

    auto minutes = total_sec / 60;
    auto seconds = total_sec % 60;
    if (minutes < 60) {
        std::string result = std::to_string(minutes) + "m";
        if (seconds > 0) result += " " + std::to_string(seconds) + "s";
        return result;
    }

    auto hours = minutes / 60;
    minutes %= 60;
    if (hours < 24) {
        std::string result = std::to_string(hours) + "h";
        if (minutes > 0) result += " " + std::to_string(minutes) + "m";
        return result;
    }

    auto days = hours / 24;
    hours %= 24;
    std::string result = std::to_string(days) + "d";
    if (hours > 0) result += " " + std::to_string(hours) + "h";
    return result;
}


[[nodiscard]] inline std::string format_relative_time(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto diff = duration_cast<seconds>(now - tp);
    bool past = diff.count() >= 0;
    auto abs_seconds = past ? diff.count() : -diff.count();

    auto format = [past](int64_t value, const char* unit) -> std::string {
        std::string s = std::to_string(value) + " " + unit;
        if (value != 1) s += "s";
        return past ? (s + " ago") : ("in " + s);
    };

    if (abs_seconds < 5) return "just now";
    if (abs_seconds < 60) return format(abs_seconds, "second");
    if (abs_seconds < 3600) return format(abs_seconds / 60, "minute");
    if (abs_seconds < 86400) return format(abs_seconds / 3600, "hour");
    if (abs_seconds < 2592000) return format(abs_seconds / 86400, "day");
    if (abs_seconds < 31536000) return format(abs_seconds / 2592000, "month");
    return format(abs_seconds / 31536000, "year");
}


[[nodiscard]] inline std::string format_date(std::chrono::system_clock::time_point tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_val{};
    gmtime_r(&time_t_val, &tm_val);

    std::ostringstream oss;
    oss << std::setfill('0')
        << (tm_val.tm_year + 1900) << '-'
        << std::setw(2) << (tm_val.tm_mon + 1) << '-'
        << std::setw(2) << tm_val.tm_mday;
    return oss.str();
}

} // namespace cc::utils
