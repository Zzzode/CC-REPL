module;

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.cleanup_registry;

export namespace cc::utils::cleanup_registry {

using CleanupFn = std::function<void()>;

struct CleanupEntry {
    uint64_t id;
    std::string description;
    CleanupFn handler;
    int priority{0};
};

namespace detail {
    inline std::vector<CleanupEntry>& entries() {
        static std::vector<CleanupEntry> s_entries;
        return s_entries;
    }
    inline std::mutex& mutex() {
        static std::mutex s_mutex;
        return s_mutex;
    }
    inline std::atomic<uint64_t>& next_id() {
        static std::atomic<uint64_t> s_next_id{1};
        return s_next_id;
    }
}

inline uint64_t register_cleanup(std::string_view description, CleanupFn handler, int priority = 0) {
    std::lock_guard lock(detail::mutex());
    auto id = detail::next_id().fetch_add(1);
    detail::entries().push_back(CleanupEntry{id, std::string(description), std::move(handler), priority});
    return id;
}

inline void unregister_cleanup(uint64_t id) {
    std::lock_guard lock(detail::mutex());
    auto& entries = detail::entries();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [id](const CleanupEntry& e) { return e.id == id; }),
        entries.end());
}

inline void run_all_cleanups() {
    std::vector<CleanupEntry> snapshot;
    {
        std::lock_guard lock(detail::mutex());
        snapshot = std::move(detail::entries());
        detail::entries().clear();
    }
    // Run in reverse registration order (LIFO)
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        if (it->handler) it->handler();
    }
}

inline void run_cleanups_by_priority(int min_priority) {
    std::vector<CleanupEntry> to_run;
    {
        std::lock_guard lock(detail::mutex());
        auto& entries = detail::entries();
        auto it = std::partition(entries.begin(), entries.end(),
                                 [min_priority](const CleanupEntry& e) { return e.priority < min_priority; });
        to_run.assign(it, entries.end());
        entries.erase(it, entries.end());
    }
    // Sort by priority descending, then run
    std::sort(to_run.begin(), to_run.end(),
              [](const CleanupEntry& a, const CleanupEntry& b) { return a.priority > b.priority; });
    for (auto& entry : to_run) {
        if (entry.handler) entry.handler();
    }
}

inline size_t pending_cleanup_count() {
    std::lock_guard lock(detail::mutex());
    return detail::entries().size();
}

} // namespace cc::utils::cleanup_registry
