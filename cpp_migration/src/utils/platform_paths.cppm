module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>

export module cc.utils.platform_paths;

export namespace cc::utils::platform_paths {

struct PlatformPaths {
    std::string home;
    std::string config;
    std::string cache;
    std::string data;
    std::string temp;
};

inline PlatformPaths get_platform_paths() {
    return {"", "", "", "", ""};
}

inline std::string to_platform_path(std::string_view unix_path) {
    return std::string(unix_path);
}

inline std::string normalize_path_separators(std::string_view path) {
    return std::string(path);
}

inline std::optional<std::string> get_apple_terminal_backup_dir() {
    return std::nullopt;
}

inline std::optional<std::string> get_iterm_backup_dir() {
    return std::nullopt;
}

inline bool is_wsl_environment() {
    return false;
}

} // namespace cc::utils::platform_paths
