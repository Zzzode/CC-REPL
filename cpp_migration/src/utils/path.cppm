// C++23 Path Utilities Module
// Provides path manipulation functions similar to src/utils/path.ts
module;

#include <string>
#include <string_view>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
#include <shlobj.h>
#include <objbase.h>
#endif

export module cc.utils.path;

export namespace cc::utils::path {

namespace fs = std::filesystem;

// 获取用户主目录
[[nodiscard]] inline fs::path get_home_dir() {
#ifdef _WIN32
    char* home = std::getenv("USERPROFILE");
    if (home) return fs::path(home);
    
    char* drive = std::getenv("HOMEDRIVE");
    char* path = std::getenv("HOMEPATH");
    if (drive && path) return fs::path(std::string(drive) + path);
#else
    char* home = std::getenv("HOME");
    if (home) return fs::path(home);
#endif
    return fs::path();
}

// 扩展路径中的 ~ 为用户主目录
[[nodiscard]] inline fs::path expand_tilde(const fs::path& path) {
    std::string path_str = path.string();
    if (path_str.empty()) return path;
    
    if (path_str == "~") {
        return get_home_dir();
    }
    
    if (path_str.starts_with("~/") || path_str.starts_with("~\\")) {
        return get_home_dir() / path_str.substr(2);
    }
    
    return path;
}

// 扩展路径（处理 ~ 和绝对/相对路径）
[[nodiscard]] inline fs::path expand_path(const fs::path& path, const fs::path& base_dir = {}) {
    fs::path expanded = expand_tilde(path);
    
    if (expanded.is_absolute()) {
        return fs::absolute(expanded);
    }
    
    fs::path base = base_dir.empty() ? fs::current_path() : base_dir;
    return fs::absolute(base / expanded);
}

// 转换为相对路径（如果在当前工作目录下）
[[nodiscard]] inline fs::path to_relative_path(const fs::path& absolute_path) {
    try {
        fs::path cwd = fs::current_path();
        fs::path relative = fs::relative(absolute_path, cwd);
        
        // 检查是否在当前目录下（不以 .. 开头）
        std::string rel_str = relative.string();
        if (rel_str.starts_with("..")) {
            return absolute_path;
        }
        return relative;
    } catch (...) {
        return absolute_path;
    }
}

// 获取路径对应的目录（如果是文件则返回父目录）
[[nodiscard]] inline fs::path get_directory_for_path(const fs::path& path) {
    try {
        if (fs::is_directory(path)) {
            return path;
        }
    } catch (...) {}
    return path.parent_path();
}

// 检查路径是否包含目录遍历（../ 或 ..\）
[[nodiscard]] inline bool contains_path_traversal(std::string_view path) {
    return path.find("../") != std::string_view::npos || 
           path.find("..\\") != std::string_view::npos;
}

// 规范化路径用于配置键（统一使用 / 分隔符）
[[nodiscard]] inline std::string normalize_path_for_config_key(const fs::path& path) {
    std::string result = fs::weakly_canonical(path).string();
    // 将所有反斜杠替换为正斜杠
    for (char& c : result) {
        if (c == '\\') {
            c = '/';
        }
    }
    return result;
}

// 获取路径的显示形式（尽量使用相对路径或 ~）
[[nodiscard]] inline std::string get_display_path(const fs::path& path) {
    fs::path expanded = expand_path(path);
    
    // 尝试使用相对于当前工作目录的路径
    try {
        fs::path rel = fs::relative(expanded, fs::current_path());
        std::string rel_str = rel.string();
        if (!rel_str.starts_with("..")) {
            return rel_str;
        }
    } catch (...) {}
    
    // 尝试使用相对于主目录的 ~ 路径
    try {
        fs::path home = get_home_dir();
        if (!home.empty()) {
            fs::path rel_to_home = fs::relative(expanded, home);
            std::string rel_home_str = rel_to_home.string();
            if (!rel_home_str.starts_with("..")) {
                return "~/" + rel_home_str;
            }
        }
    } catch (...) {}
    
    return expanded.string();
}

// 规范化路径用于比较（Windows 下不区分大小写）
[[nodiscard]] inline std::string normalize_path_for_comparison(const fs::path& path) {
    std::string result = fs::absolute(path).string();
#ifdef _WIN32
    // Windows 下转换为小写
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return result;
}

// 比较两个路径是否相等（考虑平台差异）
[[nodiscard]] inline bool paths_equal(const fs::path& p1, const fs::path& p2) {
    return normalize_path_for_comparison(p1) == normalize_path_for_comparison(p2);
}

} // namespace cc::utils::path
