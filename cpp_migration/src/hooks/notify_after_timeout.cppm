module;
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.hooks.notify_after_timeout;

export namespace cc::hooks::notify_after_timeout {

// 超时通知：任务超时后发送通知
struct TimeoutNotification {
    std::string task_id;
    std::string message;
    std::chrono::seconds timeout;
    std::chrono::steady_clock::time_point registered_at;
    bool fired;
};

namespace detail {

struct NotificationRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, TimeoutNotification> notifications;
};

inline auto get_registry() -> NotificationRegistry& {
    static NotificationRegistry registry;
    return registry;
}

} // namespace detail

// 为指定任务设置超时通知
inline void set_timeout_notification(std::string_view task_id,
                                     std::chrono::seconds timeout,
                                     std::string_view message) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    reg.notifications[std::string(task_id)] = TimeoutNotification{
        .task_id = std::string(task_id),
        .message = std::string(message),
        .timeout = timeout,
        .registered_at = std::chrono::steady_clock::now(),
        .fired = false,
    };
}

// 取消指定任务的超时通知
inline void cancel_timeout_notification(std::string_view task_id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    reg.notifications.erase(std::string(task_id));
}

// 检查所有已触发的超时通知并返回
inline std::vector<TimeoutNotification> check_timeouts() {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    std::vector<TimeoutNotification> fired;
    auto now = std::chrono::steady_clock::now();

    for (auto& [id, notif] : reg.notifications) {
        if (notif.fired) continue;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - notif.registered_at);
        if (elapsed >= notif.timeout) {
            notif.fired = true;
            fired.push_back(notif);
        }
    }

    return fired;
}

// 检查指定任务是否有挂起的超时通知
inline bool has_pending_timeout(std::string_view task_id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    auto it = reg.notifications.find(std::string(task_id));
    if (it == reg.notifications.end()) return false;
    return !it->second.fired;
}

} // namespace cc::hooks::notify_after_timeout
