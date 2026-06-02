// C++23 Logging Module
// Provides logging and error tracking functions
module;

#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <functional>
#include <exception>
#include <iostream>

export module cc.utils.log;

export namespace cc::utils::log {

// 日志级别
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

// 日志条目
struct LogEntry {
    LogLevel level;
    std::string message;
    std::string timestamp;
    std::string file;
    int line;
};

// 错误日志条目
struct ErrorEntry {
    std::string error;
    std::string timestamp;
};

// 错误日志接收器接口
struct ErrorLogSink {
    virtual void log_error(const std::exception& e) = 0;
    virtual void log_mcp_error(const std::string& server_name, const std::exception& e) = 0;
    virtual void log_mcp_debug(const std::string& server_name, const std::string& message) = 0;
    virtual ~ErrorLogSink() = default;
};

namespace detail {

    // 内部状态
    inline std::vector<ErrorEntry>& get_in_memory_errors() {
        static std::vector<ErrorEntry> errors;
        return errors;
    }

    inline ErrorLogSink*& get_error_sink() {
        static ErrorLogSink* sink = nullptr;
        return sink;
    }

    inline constexpr std::size_t MAX_IN_MEMORY_ERRORS = 100;

} // namespace detail

// 获取当前时间戳字符串
[[nodiscard]] inline std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    
#ifdef _WIN32
    localtime_s(&tm, &now_time);
#else
    localtime_r(&now_time, &tm);
#endif
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// 日期转换为文件名安全格式
[[nodiscard]] inline std::string date_to_filename(const std::chrono::system_clock::time_point& tp) {
    std::time_t time = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
    
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    return oss.str();
}

// 附加错误到内存日志
inline void add_to_in_memory_errors(const std::string& error_msg) {
    auto& errors = detail::get_in_memory_errors();
    
    ErrorEntry entry{error_msg, get_timestamp()};
    
    if (errors.size() >= detail::MAX_IN_MEMORY_ERRORS) {
        errors.erase(errors.begin());
    }
    
    errors.push_back(std::move(entry));
}

// 记录错误
inline void log_error(const std::exception& e) {
    add_to_in_memory_errors(e.what());
    
    auto* sink = detail::get_error_sink();
    if (sink) {
        sink->log_error(e);
    }
}

// 记录错误（字符串版本）
inline void log_error(const std::string& message) {
    try {
        throw std::runtime_error(message);
    } catch (const std::exception& e) {
        log_error(e);
    }
}

// 记录 MCP 错误
inline void log_mcp_error(const std::string& server_name, const std::exception& e) {
    std::string error_msg = "[" + server_name + "] " + e.what();
    add_to_in_memory_errors(error_msg);
    
    auto* sink = detail::get_error_sink();
    if (sink) {
        sink->log_mcp_error(server_name, e);
    }
}

// 记录 MCP 调试信息
inline void log_mcp_debug(const std::string& server_name, const std::string& message) {
    auto* sink = detail::get_error_sink();
    if (sink) {
        sink->log_mcp_debug(server_name, message);
    }
}

// 获取内存中的错误列表
[[nodiscard]] inline std::vector<ErrorEntry> get_in_memory_errors() {
    return detail::get_in_memory_errors();
}

// 附加错误日志接收器
inline void attach_error_sink(ErrorLogSink* sink) {
    if (detail::get_error_sink() == nullptr) {
        detail::get_error_sink() = sink;
    }
}

// 清除内存中的错误（用于测试）
inline void clear_in_memory_errors() {
    detail::get_in_memory_errors().clear();
}

// 简单的控制台日志（用于调试）
inline void console_log(LogLevel level, const std::string& message) {
    const char* level_str = "";
    switch (level) {
        case LogLevel::Debug: level_str = "DEBUG"; break;
        case LogLevel::Info: level_str = "INFO"; break;
        case LogLevel::Warning: level_str = "WARN"; break;
        case LogLevel::Error: level_str = "ERROR"; break;
    }
    
    std::clog << "[" << get_timestamp() << "] [" << level_str << "] " << message << std::endl;
}

// 便捷日志函数
inline void debug(const std::string& message) {
    console_log(LogLevel::Debug, message);
}

inline void info(const std::string& message) {
    console_log(LogLevel::Info, message);
}

inline void warning(const std::string& message) {
    console_log(LogLevel::Warning, message);
}

} // namespace cc::utils::log
