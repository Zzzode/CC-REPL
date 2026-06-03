module;

#include <filesystem>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <algorithm>
#include <cstdlib>

export module cc.utils.path_utils;

namespace fs = std::filesystem;

export namespace cc::utils {


inline fs::path normalize_path(const fs::path& p) {
    fs::path result = p.lexically_normal();

    std::error_code ec;
    if (fs::exists(result, ec)) {
        auto canonical = fs::canonical(result, ec);
        if (!ec) {
            return canonical;
        }
    }
    return result;
}


inline fs::path expand_tilde(std::string_view path_str) {
    if (path_str.empty() || path_str[0] != '~') {
        return fs::path{path_str};
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        return fs::path{path_str};
    }


    if (path_str.size() == 1) {
        return fs::path{home};
    }
    if (path_str[1] == '/') {
        return fs::path{home} / fs::path{path_str.substr(2)};
    }


    return fs::path{path_str};
}


inline std::string relative_to_home(const fs::path& p) {
    const char* home = std::getenv("HOME");
    if (!home) {
        return p.string();
    }

    fs::path home_path{home};
    std::error_code ec;
    auto rel = p.lexically_relative(home_path);
    if (rel.empty() || rel.string().starts_with("..")) {
        return p.string();
    }
    return "~/" + rel.string();
}


inline fs::path get_common_prefix(std::span<const fs::path> paths) {
    if (paths.empty()) {
        return {};
    }
    if (paths.size() == 1) {
        return paths[0].parent_path();
    }


    auto decompose = [](const fs::path& p) -> std::vector<std::string> {
        std::vector<std::string> parts;
        for (const auto& component : p) {
            parts.push_back(component.string());
        }
        return parts;
    };

    std::vector<std::string> common = decompose(paths[0]);

    for (size_t i = 1; i < paths.size(); ++i) {
        auto parts = decompose(paths[i]);
        size_t min_len = std::min(common.size(), parts.size());
        size_t match_count = 0;
        for (size_t j = 0; j < min_len; ++j) {
            if (common[j] != parts[j]) break;
            ++match_count;
        }
        common.resize(match_count);
    }


    fs::path result;
    for (const auto& part : common) {
        result /= part;
    }
    return result;
}


inline bool is_subpath(const fs::path& child, const fs::path& parent) {
    auto child_norm = child.lexically_normal();
    auto parent_norm = parent.lexically_normal();

    auto rel = child_norm.lexically_relative(parent_norm);

    if (rel.empty()) return false;
    auto rel_str = rel.string();
    return !rel_str.starts_with("..");
}


inline std::string ensure_trailing_slash(std::string path_str) {
    if (path_str.empty()) {
        return "/";
    }
    char sep = fs::path::preferred_separator;
    if (path_str.back() != sep && path_str.back() != '/') {
        path_str.push_back(sep);
    }
    return path_str;
}

} // namespace cc::utils
