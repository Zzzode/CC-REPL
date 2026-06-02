module;
#include <string>
#include <string_view>
#include <map>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <mutex>

export module cc.bridge.debug_utils;

export namespace cc::bridge {

namespace detail {
    inline std::mutex& get_debug_mutex() {
        static std::mutex mutex;
        return mutex;
    }

    inline bool& get_debug_enabled_ref() {
        static bool enabled = []() {
            const char* env = std::getenv("CLAUDE_BRIDGE_DEBUG");
            return env && (std::string(env) == "1" || std::string(env) == "true");
        }();
        return enabled;
    }
}

// Check if bridge debug logging is enabled
bool is_bridge_debug_enabled();

// Get the path to the bridge debug log file
std::filesystem::path get_bridge_log_path();

// Log a bridge event with associated data to the debug log file
void log_bridge_event(std::string_view event, std::map<std::string, std::string> data) {
    if (!is_bridge_debug_enabled()) return;

    std::lock_guard lock(detail::get_debug_mutex());

    auto log_path = get_bridge_log_path();
    std::filesystem::create_directories(log_path.parent_path());

    std::ofstream ofs(log_path, std::ios::app);
    if (!ofs.is_open()) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    ofs << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");
    ofs << "." << std::setfill('0') << std::setw(3) << ms.count();
    ofs << " [" << event << "]";

    // Write data fields
    for (const auto& [key, value] : data) {
        ofs << " " << key << "=" << value;
    }
    ofs << "\n";
}

// Get the path to the bridge debug log file
std::filesystem::path get_bridge_log_path() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "claude-code" / "bridge" / "debug.log";
    }
    return std::filesystem::path("/tmp") / "claude-code-bridge-debug.log";
}

// Dump the current bridge state as a diagnostic string
std::string dump_bridge_state() {
    std::ostringstream oss;

    oss << "=== Bridge State Dump ===\n";

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    oss << "Timestamp: " << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S") << "\n";

    // Debug mode
    oss << "Debug enabled: " << (is_bridge_debug_enabled() ? "yes" : "no") << "\n";

    // Log file info
    auto log_path = get_bridge_log_path();
    oss << "Log path: " << log_path.string() << "\n";
    if (std::filesystem::exists(log_path)) {
        auto log_size = std::filesystem::file_size(log_path);
        oss << "Log size: " << log_size << " bytes\n";
    } else {
        oss << "Log file: not created yet\n";
    }

    // Environment
    const char* envless = std::getenv("CLAUDE_ENVLESS");
    oss << "Envless mode: " << (envless ? envless : "not set") << "\n";

    oss << "=========================\n";

    return oss.str();
}

// Check if bridge debug logging is enabled
bool is_bridge_debug_enabled() {
    return detail::get_debug_enabled_ref();
}

} // namespace cc::bridge
