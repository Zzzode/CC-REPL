module;

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <memory>
#include <regex>
#include <thread>
#include <atomic>
#include <chrono>
#include <system_error>

#if defined(__APPLE__)
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#endif

export module cc.utils.fs_operations;

namespace fs = std::filesystem;

export namespace cc::utils {

// 递归复制目录
inline std::expected<void, std::string> copy_recursive(
    const fs::path& src,
    const fs::path& dst
) {
    std::error_code ec;

    if (!fs::exists(src, ec)) {
        return std::unexpected("Source not found: " + src.string());
    }

    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected("Copy failed: " + ec.message());
    }
    return {};
}

// 递归删除目录/文件，返回删除的文件数
inline std::expected<size_t, std::string> remove_recursive(const fs::path& target) {
    std::error_code ec;

    if (!fs::exists(target, ec)) {
        return 0;
    }

    // 先计算文件数再删除
    size_t count = 0;
    if (fs::is_directory(target, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(target, ec)) {
            if (entry.is_regular_file(ec)) {
                ++count;
            }
        }
    } else {
        count = 1;
    }

    auto removed = fs::remove_all(target, ec);
    if (ec) {
        return std::unexpected("Remove failed: " + ec.message());
    }

    return count;
}

// 计算目录总大小（字节）
inline size_t directory_size(const fs::path& dir) {
    size_t total = 0;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        // 如果是文件，直接返回文件大小
        if (fs::is_regular_file(dir, ec)) {
            return static_cast<size_t>(fs::file_size(dir, ec));
        }
        return 0;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            auto size = entry.file_size(ec);
            if (!ec) {
                total += static_cast<size_t>(size);
            }
        }
    }
    return total;
}

// 递归列出所有文件，可选 glob 模式过滤
inline std::vector<fs::path> list_files_recursive(
    const fs::path& dir,
    std::optional<std::string> pattern = std::nullopt
) {
    std::vector<fs::path> results;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        return results;
    }

    // 如果提供了模式，将 glob 转为正则
    std::optional<std::regex> regex_pattern;
    if (pattern) {
        std::string regex_str;
        for (char c : *pattern) {
            switch (c) {
                case '*': regex_str += ".*"; break;
                case '?': regex_str += "."; break;
                case '.': regex_str += "\\."; break;
                default: regex_str.push_back(c); break;
            }
        }
        regex_pattern = std::regex(regex_str);
    }

    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        if (regex_pattern) {
            auto filename = entry.path().filename().string();
            if (!std::regex_match(filename, *regex_pattern)) {
                continue;
            }
        }

        results.push_back(entry.path());
    }

    return results;
}

// 按扩展名查找文件
inline std::vector<fs::path> find_files_by_extension(
    const fs::path& dir,
    std::string_view ext
) {
    std::vector<fs::path> results;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        return results;
    }

    // 确保扩展名以 . 开头
    std::string target_ext;
    if (!ext.empty() && ext[0] != '.') {
        target_ext = '.';
    }
    target_ext += ext;

    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        if (entry.path().extension().string() == target_ext) {
            results.push_back(entry.path());
        }
    }

    return results;
}

// 文件监视器接口
class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

// 基于 kqueue (macOS) / inotify (Linux) 的文件监视器实现
class PlatformFileWatcher : public FileWatcher {
public:
    PlatformFileWatcher(const fs::path& target, std::function<void(fs::path)> callback)
        : target_(target), callback_(std::move(callback)), running_(true) {
        watcher_thread_ = std::thread([this]() { run(); });
    }

    ~PlatformFileWatcher() override {
        stop();
    }

    void stop() override {
        if (running_.exchange(false)) {
            if (watcher_thread_.joinable()) {
                watcher_thread_.join();
            }
        }
    }

    bool is_running() const override {
        return running_.load();
    }

private:
    void run() {
#if defined(__APPLE__)
        // macOS: 使用 kqueue 监视文件变更
        int kq = kqueue();
        if (kq == -1) {
            running_ = false;
            return;
        }

        int fd = open(target_.c_str(), O_EVTONLY);
        if (fd == -1) {
            close(kq);
            running_ = false;
            return;
        }

        struct kevent change;
        EV_SET(&change, fd, EVFILT_VNODE,
               EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB,
               0, nullptr);

        while (running_.load()) {
            struct timespec timeout = {1, 0}; // 1 秒超时用于检查 running_ 标志
            struct kevent event;
            int n = kevent(kq, &change, 1, &event, 1, &timeout);
            if (n > 0 && running_.load()) {
                callback_(target_);
            }
        }

        close(fd);
        close(kq);

#elif defined(__linux__)
        // Linux: 使用 inotify 监视文件变更
        int ifd = inotify_init1(IN_NONBLOCK);
        if (ifd == -1) {
            running_ = false;
            return;
        }

        int wd = inotify_add_watch(ifd, target_.c_str(),
                                    IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
        if (wd == -1) {
            close(ifd);
            running_ = false;
            return;
        }

        struct pollfd pfd = {ifd, POLLIN, 0};

        while (running_.load()) {
            int ret = poll(&pfd, 1, 1000); // 1 秒超时
            if (ret > 0 && (pfd.revents & POLLIN)) {
                // 读取并丢弃事件数据
                char buf[4096];
                while (read(ifd, buf, sizeof(buf)) > 0) {}
                if (running_.load()) {
                    callback_(target_);
                }
            }
        }

        inotify_rm_watch(ifd, wd);
        close(ifd);
#else
        // 无平台支持时使用轮询方式
        auto last_mtime = fs::last_write_time(target_);
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::error_code ec;
            auto current_mtime = fs::last_write_time(target_, ec);
            if (!ec && current_mtime != last_mtime) {
                last_mtime = current_mtime;
                if (running_.load()) {
                    callback_(target_);
                }
            }
        }
#endif
    }

    fs::path target_;
    std::function<void(fs::path)> callback_;
    std::atomic<bool> running_;
    std::thread watcher_thread_;
};

// 创建文件监视器
inline std::unique_ptr<FileWatcher> watch_file(
    const fs::path& target,
    std::function<void(fs::path)> callback
) {
    return std::make_unique<PlatformFileWatcher>(target, std::move(callback));
}

} // namespace cc::utils
