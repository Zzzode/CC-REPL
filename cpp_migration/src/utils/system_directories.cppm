module;

#include <filesystem>
#include <string>
#include <cstdlib>
#include <system_error>

export module cc.utils.system_directories;

namespace fs = std::filesystem;

export namespace cc::utils {

namespace detail {

// 检测运行平台
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

// Claude 配置目录 (~/.claude)
inline fs::path get_claude_config_dir() {
    // 优先检查环境变量覆盖
    if (const char* env = std::getenv("CLAUDE_CONFIG_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }

    constexpr auto platform = detail::detect_platform();
    auto home = detail::get_home();

    if constexpr (platform == detail::Platform::MacOS) {
        // macOS: 优先使用 ~/Library/Application Support/claude
        // 但保持向后兼容 ~/.claude
        auto legacy = home / ".claude";
        std::error_code ec;
        if (fs::exists(legacy, ec)) {
            return legacy;
        }
        return home / "Library" / "Application Support" / "claude";
    } else {
        // Linux: 使用 XDG 规范
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
            return fs::path{xdg} / "claude";
        }
        return home / ".config" / "claude";
    }
}

// Claude 缓存目录
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

// Claude 数据目录
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

// Claude 日志目录
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

// 确保所有必需目录存在，不存在则递归创建
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
        // 忽略创建失败（可能是权限问题）
    }
}

} // namespace cc::utils
