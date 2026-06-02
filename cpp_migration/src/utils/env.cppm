// C++23 Environment Variables Module
// Provides access to environment variables
module;

#include <string>
#include <optional>
#include <cstdlib>
#include <vector>
#include <sstream>

export module cc.utils.env;

export namespace cc::utils::env {

// 获取环境变量
[[nodiscard]] inline std::optional<std::string> get_env(std::string_view name) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value) {
        return std::string(value);
    }
    return std::nullopt;
}

// 获取环境变量，带默认值
[[nodiscard]] inline std::string get_env_or(std::string_view name, std::string_view default_value) {
    auto value = get_env(name);
    return value ? *value : std::string(default_value);
}

// 设置环境变量
inline bool set_env(std::string_view name, std::string_view value, bool overwrite = true) {
#ifdef _WIN32
    if (!overwrite && get_env(name).has_value()) {
        return true;
    }
    return _putenv_s(std::string(name).c_str(), std::string(value).c_str()) == 0;
#else
    return setenv(std::string(name).c_str(), std::string(value).c_str(), overwrite ? 1 : 0) == 0;
#endif
}

// 移除环境变量
inline bool unset_env(std::string_view name) {
#ifdef _WIN32
    return _putenv_s(std::string(name).c_str(), "") == 0;
#else
    return unsetenv(std::string(name).c_str()) == 0;
#endif
}

// 检查环境变量是否存在
[[nodiscard]] inline bool has_env(std::string_view name) {
    return get_env(name).has_value();
}

// 检查环境变量是否为 true（"1", "true", "yes", "on" 等，不区分大小写）
[[nodiscard]] inline bool is_env_truthy(std::string_view name) {
    auto value = get_env(name);
    if (!value) return false;
    
    std::string lower;
    for (char c : *value) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

// 检查环境变量是否为 false（"0", "false", "no", "off" 等，不区分大小写）
[[nodiscard]] inline bool is_env_falsy(std::string_view name) {
    auto value = get_env(name);
    if (!value) return false;
    
    std::string lower;
    for (char c : *value) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    return lower == "0" || lower == "false" || lower == "no" || lower == "off";
}

// 获取整数环境变量
[[nodiscard]] inline std::optional<int> get_env_int(std::string_view name) {
    auto value = get_env(name);
    if (!value) return std::nullopt;
    
    try {
        return std::stoi(*value);
    } catch (...) {
        return std::nullopt;
    }
}

// 获取整数环境变量，带默认值
[[nodiscard]] inline int get_env_int_or(std::string_view name, int default_value) {
    auto value = get_env_int(name);
    return value ? *value : default_value;
}

// 展开路径中的环境变量（如 $HOME 或 %USERPROFILE%）
[[nodiscard]] inline std::string expand_env_vars(std::string_view path) {
    std::string result;
    std::size_t i = 0;
    
    while (i < path.size()) {
        if (path[i] == '$') {
            // 处理 $VAR 或 ${VAR}
            std::size_t start = i + 1;
            std::size_t end;
            bool has_braces = false;
            
            if (start < path.size() && path[start] == '{') {
                has_braces = true;
                start++;
                end = path.find('}', start);
            } else {
                end = start;
                while (end < path.size() && (std::isalnum(static_cast<unsigned char>(path[end])) || path[end] == '_')) {
                    end++;
                }
            }
            
            if (end != std::string_view::npos) {
                std::string var_name = std::string(path.substr(start, end - start));
                auto var_value = get_env(var_name);
                if (var_value) {
                    result += *var_value;
                }
                i = has_braces ? end + 1 : end;
                continue;
            }
        }
        
#ifdef _WIN32
        if (path[i] == '%') {
            // 处理 %VAR%
            std::size_t start = i + 1;
            std::size_t end = path.find('%', start);
            if (end != std::string_view::npos) {
                std::string var_name = std::string(path.substr(start, end - start));
                auto var_value = get_env(var_name);
                if (var_value) {
                    result += *var_value;
                }
                i = end + 1;
                continue;
            }
        }
#endif
        
        result += path[i];
        i++;
    }
    
    return result;
}

// 获取 PATH 环境变量的各个路径
[[nodiscard]] inline std::vector<std::string> get_path() {
    auto path_str = get_env("PATH");
    if (!path_str) return {};
    
    std::vector<std::string> paths;
    std::stringstream ss(*path_str);
    std::string path;
    char separator = ':';
    
#ifdef _WIN32
    separator = ';';
#endif
    
    while (std::getline(ss, path, separator)) {
        if (!path.empty()) {
            paths.push_back(path);
        }
    }
    
    return paths;
}

} // namespace cc::utils::env
