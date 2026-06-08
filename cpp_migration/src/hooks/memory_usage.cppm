// cc.hooks.memory_usage — migrated from useMemoryUsage.ts
module;

#include <string>
#include <cstddef>
#include <cstdio>
#include <format>
#include <atomic>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <fstream>
#include <string>
#include <unistd.h>
#endif

export module cc.hooks.memory_usage;

export namespace cc::hooks::memory_usage {

struct MemoryStats {
    std::size_t heap_used;
    std::size_t heap_total;
    std::size_t rss;
    std::size_t external;
    double percent_used;
};

struct MemoryThreshold {
    std::size_t warning_bytes;
    std::size_t critical_bytes;
};

namespace detail {

inline std::atomic<std::size_t> warning_threshold{512ULL * 1024 * 1024};  // 512MB default
inline std::atomic<std::size_t> critical_threshold{1024ULL * 1024 * 1024}; // 1GB default
inline std::atomic<std::size_t> peak_rss{0};

inline std::size_t get_total_system_memory() {
#ifdef __APPLE__
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    sysctl(mib, 2, &mem, &len, nullptr, 0);
    return static_cast<std::size_t>(mem);
#else
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.starts_with("MemTotal:")) {
            std::size_t kb = 0;
            std::sscanf(line.c_str(), "MemTotal: %zu kB", &kb);
            return kb * 1024;
        }
    }
    return 0;
#endif
}

} // namespace detail

inline MemoryStats get_memory_stats() {
    MemoryStats stats{};

#ifdef __APPLE__
    // Get current process memory via Mach task_info
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        stats.rss = info.resident_size;
        stats.heap_used = info.virtual_size;  // Approximation
    }
#else
    // Linux: read from /proc/self/statm
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        std::size_t pages_total = 0, pages_rss = 0;
        statm >> pages_total >> pages_rss;
        long page_size = sysconf(_SC_PAGESIZE);
        stats.rss = pages_rss * static_cast<std::size_t>(page_size);
        stats.heap_used = pages_total * static_cast<std::size_t>(page_size);
    }
#endif

    stats.heap_total = detail::get_total_system_memory();
    stats.external = 0; // No external memory tracking in C++
    stats.percent_used = stats.heap_total > 0
        ? (static_cast<double>(stats.rss) / static_cast<double>(stats.heap_total)) * 100.0
        : 0.0;

    // Track peak RSS
    auto current_peak = detail::peak_rss.load(std::memory_order_relaxed);
    while (stats.rss > current_peak &&
           !detail::peak_rss.compare_exchange_weak(current_peak, stats.rss,
               std::memory_order_relaxed)) {}

    return stats;
}

inline bool check_memory_pressure() {
    auto stats = get_memory_stats();
    return stats.rss >= detail::critical_threshold.load(std::memory_order_relaxed);
}

inline void set_memory_threshold(MemoryThreshold threshold) {
    detail::warning_threshold.store(threshold.warning_bytes, std::memory_order_relaxed);
    detail::critical_threshold.store(threshold.critical_bytes, std::memory_order_relaxed);
}

inline std::string format_memory_stats(MemoryStats stats) {
    auto format_bytes = [](std::size_t bytes) -> std::string {
        if (bytes >= 1024ULL * 1024 * 1024)
            return std::format("{:.1f}GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        if (bytes >= 1024ULL * 1024)
            return std::format("{:.1f}MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        if (bytes >= 1024)
            return std::format("{:.1f}KB", static_cast<double>(bytes) / 1024.0);
        return std::format("{}B", bytes);
    };

    return std::format("rss={} heap_used={} heap_total={} percent={:.1f}%",
        format_bytes(stats.rss), format_bytes(stats.heap_used),
        format_bytes(stats.heap_total), stats.percent_used);
}

inline std::size_t get_peak_memory() {
    return detail::peak_rss.load(std::memory_order_relaxed);
}

} // namespace cc::hooks::memory_usage
