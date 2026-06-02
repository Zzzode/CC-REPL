module;

#include <filesystem>
#include <optional>
#include <string>
#include <cstdlib>

export module cc.utils.xdg;

namespace fs = std::filesystem;

export namespace cc::utils {

// XDG Base Directory Specification 实现
// 所有函数均优先尊重环境变量覆盖

// $XDG_CONFIG_HOME，默认 ~/.config
inline fs::path xdg_config_home() {
    if (const char* env = std::getenv("XDG_CONFIG_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".config";
}

// $XDG_DATA_HOME，默认 ~/.local/share
inline fs::path xdg_data_home() {
    if (const char* env = std::getenv("XDG_DATA_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".local" / "share";
}

// $XDG_CACHE_HOME，默认 ~/.cache
inline fs::path xdg_cache_home() {
    if (const char* env = std::getenv("XDG_CACHE_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".cache";
}

// $XDG_STATE_HOME，默认 ~/.local/state
inline fs::path xdg_state_home() {
    if (const char* env = std::getenv("XDG_STATE_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".local" / "state";
}

// $XDG_RUNTIME_DIR，无默认值（返回 optional）
// 该目录应由操作系统分配，通常为 /run/user/<uid>
inline std::optional<fs::path> xdg_runtime_dir() {
    if (const char* env = std::getenv("XDG_RUNTIME_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }
    return std::nullopt;
}

} // namespace cc::utils
