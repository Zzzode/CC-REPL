module;

#include <filesystem>
#include <optional>
#include <string>
#include <cstdlib>

export module cc.utils.xdg;

namespace fs = std::filesystem;

export namespace cc::utils {





inline fs::path xdg_config_home() {
    if (const char* env = std::getenv("XDG_CONFIG_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".config";
}


inline fs::path xdg_data_home() {
    if (const char* env = std::getenv("XDG_DATA_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".local" / "share";
}


inline fs::path xdg_cache_home() {
    if (const char* env = std::getenv("XDG_CACHE_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".cache";
}


inline fs::path xdg_state_home() {
    if (const char* env = std::getenv("XDG_STATE_HOME"); env && env[0] != '\0') {
        return fs::path{env};
    }
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return fs::path{home} / ".local" / "state";
}



inline std::optional<fs::path> xdg_runtime_dir() {
    if (const char* env = std::getenv("XDG_RUNTIME_DIR"); env && env[0] != '\0') {
        return fs::path{env};
    }
    return std::nullopt;
}

} // namespace cc::utils
