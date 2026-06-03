// C++23 Cache Module
// Provides in-memory and file-based caching utilities
module;

#include <string>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <vector>
#include <list>

export module cc.utils.cache;

export namespace cc::utils::cache {


template<typename T>
struct CacheEntry {
    T value;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
    std::size_t access_count;
};


template<typename K, typename V>
class LRUCache {
public:
    using key_type = K;
    using value_type = V;
    using size_type = std::size_t;

    explicit LRUCache(size_type max_size = 1000) 
        : max_size_(max_size) {}


    void set(const K& key, const V& value) {

        auto map_it = cache_map_.find(key);
        if (map_it != cache_map_.end()) {

            access_order_.erase(map_it->second);
            cache_map_.erase(map_it);
        }


        if (cache_map_.size() >= max_size_) {

            const auto& oldest_key = access_order_.back();
            cache_map_.erase(oldest_key);
            access_order_.pop_back();
        }


        access_order_.push_front(key);
        CacheEntry<V> entry{
            value,
            std::chrono::steady_clock::now(),
            std::chrono::steady_clock::now(),
            1
        };
        cache_map_.insert({key, access_order_.begin()});
        entries_.insert({key, std::move(entry)});
    }


    std::optional<V> get(const K& key) {
        auto map_it = cache_map_.find(key);
        if (map_it == cache_map_.end()) {
            return std::nullopt;
        }


        auto entry_it = map_it->second;
        access_order_.erase(entry_it);
        access_order_.push_front(key);
        cache_map_[key] = access_order_.begin();


        auto& entry = entries_.at(key);
        entry.last_accessed = std::chrono::steady_clock::now();
        entry.access_count++;

        return entry.value;
    }


    bool has(const K& key) const {
        return cache_map_.count(key) > 0;
    }


    void remove(const K& key) {
        auto map_it = cache_map_.find(key);
        if (map_it != cache_map_.end()) {
            access_order_.erase(map_it->second);
            cache_map_.erase(map_it);
            entries_.erase(key);
        }
    }


    void clear() {
        cache_map_.clear();
        access_order_.clear();
        entries_.clear();
    }


    size_type size() const {
        return cache_map_.size();
    }


    size_type max_size() const {
        return max_size_;
    }


    void set_max_size(size_type new_size) {
        max_size_ = new_size;

        while (cache_map_.size() > max_size_) {
            const auto& oldest_key = access_order_.back();
            cache_map_.erase(oldest_key);
            entries_.erase(oldest_key);
            access_order_.pop_back();
        }
    }

private:
    size_type max_size_;
    std::list<K> access_order_;
    std::unordered_map<K, typename std::list<K>::iterator> cache_map_;
    std::unordered_map<K, CacheEntry<V>> entries_;
};


template<typename K, typename V>
class SimpleCache {
public:
    void set(const K& key, const V& value) {
        cache_[key] = value;
    }

    std::optional<V> get(const K& key) const {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool has(const K& key) const {
        return cache_.count(key) > 0;
    }

    void remove(const K& key) {
        cache_.erase(key);
    }

    void clear() {
        cache_.clear();
    }

    std::size_t size() const {
        return cache_.size();
    }

private:
    std::unordered_map<K, V> cache_;
};


template<typename K, typename V>
class TTLTimeCache {
public:
    using duration = std::chrono::steady_clock::duration;

    explicit TTLTimeCache(duration ttl) : ttl_(ttl) {}

    void set(const K& key, const V& value) {
        CacheEntry<V> entry{
            value,
            std::chrono::steady_clock::now(),
            std::chrono::steady_clock::now(),
            1
        };
        cache_[key] = std::move(entry);
    }

    std::optional<V> get(const K& key) {
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }


        auto now = std::chrono::steady_clock::now();
        if (now - it->second.created_at > ttl_) {
            cache_.erase(it);
            return std::nullopt;
        }


        it->second.last_accessed = now;
        it->second.access_count++;
        return it->second.value;
    }

    bool has(const K& key) {
        return get(key).has_value();
    }

    void remove(const K& key) {
        cache_.erase(key);
    }

    void clear() {
        cache_.clear();
    }


    std::size_t cleanup() {
        std::size_t removed = 0;
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (now - it->second.created_at > ttl_) {
                it = cache_.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
        
        return removed;
    }

    std::size_t size() const {
        return cache_.size();
    }

private:
    duration ttl_;
    std::unordered_map<K, CacheEntry<V>> cache_;
};

} // namespace cc::utils::cache
