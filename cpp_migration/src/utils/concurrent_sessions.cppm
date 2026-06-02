module;
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

export module cc.utils.concurrent_sessions;

export namespace cc::utils {

namespace fs = std::filesystem;

namespace detail {
    inline fs::path get_sessions_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }

    inline fs::path lock_path_for(std::string_view session_id) {
        return get_sessions_dir() / std::string(session_id) / ".lock";
    }
} // namespace detail

// Simple RAII lock file for session locking
class SessionLock {
public:
    explicit SessionLock(int fd, fs::path path) : fd_(fd), path_(std::move(path)) {}
    ~SessionLock() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }
    SessionLock(SessionLock&& o) noexcept : fd_(o.fd_), path_(std::move(o.path_)) { o.fd_ = -1; }
    SessionLock& operator=(SessionLock&& o) noexcept {
        if (this != &o) { fd_ = o.fd_; path_ = std::move(o.path_); o.fd_ = -1; }
        return *this;
    }
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;
private:
    int fd_;
    fs::path path_;
};

// Get list of active (locked) session IDs
std::vector<std::string> get_active_sessions() {
    std::vector<std::string> active;
    auto base = detail::get_sessions_dir();
    if (!fs::exists(base)) return active;

    for (auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_directory()) continue;
        auto lock_file = entry.path() / ".lock";
        if (fs::exists(lock_file)) {
            // Try to acquire the lock - if we can't, it's active
            int fd = open(lock_file.c_str(), O_RDWR);
            if (fd >= 0) {
                if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
                    // Can't lock = session is active
                    active.push_back(entry.path().filename().string());
                } else {
                    flock(fd, LOCK_UN);
                }
                close(fd);
            }
        }
    }

    return active;
}

// Check if a specific session is locked
bool is_session_locked(std::string_view session_id) {
    auto lock_file = detail::lock_path_for(session_id);
    if (!fs::exists(lock_file)) return false;

    int fd = open(lock_file.c_str(), O_RDWR);
    if (fd < 0) return false;

    bool locked = (flock(fd, LOCK_EX | LOCK_NB) != 0);
    if (!locked) flock(fd, LOCK_UN);
    close(fd);
    return locked;
}

// Acquire an exclusive lock on a session
std::expected<SessionLock, std::string> acquire_session_lock(std::string_view session_id) {
    auto lock_file = detail::lock_path_for(session_id);

    // Ensure parent directory exists
    fs::create_directories(lock_file.parent_path());

    int fd = open(lock_file.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return std::unexpected("Failed to create lock file for session: " + std::string(session_id));
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return std::unexpected("Session is already locked: " + std::string(session_id));
    }

    return SessionLock(fd, lock_file);
}

// Get the maximum number of concurrent sessions allowed
std::size_t get_concurrent_session_limit() {
    const char* limit = std::getenv("CLAUDE_MAX_SESSIONS");
    if (limit) {
        try {
            int val = std::stoi(limit);
            if (val > 0) return static_cast<std::size_t>(val);
        } catch (...) {}
    }
    return 10; // Default limit
}

} // namespace cc::utils
