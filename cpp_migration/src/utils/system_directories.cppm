module;

#include <filesystem>
#include <string>
#include <cstdlib>
#include <system_error>

export module cc.utils.system_directories;

namespace fs = std::filesystem;

export namespace cc::utils {

namespace detail {


enum class Platform { MacOS, Linux, Unknown };

inline consteval Platform detect_platform() {
#if defined(__APPLE__)
    return Platform::MacOS;
#elif defined(__linux__)
    return Platform::Linux;
#else
    return Platform::Unknown;
#endif
}

inline fs::path get_home() {
    const char* home = std::getenv("HOME");
    return home ? fs::path{home} : fs::path{"/tmp"};
}

} // namespace detail


inline fs::path get_claude_config_dir() {

    if (const char* env = std::getenv("CLAUDE_CONFIG_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }

    constexpr auto platform = detail::detect_platform();
    auto home = detail::get_home();

    if constexpr (platform == detail::Platform::MacOS) {


        auto legacy = home / ".claude";
        std::error_code ec;
        if (fs::exists(legacy, ec)) {
            return legacy;
        }
        return home / "Library" / "Application Support" / "claude";
    } else {

        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
            return fs::path{xdg} / "claude";
        }
        return home / ".config" / "claude";
    }
}


inline fs::path get_claude_cache_dir() {
    if (const char* env = std::getenv("CLAUDE_CACHE_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }

    constexpr auto platform = detail::detect_platform();
    auto home = detail::get_home();

    if constexpr (platform == detail::Platform::MacOS) {
        return home / "Library" / "Caches" / "claude";
    } else {
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0] != '\0') {
            return fs::path{xdg} / "claude";
        }
        return home / ".cache" / "claude";
    }
}


inline fs::path get_claude_data_dir() {
    if (const char* env = std::getenv("CLAUDE_DATA_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }

    constexpr auto platform = detail::detect_platform();
    auto home = detail::get_home();

    if constexpr (platform == detail::Platform::MacOS) {
        return home / "Library" / "Application Support" / "claude" / "data";
    } else {
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] != '\0') {
            return fs::path{xdg} / "claude";
        }
        return home / ".local" / "share" / "claude";
    }
}


inline fs::path get_claude_log_dir() {
    if (const char* env = std::getenv("CLAUDE_LOG_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }

    constexpr auto platform = detail::detect_platform();
    auto home = detail::get_home();

    if constexpr (platform == detail::Platform::MacOS) {
        return home / "Library" / "Logs" / "claude";
    } else {
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && xdg[0] != '\0') {
            return fs::path{xdg} / "claude" / "logs";
        }
        return home / ".local" / "state" / "claude" / "logs";
    }
}


inline void ensure_directories_exist() {
    std::error_code ec;
    auto dirs = {
        get_claude_config_dir(),
        get_claude_cache_dir(),
        get_claude_data_dir(),
        get_claude_log_dir()
    };

    for (const auto& dir : dirs) {
        fs::create_directories(dir, ec);

    }
}

} // namespace cc::utils
