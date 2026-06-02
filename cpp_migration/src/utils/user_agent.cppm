module;

#include <string>
#include <cstdlib>
#include <sys/utsname.h>

export module cc.utils.user_agent;

export namespace cc::utils {

namespace detail {

// Application version (compile-time or fallback)
constexpr const char* APP_VERSION = "1.0.0";
constexpr const char* APP_NAME = "claude-code";

} // namespace detail

// Get the application version string
inline std::string get_app_version() {
    // Check for runtime version override
    if (const char* ver = std::getenv("CLAUDE_CODE_VERSION")) {
        return std::string(ver);
    }
    return detail::APP_VERSION;
}

// Get platform information (OS name and architecture)
inline std::string get_platform_info() {
    struct utsname info{};
    if (uname(&info) != 0) {
        return "Unknown";
    }

    std::string os_name;
    std::string sysname(info.sysname);

    // Normalize OS name for user-agent
    if (sysname == "Darwin") {
        os_name = "macOS";
    } else if (sysname == "Linux") {
        os_name = "Linux";
    } else {
        os_name = sysname;
    }

    std::string arch(info.machine);
    // Normalize architecture names
    if (arch == "x86_64") arch = "x64";
    else if (arch == "aarch64" || arch == "arm64") arch = "arm64";

    return os_name + " " + arch;
}

// Construct the full user-agent string
inline std::string get_user_agent() {
    // Format: "claude-code/1.0.0 (macOS arm64)"
    return std::string(detail::APP_NAME) + "/" + get_app_version() +
           " (" + get_platform_info() + ")";
}

} // namespace cc::utils
