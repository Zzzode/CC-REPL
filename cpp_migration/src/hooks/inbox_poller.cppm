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
#include <expected>
#include <optional>
#include <thread>
#include <utility>

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
using InboxFetcher = std::function<std::expected<std::vector<InboxItem>, std::string>()>;

namespace detail {

struct PollerState {
    std::mutex mutex;
    std::deque<InboxItem> items;
    std::vector<InboxCallback> callbacks;
    InboxFetcher fetcher;
    PollerConfig config;
    std::atomic<bool> polling{false};
    std::atomic<std::size_t> unread_count{0};
    std::chrono::system_clock::time_point last_poll_time;
    std::optional<std::string> last_error;
    std::uint64_t next_callback_id{1};
    std::thread worker;
    std::atomic<bool> worker_stop{false};
};

inline auto get_state() -> PollerState& {
    static PollerState state;
    return state;
}

} // namespace detail

inline void push_item(InboxItem item);

/// Start the inbox polling loop with the given configuration.
inline void start_polling(PollerConfig config = {}) {
    auto& state = detail::get_state();
    std::thread previous_worker;
    bool previous_stop = false;
    {
        std::lock_guard lock(state.mutex);
        state.polling.store(false);
        state.worker_stop.store(true);
        previous_worker = std::move(state.worker);
        previous_stop = true;
    }
    if (previous_worker.joinable()) {
        previous_worker.join();
    }

    {
        std::lock_guard lock(state.mutex);
        state.config = config;
        state.polling.store(true);
        state.last_poll_time = std::chrono::system_clock::now();
        state.last_error.reset();
    }

    std::thread worker([&state]() {
        while (!state.worker_stop.load(std::memory_order_acquire)) {
            PollerConfig current_config;
            InboxFetcher fetcher;
            {
                auto& current_state = detail::get_state();
                std::lock_guard lock(current_state.mutex);
                if (!current_state.polling.load()) break;
                current_config = current_state.config;
                fetcher = current_state.fetcher;
            }

            auto interval = current_config.interval;
            if (interval <= std::chrono::seconds::zero()) {
                interval = std::chrono::seconds{30};
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
            while (remaining > std::chrono::milliseconds::zero() && !state.worker_stop.load(std::memory_order_acquire)) {
                const auto slice = std::min(remaining, std::chrono::milliseconds{100});
                std::this_thread::sleep_for(slice);
                remaining -= slice;
            }
            if (state.worker_stop.load(std::memory_order_acquire)) break;

            if (!fetcher) {
                auto& current_state = detail::get_state();
                std::lock_guard lock(current_state.mutex);
                current_state.last_poll_time = std::chrono::system_clock::now();
                continue;
            }

            auto fetched = fetcher();
            {
                auto& current_state = detail::get_state();
                std::lock_guard lock(current_state.mutex);
                current_state.last_poll_time = std::chrono::system_clock::now();
                if (!fetched) {
                    current_state.last_error = fetched.error();
                } else {
                    current_state.last_error.reset();
                }
            }

            if (fetched) {
                for (auto& item : *fetched) {
                    push_item(std::move(item));
                }
            }
        }
    });

    {
        std::lock_guard lock(state.mutex);
        state.worker = std::move(worker);
    }
}

/// Stop the active polling loop.
inline void stop_polling() {
    auto& state = detail::get_state();
    std::thread worker;
    {
        std::lock_guard lock(state.mutex);
        state.polling.store(false);
        state.worker_stop.store(true);
        worker = std::move(state.worker);
    }
    if (worker.joinable()) {
        worker.join();
    }
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
    std::vector<InboxCallback> callbacks;

    {
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
        state.items.push_back(item);
        callbacks = state.callbacks;
    }

    // Notify callbacks
    for (const auto& cb : callbacks) {
        cb(item);
    }
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

/// Set the fetcher used by the polling loop.
inline void set_inbox_fetcher(InboxFetcher fetcher) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.fetcher = std::move(fetcher);
}

/// Get the last poll error, if the configured fetcher failed.
inline std::optional<std::string> get_last_poll_error() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.last_error;
}

/// Get the time of the last successful poll.
inline std::chrono::system_clock::time_point get_last_poll_time() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.last_poll_time;
}

} // namespace cc::hooks::inbox_poller
