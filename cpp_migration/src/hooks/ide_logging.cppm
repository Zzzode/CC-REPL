module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

export module cc.hooks.ide_logging;

export namespace cc::hooks {

namespace detail {
    // 当前 IDE 日志级别
    inline std::string& ide_log_level() {
        static std::string level = "info";
        return level;
    }
} // namespace detail

// 记录 IDE 事件日志
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

// 获取 IDE 日志文件路径
inline std::filesystem::path get_ide_log_path() {
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : "/tmp";
    return base / ".cc-repl" / "logs" / "ide.log";
}

// 设置 IDE 日志级别（debug/info/warn/error）
inline void set_ide_log_level(std::string_view level) {
    detail::ide_log_level() = std::string(level);
}

} // namespace cc::hooks
