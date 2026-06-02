module;
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.log_messages;

export namespace cc::hooks {

// 日志条目
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string level;
    std::string message;
    std::map<std::string, std::string> context;
};

namespace detail {
    // 内存中的日志缓冲区
    inline std::vector<LogEntry>& log_buffer() {
        static std::vector<LogEntry> buffer;
        return buffer;
    }

    constexpr std::size_t max_log_buffer_size = 10000;
} // namespace detail

// 记录一条日志消息
inline void log_message(
    std::string_view level,
    std::string_view message,
    std::map<std::string, std::string> context = {}
) {
    auto& buffer = detail::log_buffer();

    // 如果缓冲区已满，移除最旧的条目
    if (buffer.size() >= detail::max_log_buffer_size) {
        buffer.erase(buffer.begin());
    }

    buffer.push_back(LogEntry{
        .timestamp = std::chrono::system_clock::now(),
        .level = std::string(level),
        .message = std::string(message),
        .context = std::move(context)
    });
}

// 获取最近的 n 条日志
inline std::vector<LogEntry> get_recent_logs(std::size_t n = 100) {
    auto& buffer = detail::log_buffer();
    if (n >= buffer.size()) return buffer;

    // 返回最后 n 条
    return std::vector<LogEntry>(buffer.end() - static_cast<ptrdiff_t>(n), buffer.end());
}

// 清除所有日志
inline void clear_logs() {
    detail::log_buffer().clear();
}

} // namespace cc::hooks
