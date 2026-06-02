module;

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.plugin_versioning;

import cc.utils.crypto;

export namespace cc::utils::plugin_versioning {

[[nodiscard]] inline std::vector<std::string_view> split_non_empty(std::string_view value, char delim) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto pos = value.find(delim, start);
        const auto end = pos == std::string_view::npos ? value.size() : pos;
        if (end > start) out.push_back(value.substr(start, end - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

[[nodiscard]] inline std::optional<std::string> get_version_from_path(std::string_view install_path) {
    const auto parts = split_non_empty(install_path, '/');
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i] == "cache" && parts[i - 1] == "plugins") {
            if (i + 3 < parts.size()) return std::string(parts[i + 3]);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool is_versioned_path(std::string_view path) {
    return get_version_from_path(path).has_value();
}

[[nodiscard]] inline std::string normalize_git_subdir_path(std::string_view path) {
    std::string norm(path);
    std::replace(norm.begin(), norm.end(), '\\', '/');
    if (norm.starts_with("./")) norm.erase(0, 2);
    while (!norm.empty() && norm.back() == '/') norm.pop_back();
    return norm;
}

[[nodiscard]] inline std::string derive_plugin_version(
    std::optional<std::string_view> manifest_version,
    std::optional<std::string_view> provided_version,
    std::optional<std::string_view> git_commit_sha,
    std::string_view source_type = {},
    std::string_view source_path = {}
) {
    if (manifest_version && !manifest_version->empty()) return std::string(*manifest_version);
    if (provided_version && !provided_version->empty()) return std::string(*provided_version);
    if (git_commit_sha && !git_commit_sha->empty()) {
        std::string short_sha(git_commit_sha->substr(0, std::min<std::size_t>(12, git_commit_sha->size())));
        if (source_type == "git-subdir") {
            const auto normalized = normalize_git_subdir_path(source_path);
            short_sha.push_back('-');
            short_sha.append(cc::utils::crypto::sha256(normalized).substr(0, 8));
        }
        return short_sha;
    }
    return "unknown";
}

} // namespace cc::utils::plugin_versioning
