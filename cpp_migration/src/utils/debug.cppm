module;
#include <string>
#include <string_view>
#include <cstdlib>
#include <mutex>
#include <format>
#include <iostream>
#include <chrono>
#include <vector>

export module cc.utils.debug;

import cc.utils.debug_filter;

export namespace cc::utils {

namespace debug_detail {
    // 全局 debug 过滤器实例（线程安全初始化）
    inline DebugFilter& get_filter() {
        static DebugFilter filter = DebugFilter::from_env();
        return filter;
    }

    // 输出互斥锁，防止多线程输出交错
    inline std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }
}

// 检查指定命名空间的调试输出是否启用
[[nodiscard]] inline bool is_debug_enabled(std::string_view ns) {
    return debug_detail::get_filter().matches(ns);
}

// 设置调试命名空间模式（逗号分隔，支持通配符和取反）
inline void set_debug_namespaces(std::string_view pattern) {
    debug_detail::get_filter().set_patterns(pattern);
}

// 条件性调试输出
template <typename... Args>
inline void debug(std::string_view ns, std::format_string<Args...> fmt, Args&&... args) {
    if (!is_debug_enabled(ns)) return;

    auto msg = std::format(fmt, std::forward<Args>(args)...);

    // 获取时间戳
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time, &tm_buf);

    // 线程安全输出
    std::lock_guard lock(debug_detail::get_mutex());
    std::cerr << std::format("\033[36m{:02}:{:02}:{:02}.{:03}\033[0m "
                             "\033[35m[{}]\033[0m {}",
                             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                             static_cast<int>(ms.count()),
                             ns, msg)
              << '\n';
}

} // namespace cc::utils
