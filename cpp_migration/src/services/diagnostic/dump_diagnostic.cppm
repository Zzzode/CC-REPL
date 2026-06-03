/// @file dump_diagnostic.cppm
/// @brief System diagnostic collection for debugging and support.
/// Gathers OS info, memory usage, runtime state, and active plugins/MCP servers.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <chrono>
#include <cstdlib>
#include <array>
#include <cstdio>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#endif

export module cc.services.diagnostic.dump_diagnostic;

export namespace cc::services::diagnostic {

namespace fs = std::filesystem;

/// Diagnostic dump level
enum class DumpLevel : std::uint8_t {
    Minimal,    // Only version and basic state
    Standard,   // + memory, OS info
    Verbose,    // + plugins, MCP servers
    Full        // + environment variables, all config
};

/// System diagnostic information
struct DiagnosticInfo {
    std::string os_version;
    std::string arch;
    std::string app_version;
    std::uint64_t memory_usage_bytes{0};
    std::uint64_t total_memory_bytes{0};
    std::uint64_t uptime_seconds{0};
    std::vector<std::string> active_plugins;
    std::vector<std::string> mcp_servers;
    std::string working_directory;
    std::string home_directory;
    std::optional<std::string> shell;
    std::optional<std::string> term;
};

/// Get current process memory usage (platform-specific)
[[nodiscard]] inline std::uint64_t get_process_memory() {
#ifdef __APPLE__
    struct mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        std::uint64_t pages = 0;
        statm >> pages; // total program size
        statm >> pages; // resident set
        return pages * 4096; // Assume 4KB page size
    }
    return 0;
#else
    return 0;
#endif
}

/// Get total system memory
[[nodiscard]] inline std::uint64_t get_total_memory() {
#ifdef __APPLE__
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    std::uint64_t mem = 0;
    std::size_t len = sizeof(mem);
    sysctl(mib, 2, &mem, &len, nullptr, 0);
    return mem;
#elif defined(__linux__)
    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        return si.totalram * si.mem_unit;
    }
    return 0;
#else
    return 0;
#endif
}

/// Get OS version string
[[nodiscard]] inline std::string get_os_version() {
#if defined(__APPLE__) || defined(__linux__)
    struct utsname uts{};
    if (uname(&uts) == 0) {
        return std::format("{} {} {}", uts.sysname, uts.release, uts.machine);
    }
#endif
    return "unknown";
}

/// Get system uptime in seconds
[[nodiscard]] inline std::uint64_t get_system_uptime() {
#ifdef __APPLE__
    struct timeval boot_time{};
    std::size_t len = sizeof(boot_time);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot_time, &len, nullptr, 0) == 0) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto boot = std::chrono::seconds(boot_time.tv_sec);
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now).count() - boot.count());
    }
    return 0;
#elif defined(__linux__)
    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        return static_cast<std::uint64_t>(si.uptime);
    }
    return 0;
#else
    return 0;
#endif
}

/// Collect diagnostic information at the specified verbosity level
[[nodiscard]] inline DiagnosticInfo collect_diagnostics(DumpLevel level = DumpLevel::Standard) {
    DiagnosticInfo info;

    // Always collect basic info
    info.app_version = "cc-repl 1.0.0-cpp (C++23)";
    info.os_version = get_os_version();

#if defined(__APPLE__) || defined(__linux__)
    struct utsname uts{};
    if (uname(&uts) == 0) {
        info.arch = uts.machine;
    }
#endif

    if (level == DumpLevel::Minimal) return info;

    // Standard: add memory and uptime
    info.memory_usage_bytes = get_process_memory();
    info.total_memory_bytes = get_total_memory();
    info.uptime_seconds = get_system_uptime();

    std::error_code ec;
    info.working_directory = fs::current_path(ec).string();
    if (const char* home = std::getenv("HOME")) {
        info.home_directory = home;
    }

    if (level == DumpLevel::Standard) return info;

    // Verbose: add environment
    if (const char* shell = std::getenv("SHELL")) {
        info.shell = shell;
    }
    if (const char* term = std::getenv("TERM")) {
        info.term = term;
    }

    // Plugins and MCP servers would be populated by the session manager
    // These remain empty until wired to the actual registry

    return info;
}

/// Format diagnostics for human-readable display
[[nodiscard]] inline std::string format_diagnostics(const DiagnosticInfo& info) {
    std::string output;
    output += std::format("CC-REPL Diagnostic Report\n");
    output += std::format("========================\n\n");
    output += std::format("Version:     {}\n", info.app_version);
    output += std::format("OS:          {}\n", info.os_version);
    output += std::format("Arch:        {}\n", info.arch);

    if (info.memory_usage_bytes > 0) {
        auto mb = info.memory_usage_bytes / (1024 * 1024);
        auto total_mb = info.total_memory_bytes / (1024 * 1024);
        output += std::format("Memory:      {} MB / {} MB\n", mb, total_mb);
    }

    if (info.uptime_seconds > 0) {
        auto hours = info.uptime_seconds / 3600;
        auto minutes = (info.uptime_seconds % 3600) / 60;
        output += std::format("Uptime:      {}h {}m\n", hours, minutes);
    }

    if (!info.working_directory.empty()) {
        output += std::format("Working Dir: {}\n", info.working_directory);
    }

    if (info.shell.has_value()) {
        output += std::format("Shell:       {}\n", *info.shell);
    }

    if (!info.active_plugins.empty()) {
        output += std::format("\nPlugins ({}):\n", info.active_plugins.size());
        for (const auto& p : info.active_plugins) {
            output += std::format("  - {}\n", p);
        }
    }

    if (!info.mcp_servers.empty()) {
        output += std::format("\nMCP Servers ({}):\n", info.mcp_servers.size());
        for (const auto& s : info.mcp_servers) {
            output += std::format("  - {}\n", s);
        }
    }

    return output;
}

/// Dump diagnostics to a file
/// Returns true on success
[[nodiscard]] inline bool dump_diagnostics_to_file(
    std::string_view output_path,
    DumpLevel level = DumpLevel::Standard) {

    fs::path path(output_path);

    // Ensure parent directory exists
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream file(path);
    if (!file.is_open()) return false;

    auto info = collect_diagnostics(level);
    file << format_diagnostics(info);

    return file.good();
}

/// Dump diagnostics to the default location and return the path
[[nodiscard]] inline std::optional<std::string> dump_diagnostics_auto(
    DumpLevel level = DumpLevel::Standard) {

    const char* home = std::getenv("HOME");
    fs::path dir = home
        ? fs::path(home) / ".claude" / "diagnostics"
        : fs::temp_directory_path() / "cc-repl-diagnostics";

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return std::nullopt;

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&time_t, &tm_buf);

    auto filename = std::format("diag_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.txt",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    auto filepath = dir / filename;
    if (dump_diagnostics_to_file(filepath.string(), level)) {
        return filepath.string();
    }
    return std::nullopt;
}

} // namespace cc::services::diagnostic
