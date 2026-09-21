module;
#include <chrono>
#include <optional>
#include <string>
#include <format>

export module cc.hooks.notifs.rate_limit_warning;

export namespace cc::hooks::notifs {


struct RateLimitInfo {
    int remaining;
    int total;
    std::chrono::seconds reset_in;
};

inline bool should_show_rate_limit_warning(RateLimitInfo info);
inline std::string format_rate_limit_notification(RateLimitInfo info);


inline std::optional<std::string> check_rate_limit_warning(RateLimitInfo info) {
    if (should_show_rate_limit_warning(info)) {
        return format_rate_limit_notification(info);
    }
    return std::nullopt;
}


inline bool should_show_rate_limit_warning(RateLimitInfo info) {
    if (info.total <= 0) return false;
    double ratio = static_cast<double>(info.remaining) / info.total;
    return ratio < 0.1;
}


inline std::string format_rate_limit_notification(RateLimitInfo info) {
    return std::format(
        "Rate limit warning: {}/{} requests remaining, resets in {}s",
        info.remaining, info.total, info.reset_in.count()
    );
}

} // namespace cc::hooks::notifs
