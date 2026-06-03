module;
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

export module cc.hooks.timeout_hook;

export namespace cc::hooks::timeout_hook {


struct TimeoutHandle {
    std::uint64_t id;
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> callback;
    bool cancelled;
    bool is_interval;
    std::chrono::milliseconds interval_ms{0};
};

namespace detail {

struct TimeoutRegistry {
    std::mutex mutex;
    std::vector<TimeoutHandle> handles;
    std::atomic<std::uint64_t> next_id{1};
};

inline auto get_registry() -> TimeoutRegistry& {
    static TimeoutRegistry registry;
    return registry;
}

} // namespace detail


inline std::uint64_t set_timeout(std::chrono::milliseconds delay,
                                 std::function<void()> callback) {
    auto& reg = detail::get_registry();
    auto id = reg.next_id.fetch_add(1);

    std::lock_guard lock(reg.mutex);
    reg.handles.push_back(TimeoutHandle{
        .id = id,
        .deadline = std::chrono::steady_clock::now() + delay,
        .callback = std::move(callback),
        .cancelled = false,
        .is_interval = false,
        .interval_ms = std::chrono::milliseconds{0},
    });
    return id;
}


inline void clear_timeout(std::uint64_t handle) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    for (auto& h : reg.handles) {
        if (h.id == handle) {
            h.cancelled = true;
            break;
        }
    }
}


inline std::uint64_t set_interval(std::chrono::milliseconds interval,
                                  std::function<void()> callback) {
    auto& reg = detail::get_registry();
    auto id = reg.next_id.fetch_add(1);

    std::lock_guard lock(reg.mutex);
    reg.handles.push_back(TimeoutHandle{
        .id = id,
        .deadline = std::chrono::steady_clock::now() + interval,
        .callback = std::move(callback),
        .cancelled = false,
        .is_interval = true,
        .interval_ms = interval,
    });
    return id;
}


inline void clear_interval(std::uint64_t handle) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    for (auto& h : reg.handles) {
        if (h.id == handle) {
            h.cancelled = true;
            break;
        }
    }
}


inline std::size_t get_active_timers_count() {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    std::size_t count = 0;
    auto now = std::chrono::steady_clock::now();
    for (const auto& h : reg.handles) {
        if (!h.cancelled && (h.is_interval || h.deadline > now)) {
            ++count;
        }
    }
    return count;
}

} // namespace cc::hooks::timeout_hook
