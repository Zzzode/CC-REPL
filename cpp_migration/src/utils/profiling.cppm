module;

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <optional>
#include <cstdint>

export module cc.utils.profiling;

export namespace cc::utils::profiling {

struct ProfileMark {
    std::string name;
    std::chrono::steady_clock::time_point timestamp;
    std::optional<std::string> metadata;
};

struct ProfileReport {
    std::string name;
    std::chrono::milliseconds total_duration;
    std::vector<ProfileMark> marks;
};

inline void mark_startup([[maybe_unused]] std::string_view label) {}

inline void mark_query([[maybe_unused]] std::string_view label) {}

inline void mark_headless([[maybe_unused]] std::string_view label) {}

inline ProfileReport get_startup_profile() {
    return {"startup", std::chrono::milliseconds{0}, {}};
}

inline ProfileReport get_query_profile() {
    return {"query", std::chrono::milliseconds{0}, {}};
}

inline std::string format_profile_report(const ProfileReport& report) {
    return report.name + ": " + std::to_string(report.total_duration.count()) + "ms";
}

} // namespace cc::utils::profiling
