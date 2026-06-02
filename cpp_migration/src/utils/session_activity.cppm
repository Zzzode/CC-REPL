module;
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.session_activity;

export namespace cc::utils {

namespace fs = std::filesystem;

namespace detail {
    // In-memory activity tracking with thread safety
    inline std::mutex& activity_mutex() {
        static std::mutex m;
        return m;
    }

    using TimePoint = std::chrono::system_clock::time_point;

    inline std::map<std::string, TimePoint>& activity_map() {
        static std::map<std::string, TimePoint> m;
        return m;
    }

    inline fs::path get_sessions_base_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }
} // namespace detail

// Record activity for a session (update last-active timestamp)
void record_activity(std::string_view session_id) {
    std::lock_guard lock(detail::activity_mutex());
    detail::activity_map()[std::string(session_id)] = std::chrono::system_clock::now();

    // Persist to disk
    auto path = detail::get_sessions_base_dir() / std::string(session_id) / ".last_active";
    if (auto parent = path.parent_path(); fs::exists(parent)) {
        std::ofstream f(path);
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        f << t;
    }
}

// Get last activity time for a session
std::optional<std::chrono::system_clock::time_point> get_last_activity(std::string_view session_id) {
    std::lock_guard lock(detail::activity_mutex());
    auto& map = detail::activity_map();
    auto it = map.find(std::string(session_id));
    if (it != map.end()) {
        return it->second;
    }

    // Try loading from disk
    auto path = detail::get_sessions_base_dir() / std::string(session_id) / ".last_active";
    if (fs::exists(path)) {
        std::ifstream f(path);
        std::time_t t;
        if (f >> t) {
            auto tp = std::chrono::system_clock::from_time_t(t);
            map[std::string(session_id)] = tp;
            return tp;
        }
    }

    return std::nullopt;
}

// Get how long a session has been idle
std::chrono::seconds get_idle_duration(std::string_view session_id) {
    auto last = get_last_activity(session_id);
    if (!last) return std::chrono::seconds(0);

    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - *last);
}

// Check if a session is idle beyond a threshold
bool is_session_idle(std::string_view session_id, std::chrono::seconds threshold) {
    return get_idle_duration(session_id) > threshold;
}

} // namespace cc::utils
