module;

#include <filesystem>
#include <string>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <system_error>

export module cc.utils.file_state_cache;

namespace fs = std::filesystem;

export namespace cc::utils {

// 文件元信息结构体
struct FileStat {
    size_t size;
    std::chrono::system_clock::time_point mtime;
    bool is_directory;
    bool is_symlink;
};

// 文件状态缓存：缓存文件的 stat 信息以减少系统调用
class FileStatCache {
public:
    explicit FileStatCache() = default;

    // 获取文件状态信息，不存在或出错返回 nullopt
    std::optional<FileStat> stat(const fs::path& filepath) {
        std::lock_guard lock(mutex_);

        auto key = filepath.string();
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }

        // 缓存未命中，执行系统调用
        auto result = stat_impl(filepath);
        if (result) {
            cache_[key] = *result;
        }
        return result;
    }

    // 判断文件自指定时间点后是否被修改
    bool is_modified_since(
        const fs::path& filepath,
        std::chrono::system_clock::time_point since
    ) {
        auto file_stat = stat(filepath);
        if (!file_stat) {
            return true; // 无法读取状态，假设已修改
        }
        return file_stat->mtime > since;
    }

    // 使指定路径的缓存失效
    void invalidate(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        cache_.erase(filepath.string());
    }

    // 清空所有缓存
    void clear() {
        std::lock_guard lock(mutex_);
        cache_.clear();
    }

    // 刷新指定路径的缓存（重新读取）
    std::optional<FileStat> refresh(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        auto key = filepath.string();
        auto result = stat_impl(filepath);
        if (result) {
            cache_[key] = *result;
        } else {
            cache_.erase(key);
        }
        return result;
    }

    // 缓存条目数
    size_t size() const {
        std::lock_guard lock(mutex_);
        return cache_.size();
    }

private:
    // 实际执行 stat 系统调用
    static std::optional<FileStat> stat_impl(const fs::path& filepath) {
        std::error_code ec;

        auto status = fs::status(filepath, ec);
        if (ec || status.type() == fs::file_type::not_found) {
            return std::nullopt;
        }

        auto symlink_status = fs::symlink_status(filepath, ec);
        bool is_symlink = (symlink_status.type() == fs::file_type::symlink);

        size_t file_size = 0;
        if (status.type() == fs::file_type::regular) {
            file_size = static_cast<size_t>(fs::file_size(filepath, ec));
            if (ec) file_size = 0;
        }

        auto ftime = fs::last_write_time(filepath, ec);
        std::chrono::system_clock::time_point mtime{};
        if (!ec) {
            // Convert file_time_type to system_clock via duration
            auto d = ftime.time_since_epoch();
            mtime = std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(d)
            };
        }

        return FileStat{
            .size = file_size,
            .mtime = mtime,
            .is_directory = (status.type() == fs::file_type::directory),
            .is_symlink = is_symlink
        };
    }

    std::unordered_map<std::string, FileStat> cache_;
    mutable std::mutex mutex_;
};

} // namespace cc::utils
