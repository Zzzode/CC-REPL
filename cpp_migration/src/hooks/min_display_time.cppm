module;
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

export module cc.hooks.min_display_time;

export namespace cc::hooks::min_display_time {

// 显示计时器：确保消息的最短显示时间
struct DisplayTimer {
    std::string id;
    std::chrono::steady_clock::time_point start;
    std::chrono::milliseconds min_duration;
    bool expired;
};

namespace detail {

struct DisplayTimerRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, DisplayTimer> timers;
};

inline auto get_registry() -> DisplayTimerRegistry& {
    static DisplayTimerRegistry registry;
    return registry;
}

} // namespace detail

// 启动一个显示计时器，指定最短显示时长
inline void start_display_timer(std::string_view id,
                                std::chrono::milliseconds min_duration) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    reg.timers[std::string(id)] = DisplayTimer{
        .id = std::string(id),
        .start = std::chrono::steady_clock::now(),
        .min_duration = min_duration,
        .expired = false,
    };
}

// 检查指定 ID 的显示计时器是否已过期
inline bool is_display_expired(std::string_view id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    auto it = reg.timers.find(std::string(id));
    if (it == reg.timers.end()) return true;

    auto elapsed = std::chrono::steady_clock::now() - it->second.start;
    if (elapsed >= it->second.min_duration) {
        it->second.expired = true;
        return true;
    }
    return false;
}

// 获取指定 ID 计时器的剩余时间
inline std::chrono::milliseconds get_remaining_time(std::string_view id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    auto it = reg.timers.find(std::string(id));
    if (it == reg.timers.end()) return std::chrono::milliseconds{0};

    auto elapsed = std::chrono::steady_clock::now() - it->second.start;
    auto remaining = it->second.min_duration -
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    return remaining.count() > 0 ? remaining : std::chrono::milliseconds{0};
}

// 取消指定 ID 的显示计时器
inline void cancel_display_timer(std::string_view id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    reg.timers.erase(std::string(id));
}

// 阻塞等待直到指定 ID 的显示计时器过期
inline void wait_for_display(std::string_view id) {
    auto remaining = get_remaining_time(id);
    if (remaining.count() > 0) {
        std::this_thread::sleep_for(remaining);
    }
}

} // namespace cc::hooks::min_display_time
