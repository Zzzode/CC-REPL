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

// 带 TTL 的 LRU 文件读取缓存
class FileReadCache {
public:
    explicit FileReadCache(size_t max_entries = 128,
                           std::chrono::seconds ttl = std::chrono::seconds{60})
        : max_entries_(max_entries), ttl_(ttl) {}

    // 从缓存获取文件内容，过期或不存在返回 nullopt
    std::optional<std::string> get(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(filepath.string());
        if (it == map_.end()) {
            return std::nullopt;
        }

        auto& entry = it->second->second;
        // 检查 TTL 是否过期
        auto now = std::chrono::steady_clock::now();
        if (now - entry.timestamp > ttl_) {
            // 已过期，移除
            lru_list_.erase(it->second);
            map_.erase(it);
            return std::nullopt;
        }

        // 移到 LRU 列表头部（最近使用）
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return entry.content;
    }

    // 将文件内容放入缓存
    void put(const fs::path& filepath, std::string content) {
        std::lock_guard lock(mutex_);
        std::string key = filepath.string();

        auto it = map_.find(key);
        if (it != map_.end()) {
            // 更新已有条目
            it->second->second = CacheEntry{
                std::move(content),
                std::chrono::steady_clock::now()
            };
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        // 驱逐最久未使用的条目（如果满了）
        while (lru_list_.size() >= max_entries_) {
            auto last = std::prev(lru_list_.end());
            map_.erase(last->first);
            lru_list_.pop_back();
        }

        // 插入新条目到头部
        lru_list_.emplace_front(
            key,
            CacheEntry{std::move(content), std::chrono::steady_clock::now()}
        );
        map_[key] = lru_list_.begin();
    }

    // 使指定路径的缓存失效
    void invalidate(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(filepath.string());
        if (it != map_.end()) {
            lru_list_.erase(it->second);
            map_.erase(it);
        }
    }

    // 清空所有缓存
    void clear() {
        std::lock_guard lock(mutex_);
        lru_list_.clear();
        map_.clear();
    }

    // 设置最大条目数
    void set_max_entries(size_t n) {
        std::lock_guard lock(mutex_);
        max_entries_ = n;
        // 驱逐多余条目
        while (lru_list_.size() > max_entries_) {
            auto last = std::prev(lru_list_.end());
            map_.erase(last->first);
            lru_list_.pop_back();
        }
    }

    // 设置 TTL
    void set_ttl(std::chrono::seconds new_ttl) {
        std::lock_guard lock(mutex_);
        ttl_ = new_ttl;
    }

    // 当前缓存条目数量
    size_t size() const {
        std::lock_guard lock(mutex_);
        return lru_list_.size();
    }

private:
    struct CacheEntry {
        std::string content;
        std::chrono::steady_clock::time_point timestamp;
    };

    // LRU 列表：头部是最近使用的，尾部是最久未使用的
    using ListType = std::list<std::pair<std::string, CacheEntry>>;
    ListType lru_list_;

    // 路径 → LRU 列表迭代器的映射，用于 O(1) 查找
    std::unordered_map<std::string, ListType::iterator> map_;

    size_t max_entries_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
};

// 全局单例缓存
inline FileReadCache& get_global_cache() {
    static FileReadCache instance(256, std::chrono::seconds{120});
    return instance;
}

} // namespace cc::utils
