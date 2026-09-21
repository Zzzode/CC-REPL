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

    inline DebugFilter& get_filter() {
        static DebugFilter filter = DebugFilter::from_env();
        return filter;
    }


    inline std::mutex& get_mutex() {
        static std::mutex mtx;
        return mtx;
    }
}


[[nodiscard]] inline bool is_debug_enabled(std::string_view ns) {
    return debug_detail::get_filter().matches(ns);
}


inline void set_debug_namespaces(std::string_view pattern) {
    debug_detail::get_filter().set_patterns(pattern);
}


template <typename... Args>
inline void debug(std::string_view ns, std::format_string<Args...> fmt, Args&&... args) {
    if (!is_debug_enabled(ns)) return;

    auto msg = std::format(fmt, std::forward<Args>(args)...);


    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time, &tm_buf);


    std::lock_guard lock(debug_detail::get_mutex());
    std::cerr << std::format("\033[36m{:02}:{:02}:{:02}.{:03}\033[0m "
                             "\033[35m[{}]\033[0m {}",
                             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                             static_cast<int>(ms.count()),
                             ns, msg)
              << '\n';
}

} // namespace cc::utils
