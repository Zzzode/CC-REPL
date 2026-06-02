// C++23 Platform Detection Module
// Provides platform identification functions
module;

#include <string>

export module cc.utils.platform;

export namespace cc::utils::platform {

enum class Platform {
    Unknown,
    Windows,
    MacOS,
    Linux,
    BSD,
    Other
};

// 获取当前平台
[[nodiscard]] inline Platform get_platform() {
#ifdef _WIN32
    return Platform::Windows;
#elif __APPLE__
    return Platform::MacOS;
#elif __linux__
    return Platform::Linux;
#elif __BSD__
    return Platform::BSD;
#else
    return Platform::Other;
#endif
}

// 获取平台名称字符串
[[nodiscard]] inline std::string get_platform_name() {
    switch (get_platform()) {
        case Platform::Windows: return "windows";
        case Platform::MacOS: return "macos";
        case Platform::Linux: return "linux";
        case Platform::BSD: return "bsd";
        default: return "unknown";
    }
}

// 检查是否是 Windows 平台
[[nodiscard]] inline bool is_windows() {
    return get_platform() == Platform::Windows;
}

// 检查是否是 macOS 平台
[[nodiscard]] inline bool is_macos() {
    return get_platform() == Platform::MacOS;
}

// 检查是否是 Linux 平台
[[nodiscard]] inline bool is_linux() {
    return get_platform() == Platform::Linux;
}

// 检查是否是类 Unix 平台（macOS、Linux、BSD）
[[nodiscard]] inline bool is_unix() {
    Platform p = get_platform();
    return p == Platform::MacOS || p == Platform::Linux || p == Platform::BSD;
}

// 获取路径分隔符
[[nodiscard]] inline char path_separator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

// 获取路径列表分隔符
[[nodiscard]] inline char path_list_separator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

// 获取默认 shell 路径
[[nodiscard]] inline std::string default_shell() {
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}

} // namespace cc::utils::platform
