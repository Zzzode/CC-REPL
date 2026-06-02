module;
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
export module cc.services.auto_dream.consolidation_lock;

export namespace cc::services::auto_dream {

namespace fs = std::filesystem;
using time_point = std::chrono::system_clock::time_point;

// Lock file handle for consolidation
struct LockFile {
    fs::path path;
    bool held{false};

    ~LockFile() {
        if (held) {
            // Release lock by removing lock file
            std::error_code ec;
            fs::remove(path, ec);
        }
    }

    // Non-copyable, movable
    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;
    LockFile(LockFile&& other) noexcept : path(std::move(other.path)), held(other.held) {
        other.held = false;
    }
    LockFile& operator=(LockFile&& other) noexcept {
        path = std::move(other.path);
        held = other.held;
        other.held = false;
        return *this;
    }
    LockFile() = default;
};

// Attempt to acquire the consolidation lock
auto acquire_consolidation_lock() -> std::expected<LockFile, std::string> {
    // Lock file location in config directory
    fs::path lock_path = fs::temp_directory_path() / "claude_dream.lock";

    if (fs::exists(lock_path)) {
        return std::unexpected("Consolidation already in progress");
    }

    // Create lock file
    LockFile lock;
    lock.path = lock_path;
    lock.held = true;
    std::ofstream lock_file(lock_path, std::ios::out | std::ios::trunc);
    if (!lock_file) {
        return std::unexpected("Unable to create consolidation lock");
    }
    lock_file << "locked\n";
    return lock;
}

// Check if a consolidation is currently running
auto is_consolidation_running() -> bool {
    fs::path lock_path = fs::temp_directory_path() / "claude_dream.lock";
    return fs::exists(lock_path);
}

// Get the time of the last completed consolidation
auto get_last_consolidation_time() -> std::optional<time_point> {
    // No persistent consolidation timestamp is stored by this module.
    return std::nullopt;
}

} // namespace cc::services::auto_dream
