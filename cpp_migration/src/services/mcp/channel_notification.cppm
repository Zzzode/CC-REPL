module;
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
export module cc.services.mcp.channel_notification;

export namespace cc::services::mcp {

namespace detail {
    inline std::mutex notification_mutex;
    inline std::atomic<int> next_subscription_id{1};
    inline std::map<int, std::pair<std::string, std::function<void(std::string)>>> subscriptions;
} // namespace detail

// Notify all subscribers of a channel update event
auto notify_channel_update(std::string_view channel, std::string_view event) -> void {
    std::lock_guard lock(detail::notification_mutex);
    for (auto& [id, sub] : detail::subscriptions) {
        if (sub.first == channel) {
            sub.second(std::string(event));
        }
    }
}

// Subscribe to events on a specific channel
auto subscribe_channel_events(std::string_view channel, std::function<void(std::string)> callback)
    -> int {
    std::lock_guard lock(detail::notification_mutex);
    int id = detail::next_subscription_id.fetch_add(1);
    detail::subscriptions[id] = {std::string(channel), std::move(callback)};
    return id;
}

// Unsubscribe from channel events
auto unsubscribe(int id) -> void {
    std::lock_guard lock(detail::notification_mutex);
    detail::subscriptions.erase(id);
}

} // namespace cc::services::mcp
