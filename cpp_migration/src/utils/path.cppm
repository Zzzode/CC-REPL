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


[[nodiscard]] inline fs::path expand_path(const fs::path& path, const fs::path& base_dir = {}) {
    fs::path expanded = expand_tilde(path);
    
    if (expanded.is_absolute()) {
        return fs::absolute(expanded);
    }
    
    fs::path base = base_dir.empty() ? fs::current_path() : base_dir;
    return fs::absolute(base / expanded);
}


[[nodiscard]] inline fs::path to_relative_path(const fs::path& absolute_path) {
    try {
        fs::path cwd = fs::current_path();
        fs::path relative = fs::relative(absolute_path, cwd);
        

        std::string rel_str = relative.string();
        if (rel_str.starts_with("..")) {
            return absolute_path;
        }
        return relative;
    } catch (...) {
        return absolute_path;
    }
}


[[nodiscard]] inline fs::path get_directory_for_path(const fs::path& path) {
    try {
        if (fs::is_directory(path)) {
            return path;
        }
    } catch (...) {}
    return path.parent_path();
}


[[nodiscard]] inline bool contains_path_traversal(std::string_view path) {
    return path.find("../") != std::string_view::npos || 
           path.find("..\\") != std::string_view::npos;
}


[[nodiscard]] inline std::string normalize_path_for_config_key(const fs::path& path) {
    std::string result = fs::weakly_canonical(path).string();

    for (char& c : result) {
        if (c == '\\') {
            c = '/';
        }
    }
    return result;
}


[[nodiscard]] inline std::string get_display_path(const fs::path& path) {
    fs::path expanded = expand_path(path);
    

    try {
        fs::path rel = fs::relative(expanded, fs::current_path());
        std::string rel_str = rel.string();
        if (!rel_str.starts_with("..")) {
            return rel_str;
        }
    } catch (...) {}
    

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


[[nodiscard]] inline std::string normalize_path_for_comparison(const fs::path& path) {
    std::string result = fs::absolute(path).string();
#ifdef _WIN32

    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return result;
}


[[nodiscard]] inline bool paths_equal(const fs::path& p1, const fs::path& p2) {
    return normalize_path_for_comparison(p1) == normalize_path_for_comparison(p2);
}

} // namespace cc::utils::path
