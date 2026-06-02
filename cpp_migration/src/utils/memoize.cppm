module;
#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

export module cc.utils.memoize;

export namespace cc::utils {

// LRU cache with configurable capacity
template<typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(std::size_t capacity = 128) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        std::shared_lock lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;

        // Move to front (need exclusive lock)
        lock.unlock();
        std::unique_lock wlock(mutex_);
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    void put(const Key& key, Value value) {
        std::unique_lock lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }

        // Evict if at capacity
        if (order_.size() >= capacity_) {
            auto last = order_.back();
            map_.erase(last.first);
            order_.pop_back();
        }

        // Insert new
        order_.emplace_front(key, std::move(value));
        map_[key] = order_.begin();
    }

private:
    std::size_t capacity_;
    std::list<std::pair<Key, Value>> order_;
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator> map_;
    mutable std::shared_mutex mutex_;
};

// Memoize a function with LRU cache (thread-safe)
template<typename F>
auto memoize(F func, std::size_t capacity = 128) {
    using ArgTuple = decltype(std::apply(
        [](auto&&... args) { return std::make_tuple(args...); },
        std::declval<typename decltype(std::function{func})::argument_type>()
    ));

    // Simplified: memoize single-argument functions
    // For the general case, we use a wrapper
    struct MemoizedState {
        F fn;
        LRUCache<std::size_t, decltype(func(std::declval<typename decltype(std::function{func})::argument_type>()))> cache;
        MemoizedState(F f, std::size_t cap) : fn(std::move(f)), cache(cap) {}
    };

    auto state = std::make_shared<MemoizedState>(std::move(func), capacity);

    return [state](auto&&... args) {
        // Use hash of arguments as key
        std::size_t key = 0;
        ((key ^= std::hash<std::decay_t<decltype(args)>>{}(args) + 0x9e3779b9 + (key << 6) + (key >> 2)), ...);

        if (auto cached = state->cache.get(key)) {
            return *cached;
        }

        auto result = state->fn(std::forward<decltype(args)>(args)...);
        state->cache.put(key, result);
        return result;
    };
}

// Memoize with TTL (time-to-live)
template<typename F>
auto memoize_with_ttl(F func, std::chrono::seconds ttl, std::size_t capacity = 128) {
    using Clock = std::chrono::steady_clock;

    struct CachedValue {
        decltype(func(std::declval<typename decltype(std::function{func})::argument_type>())) value;
        Clock::time_point expires_at;
    };

    auto cache = std::make_shared<LRUCache<std::size_t, CachedValue>>(capacity);
    auto fn = std::make_shared<F>(std::move(func));
    auto ttl_val = ttl;

    return [cache, fn, ttl_val](auto&&... args) {
        std::size_t key = 0;
        ((key ^= std::hash<std::decay_t<decltype(args)>>{}(args) + 0x9e3779b9 + (key << 6) + (key >> 2)), ...);

        if (auto cached = cache->get(key)) {
            if (Clock::now() < cached->expires_at) {
                return cached->value;
            }
        }

        auto result = (*fn)(std::forward<decltype(args)>(args)...);
        cache->put(key, CachedValue{result, Clock::now() + ttl_val});
        return result;
    };
}

} // namespace cc::utils
