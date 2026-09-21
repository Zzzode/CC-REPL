module;
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

export module cc.utils.lockfile;

export namespace cc::utils {

namespace fs = std::filesystem;

// RAII file-based advisory lock
class LockFile {
public:
    explicit LockFile(fs::path lock_path, std::chrono::seconds timeout = std::chrono::seconds(10))
        : path_(std::move(lock_path)), timeout_(timeout), fd_(-1), locked_(false) {
        try_lock();
    }

    ~LockFile() {
        if (locked_) unlock();
    }

    // Non-copyable
    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;

    // Movable
    LockFile(LockFile&& other) noexcept
        : path_(std::move(other.path_)), timeout_(other.timeout_),
          fd_(other.fd_), locked_(other.locked_) {
        other.fd_ = -1;
        other.locked_ = false;
    }

    LockFile& operator=(LockFile&& other) noexcept {
        if (this != &other) {
            if (locked_) unlock();
            path_ = std::move(other.path_);
            timeout_ = other.timeout_;
            fd_ = other.fd_;
            locked_ = other.locked_;
            other.fd_ = -1;
            other.locked_ = false;
        }
        return *this;
    }

    bool is_locked() const { return locked_; }

    bool try_lock() {
        if (locked_) return true;

        // Create parent directory if needed
        if (auto parent = path_.parent_path(); !parent.empty()) {
            fs::create_directories(parent);
        }

        fd_ = open(path_.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ < 0) return false;

        // Try non-blocking lock first
        if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            locked_ = true;
            // Write PID to lock file
            auto pid_str = std::to_string(getpid());
            if (ftruncate(fd_, 0) == 0) {
                [[maybe_unused]] auto written = write(fd_, pid_str.c_str(), pid_str.size());
            }
            return true;
        }

        // If non-blocking failed, retry with timeout
        auto deadline = std::chrono::steady_clock::now() + timeout_;
        while (std::chrono::steady_clock::now() < deadline) {
            usleep(100000); // 100ms
            if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                locked_ = true;
                auto pid_str = std::to_string(getpid());
                if (ftruncate(fd_, 0) == 0) {
                    [[maybe_unused]] auto written = write(fd_, pid_str.c_str(), pid_str.size());
                }
                return true;
            }
        }

        close(fd_);
        fd_ = -1;
        return false;
    }

    void unlock() {
        if (!locked_ || fd_ < 0) return;
        flock(fd_, LOCK_UN);
        close(fd_);
        fd_ = -1;
        locked_ = false;
        // Remove lock file
        std::error_code ec;
        fs::remove(path_, ec);
    }

private:
    fs::path path_;
    std::chrono::seconds timeout_;
    int fd_;
    bool locked_;
};

// Check if a file is currently locked (without acquiring the lock)
bool is_file_locked(fs::path lock_path) {
    if (!fs::exists(lock_path)) return false;

    int fd = open(lock_path.c_str(), O_RDWR);
    if (fd < 0) return false;

    // Try non-blocking lock
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        // We got the lock, so it wasn't locked
        flock(fd, LOCK_UN);
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

// Break a stale lock file older than max_age
bool break_stale_lock(fs::path lock_path, std::chrono::seconds max_age) {
    if (!fs::exists(lock_path)) return false;

    auto mod_time = fs::last_write_time(lock_path);
    auto now = fs::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - mod_time);

    if (age > max_age) {
        std::error_code ec;
        fs::remove(lock_path, ec);
        return !ec;
    }
    return false;
}

} // namespace cc::utils
