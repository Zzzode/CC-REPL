module;

#include <filesystem>
#include <string>
#include <optional>
#include <chrono>
#include <list>
#include <unordered_map>
#include <mutex>
#include <fstream>

export module cc.utils.file_read_cache;

namespace fs = std::filesystem;

export namespace cc::utils {


class FileReadCache {
public:
    explicit FileReadCache(size_t max_entries = 128,
                           std::chrono::seconds ttl = std::chrono::seconds{60})
        : max_entries_(max_entries), ttl_(ttl) {}


    std::optional<std::string> get(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(filepath.string());
        if (it == map_.end()) {
            return std::nullopt;
        }

        auto& entry = it->second->second;

        auto now = std::chrono::steady_clock::now();
        if (now - entry.timestamp > ttl_) {

            lru_list_.erase(it->second);
            map_.erase(it);
            return std::nullopt;
        }


        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return entry.content;
    }


    void put(const fs::path& filepath, std::string content) {
        std::lock_guard lock(mutex_);
        std::string key = filepath.string();

        auto it = map_.find(key);
        if (it != map_.end()) {

            it->second->second = CacheEntry{
                std::move(content),
                std::chrono::steady_clock::now()
            };
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }


        while (lru_list_.size() >= max_entries_) {
            auto last = std::prev(lru_list_.end());
            map_.erase(last->first);
            lru_list_.pop_back();
        }


        lru_list_.emplace_front(
            key,
            CacheEntry{std::move(content), std::chrono::steady_clock::now()}
        );
        map_[key] = lru_list_.begin();
    }


    void invalidate(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(filepath.string());
        if (it != map_.end()) {
            lru_list_.erase(it->second);
            map_.erase(it);
        }
    }


    void clear() {
        std::lock_guard lock(mutex_);
        lru_list_.clear();
        map_.clear();
    }


    void set_max_entries(size_t n) {
        std::lock_guard lock(mutex_);
        max_entries_ = n;

        while (lru_list_.size() > max_entries_) {
            auto last = std::prev(lru_list_.end());
            map_.erase(last->first);
            lru_list_.pop_back();
        }
    }


    void set_ttl(std::chrono::seconds new_ttl) {
        std::lock_guard lock(mutex_);
        ttl_ = new_ttl;
    }


    size_t size() const {
        std::lock_guard lock(mutex_);
        return lru_list_.size();
    }

private:
    struct CacheEntry {
        std::string content;
        std::chrono::steady_clock::time_point timestamp;
    };


    using ListType = std::list<std::pair<std::string, CacheEntry>>;
    ListType lru_list_;


    std::unordered_map<std::string, ListType::iterator> map_;

    size_t max_entries_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
};


inline FileReadCache& get_global_cache() {
    static FileReadCache instance(256, std::chrono::seconds{120});
    return instance;
}

} // namespace cc::utils
