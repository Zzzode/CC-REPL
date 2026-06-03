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


[[nodiscard]] inline std::string get_platform_name() {
    switch (get_platform()) {
        case Platform::Windows: return "windows";
        case Platform::MacOS: return "macos";
        case Platform::Linux: return "linux";
        case Platform::BSD: return "bsd";
        default: return "unknown";
    }
}


[[nodiscard]] inline bool is_windows() {
    return get_platform() == Platform::Windows;
}


[[nodiscard]] inline bool is_macos() {
    return get_platform() == Platform::MacOS;
}


[[nodiscard]] inline bool is_linux() {
    return get_platform() == Platform::Linux;
}


[[nodiscard]] inline bool is_unix() {
    Platform p = get_platform();
    return p == Platform::MacOS || p == Platform::Linux || p == Platform::BSD;
}


[[nodiscard]] inline char path_separator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}


[[nodiscard]] inline char path_list_separator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}


[[nodiscard]] inline std::string default_shell() {
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}

} // namespace cc::utils::platform
