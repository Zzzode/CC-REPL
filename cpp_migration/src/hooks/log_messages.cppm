module;
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.log_messages;

export namespace cc::hooks {


struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string level;
    std::string message;
    std::map<std::string, std::string> context;
};

namespace detail {

    inline std::vector<LogEntry>& log_buffer() {
        static std::vector<LogEntry> buffer;
        return buffer;
    }

    constexpr std::size_t max_log_buffer_size = 10000;
} // namespace detail


inline void log_message(
    std::string_view level,
    std::string_view message,
    std::map<std::string, std::string> context = {}
) {
    auto& buffer = detail::log_buffer();


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


inline std::vector<LogEntry> get_recent_logs(std::size_t n = 100) {
    auto& buffer = detail::log_buffer();
    if (n >= buffer.size()) return buffer;


    return std::vector<LogEntry>(buffer.end() - static_cast<ptrdiff_t>(n), buffer.end());
}


inline void clear_logs() {
    detail::log_buffer().clear();
}

} // namespace cc::hooks
