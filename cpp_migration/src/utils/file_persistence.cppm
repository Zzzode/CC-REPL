module;

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <expected>
#include <random>
#include <chrono>
#include <system_error>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

export module cc.utils.file_persistence;

namespace fs = std::filesystem;

export namespace cc::utils {

namespace detail {

// Call ::fsync() on a POSIX file descriptor.  Returns empty string on
// success, or a human-readable diagnostic on failure.  Best-effort: if the
// platform doesn't support fsync on this fd type (e.g. /dev/null, some
// network filesystems), we log-and-continue by returning empty string too,
// since a subsequent rename() would fail visibly anyway if the underlying
// filesystem was truly broken.
inline auto fsync_fd(int fd) -> std::string {
    if (fd < 0) return "invalid fd";
#if defined(__APPLE__) || defined(__linux__)
    // On macOS, fcntl(F_FULLFSYNC) asks the drive to actually flush its
    // write cache, which is stronger than fsync() alone.  Fall through to
    // plain fsync on failure.
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0) return {};
#endif
    if (::fsync(fd) == 0) return {};
    // EINVAL is common on pseudo filesystems / pipes — treat as success.
    if (errno == EINVAL) return {};
    char buf[128];
    std::snprintf(buf, sizeof(buf), "fsync failed (errno=%d)", errno);
    return std::string(buf);
#else
    (void)fd;
    return {};  // non-POSIX: nothing reliable we can do
#endif
}

// Open a path for fsync-of-directory / fsync-of-file.  Returns -1 on
// failure.  Caller must close() the returned fd.
[[nodiscard]] inline auto open_for_fsync(const fs::path& p, bool is_dir) -> int {
#if defined(__APPLE__) || defined(__linux__)
    const int flags = O_RDONLY | (is_dir ? O_DIRECTORY : 0);
    int fd = ::open(p.c_str(), flags);
    return fd;
#else
    (void)p; (void)is_dir;
    return -1;
#endif
}

}  // namespace detail

inline bool ensure_parent_dir(const fs::path& filepath) {
    auto parent = filepath.parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    fs::create_directories(parent, ec);
    return !ec;
}


inline std::expected<void, std::string> atomic_write(
    const fs::path& filepath,
    std::string_view content
) {
    if (!ensure_parent_dir(filepath)) {
        return std::unexpected("Cannot create parent directory: " + filepath.parent_path().string());
    }

    auto parent = filepath.parent_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t random_val = dist(rng);

    std::string tmp_name = ".tmp_" + std::to_string(random_val);
    fs::path tmp_path = parent / tmp_name;


    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return std::unexpected("Cannot create temp file: " + tmp_path.string());
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs) {
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return std::unexpected("Write failed to temp file: " + tmp_path.string());
        }
        // Durability: flush the underlying fd *through* the C stdio buffer
        // and into the filesystem.  std::ofstream::flush() only flushes the
        // C++ streambuf; it does NOT guarantee the OS has the bytes on
        // stable storage.
        ofs.sync_with_stdio(true);  // no-op if already true, but cheap
        int fd = ::open(tmp_path.c_str(), O_RDONLY);
        if (fd >= 0) {
            auto fsync_err = detail::fsync_fd(fd);
            ::close(fd);
            if (!fsync_err.empty()) {
                std::error_code ec;
                fs::remove(tmp_path, ec);
                return std::unexpected(fsync_err + " on " + tmp_path.string());
            }
        }
    }

    // Atomically replace the target.  On POSIX, rename() within a single
    // filesystem is atomic at the syscall level — readers see either the
    // old file or the new one, never a half-written one.
    std::error_code ec;
    fs::rename(tmp_path, filepath, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return std::unexpected("Rename failed: " + ec.message());
    }

    // Durability: fsync the parent directory so the rename is durable even
    // across power loss.  Without this, a crash between the rename syscall
    // and the filesystem committing the directory entry can lose the new
    // file (the old inode is still present on disk but the directory
    // points at it again — data loss).
    if (!parent.empty()) {
        int dir_fd = detail::open_for_fsync(parent, /*is_dir=*/true);
        if (dir_fd >= 0) {
            (void)detail::fsync_fd(dir_fd);  // best-effort
            ::close(dir_fd);
        }
    }

    return {};
}



inline std::expected<void, std::string> atomic_write_json(
    const fs::path& filepath,
    std::string_view json_content
) {

    std::string with_newline{json_content};
    if (!with_newline.empty() && with_newline.back() != '\n') {
        with_newline.push_back('\n');
    }
    return atomic_write(filepath, with_newline);
}


inline std::expected<void, std::string> safe_append(
    const fs::path& filepath,
    std::string_view content
) {
    if (!ensure_parent_dir(filepath)) {
        return std::unexpected("Cannot create parent directory: " + filepath.parent_path().string());
    }

    std::ofstream ofs(filepath, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) {
        return std::unexpected("Cannot open file for append: " + filepath.string());
    }

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();

    if (!ofs) {
        return std::unexpected("Append write failed: " + filepath.string());
    }

    return {};
}

} // namespace cc::utils
