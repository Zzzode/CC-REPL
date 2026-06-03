module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

export module cc.hooks.ide_logging;

export namespace cc::hooks {

inline std::filesystem::path get_ide_log_path();

namespace detail {
    // Current IDE log level.
    inline std::string& ide_log_level() {
        static std::string level = "info";
        return level;
    }
} // namespace detail

// Append an IDE event to the local log.
inline void log_ide_event(std::string_view event, std::map<std::string, std::string> data) {
    auto path = get_ide_log_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;

    std::ofstream file(path, std::ios::app);
    if (!file) return;

    file << "[" << detail::ide_log_level() << "] " << event;
    for (const auto& [k, v] : data) {
        file << " " << k << "=" << v;
    }
    file << "\n";
}

// Return the local IDE log file path.
inline std::filesystem::path get_ide_log_path() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : "/tmp";
    return base / ".cc-repl" / "logs" / "ide.log";
}

// Set the IDE log level (debug/info/warn/error).
inline void set_ide_log_level(std::string_view level) {
    detail::ide_log_level() = std::string(level);
}

} // namespace cc::hooks
