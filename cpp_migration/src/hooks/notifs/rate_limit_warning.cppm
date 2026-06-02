module;
#include <chrono>
#include <optional>
#include <string>
#include <format>

export module cc.hooks.notifs.rate_limit_warning;

export namespace cc::hooks::notifs {

// 速率限制信息
struct RateLimitInfo {
    int remaining;                  // 剩余请求数
    int total;                      // 总配额
    std::chrono::seconds reset_in;  // 重置倒计时
};

inline bool should_show_rate_limit_warning(RateLimitInfo info);
inline std::string format_rate_limit_notification(RateLimitInfo info);

// 检查是否需要显示速率限制警告，返回警告消息或空
inline std::optional<std::string> check_rate_limit_warning(RateLimitInfo info) {
    if (should_show_rate_limit_warning(info)) {
        return format_rate_limit_notification(info);
    }
    return std::nullopt;
}

// 判断是否应该展示速率限制警告（剩余量低于 10%）
inline bool should_show_rate_limit_warning(RateLimitInfo info) {
    if (info.total <= 0) return false;
    double ratio = static_cast<double>(info.remaining) / info.total;
    return ratio < 0.1;
}

// 格式化速率限制通知文本
inline std::string format_rate_limit_notification(RateLimitInfo info) {
    return std::format(
        "Rate limit warning: {}/{} requests remaining, resets in {}s",
        info.remaining, info.total, info.reset_in.count()
    );
}

} // namespace cc::hooks::notifs
