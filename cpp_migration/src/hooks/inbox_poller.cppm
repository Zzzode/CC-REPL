// cc.hooks.inbox_poller — migrated from useInboxPoller.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <atomic>
#include <deque>
#include <algorithm>
#include <functional>

export module cc.hooks.inbox_poller;

export namespace cc::hooks::inbox_poller {

struct InboxItem {
    std::string id;
    std::string type;       // "message", "notification", "task_result", "system"
    std::string content;
    std::string source;     // originating agent or system
    std::chrono::system_clock::time_point received_at;
    bool read{false};
};

struct PollerConfig {
    std::chrono::seconds interval{30};
    int max_items{50};
    bool auto_start{true};
    std::string filter_type;  // empty means all types
};

using InboxCallback = std::function<void(const InboxItem&)>;

namespace detail {

struct PollerState {
    std::mutex mutex;
    std::deque<InboxItem> items;
    std::vector<InboxCallback> callbacks;
    PollerConfig config;
    std::atomic<bool> polling{false};
    std::atomic<std::size_t> unread_count{0};
    std::chrono::system_clock::time_point last_poll_time;
    std::uint64_t next_callback_id{1};
};

inline auto get_state() -> PollerState& {
    static PollerState state;
    return state;
}

} // namespace detail

/// Start the inbox polling loop with the given configuration.
/// In production: spawns a timer thread or registers with the event loop
/// to periodically fetch new inbox items from the bridge/server.
inline void start_polling(PollerConfig config = {}) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.config = config;
    state.polling.store(true);
    state.last_poll_time = std::chrono::system_clock::now();
    // In production: schedule first poll after interval
    // The actual HTTP fetch is handled by the bridge transport layer
}

/// Stop the active polling loop.
inline void stop_polling() {
    auto& state = detail::get_state();
    state.polling.store(false);
}

/// Check if the poller is currently active.
inline bool is_polling() {
    return detail::get_state().polling.load();
}

/// Get all inbox items (read and unread).
inline std::vector<InboxItem> get_inbox_items() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return {state.items.begin(), state.items.end()};
}

/// Get only unread inbox items.
inline std::vector<InboxItem> get_unread_items() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    std::vector<InboxItem> unread;
    for (const auto& item : state.items) {
        if (!item.read) unread.push_back(item);
    }
    return unread;
}

/// Get the count of unread items.
inline std::size_t get_unread_count() {
    return detail::get_state().unread_count.load();
}

/// Mark a specific item as read by ID.
inline void mark_item_read(std::string_view id) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    for (auto& item : state.items) {
        if (item.id == id && !item.read) {
            item.read = true;
            if (state.unread_count.load() > 0) {
                state.unread_count.fetch_sub(1);
            }
            break;
        }
    }
}

/// Mark all items as read.
inline void mark_all_read() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    for (auto& item : state.items) {
        item.read = true;
    }
    state.unread_count.store(0);
}

/// Push a new item into the inbox (called by the transport layer on new data).
inline void push_item(InboxItem item) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    // Apply type filter if configured
    if (!state.config.filter_type.empty() && item.type != state.config.filter_type) {
        return;
    }

    // Enforce max items limit (drop oldest)
    while (static_cast<int>(state.items.size()) >= state.config.max_items) {
        if (!state.items.front().read) {
            if (state.unread_count.load() > 0) state.unread_count.fetch_sub(1);
        }
        state.items.pop_front();
    }

    item.received_at = std::chrono::system_clock::now();
    state.unread_count.fetch_add(1);

    // Notify callbacks
    for (const auto& cb : state.callbacks) {
        cb(item);
    }

    state.items.push_back(std::move(item));
}

/// Register a callback for new inbox items. Returns a callback ID for removal.
inline std::uint64_t on_new_item(InboxCallback callback) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.callbacks.push_back(std::move(callback));
    return state.next_callback_id++;
}

/// Clear all inbox items.
inline void clear_inbox() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.items.clear();
    state.unread_count.store(0);
}

/// Get the time of the last successful poll.
inline std::chrono::system_clock::time_point get_last_poll_time() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.last_poll_time;
}

} // namespace cc::hooks::inbox_poller
