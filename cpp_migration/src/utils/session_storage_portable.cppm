module;
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

export module cc.utils.session_storage_portable;

export namespace cc::utils {

namespace fs = std::filesystem;

namespace detail {
    inline fs::path get_base_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";

        // XDG-compatible: use DATA_HOME if available
        const char* xdg_data = std::getenv("XDG_DATA_HOME");
        if (xdg_data) {
            return fs::path(xdg_data) / "claude-code" / "sessions";
        }
        return fs::path(home) / ".claude" / "sessions";
    }
} // namespace detail

// Get the directory where sessions are stored
fs::path get_sessions_dir() {
    auto dir = detail::get_base_dir();
    fs::create_directories(dir);
    return dir;
}

// Save session data to portable storage
std::expected<void, std::string> save_session_data(std::string_view id, std::string_view data) {
    auto dir = get_sessions_dir() / std::string(id);
    fs::create_directories(dir);

    auto path = dir / "session.json";
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("Failed to open session file for writing: " + path.string());
    }

    file << data;
    if (file.fail()) {
        return std::unexpected("Failed to write session data: " + path.string());
    }

    return {};
}

// Load session data from portable storage
std::expected<std::string, std::string> load_session_data(std::string_view id) {
    auto path = get_sessions_dir() / std::string(id) / "session.json";

    if (!fs::exists(path)) {
        return std::unexpected("Session not found: " + std::string(id));
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected("Failed to open session file: " + path.string());
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Delete a session and its directory
bool delete_session(std::string_view id) {
    auto dir = get_sessions_dir() / std::string(id);
    if (!fs::exists(dir)) return false;

    std::error_code ec;
    fs::remove_all(dir, ec);
    return !ec;
}

} // namespace cc::utils
