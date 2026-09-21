module;

#include <string>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <climits>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

export module cc.utils.user_utils;

export namespace cc::utils {

namespace fs = std::filesystem;

// Get the current username
inline std::string get_username() {
    // Try environment first (faster)
    if (const char* user = std::getenv("USER")) {
        return std::string(user);
    }
    if (const char* logname = std::getenv("LOGNAME")) {
        return std::string(logname);
    }

    // Fallback to getpwuid
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_name) {
        return std::string(pw->pw_name);
    }

    return "unknown";
}

// Get the user's home directory
inline fs::path get_home_dir() {
    // Try HOME environment variable first
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home);
    }

    // Fallback to passwd entry
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return fs::path(pw->pw_dir);
    }

    return fs::path("/tmp");
}

// Get the system hostname
inline std::string get_hostname() {
    char hostname[HOST_NAME_MAX + 1]{};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }

    // Fallback to environment
    if (const char* h = std::getenv("HOSTNAME")) {
        return std::string(h);
    }

    return "localhost";
}

// Check if the current process is running as root
inline bool is_root() {
    return getuid() == 0 || geteuid() == 0;
}

// Get the user's default shell path
inline std::string get_shell() {
    // Try SHELL environment variable
    if (const char* shell = std::getenv("SHELL")) {
        return std::string(shell);
    }

    // Fallback to passwd entry
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_shell) {
        return std::string(pw->pw_shell);
    }

    return "/bin/sh";
}

} // namespace cc::utils
