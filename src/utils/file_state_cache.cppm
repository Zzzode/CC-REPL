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


struct FileStat {
    size_t size;
    std::chrono::system_clock::time_point mtime;
    bool is_directory;
    bool is_symlink;
};


class FileStatCache {
public:
    explicit FileStatCache() = default;


    std::optional<FileStat> stat(const fs::path& filepath) {
        std::lock_guard lock(mutex_);

        auto key = filepath.string();
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }


        auto result = stat_impl(filepath);
        if (result) {
            cache_[key] = *result;
        }
        return result;
    }


    bool is_modified_since(
        const fs::path& filepath,
        std::chrono::system_clock::time_point since
    ) {
        auto file_stat = stat(filepath);
        if (!file_stat) {
            return true;
        }
        return file_stat->mtime > since;
    }


    void invalidate(const fs::path& filepath) {
        std::lock_guard lock(mutex_);
        cache_.erase(filepath.string());
    }


    void clear() {
        std::lock_guard lock(mutex_);
        cache_.clear();
    }


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


    size_t size() const {
        std::lock_guard lock(mutex_);
        return cache_.size();
    }

private:

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
