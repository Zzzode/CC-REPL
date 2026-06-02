module;

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

export module cc.utils.cache_paths;

export namespace cc::utils::cache_paths {

constexpr std::size_t max_sanitized_length = 200;

struct CachePathSet {
    std::string base_logs;
    std::string errors;
    std::string messages;
    std::string mcp_logs;
};

[[nodiscard]] inline std::int32_t djb2_hash(std::string_view value) noexcept {
    std::int32_t hash = 0;
    for (unsigned char ch : value) {
        hash = static_cast<std::int32_t>((hash << 5) - hash + ch);
    }
    return hash;
}

[[nodiscard]] inline std::string to_base36(std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (value == 0) return "0";
    std::string out;
    while (value > 0) {
        out.push_back(digits[value % 36]);
        value /= 36;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

[[nodiscard]] inline std::string sanitize_path(std::string_view name) {
    std::string sanitized;
    sanitized.reserve(name.size());
    for (unsigned char ch : name) {
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        sanitized.push_back(alnum ? static_cast<char>(ch) : '-');
    }
    if (sanitized.size() <= max_sanitized_length) return sanitized;
    const auto hash = djb2_hash(name);
    const auto abs_hash = hash < 0 ? static_cast<std::uint32_t>(-(static_cast<std::int64_t>(hash))) : static_cast<std::uint32_t>(hash);
    return sanitized.substr(0, max_sanitized_length) + "-" + to_base36(abs_hash);
}

[[nodiscard]] inline std::string project_dir(std::string_view cwd) {
    return sanitize_path(cwd);
}

[[nodiscard]] inline std::string join_path(std::string left, std::string_view right) {
    while (!left.empty() && left.back() == '/') left.pop_back();
    if (left.empty()) return std::string(right);
    return left + "/" + std::string(right);
}

[[nodiscard]] inline CachePathSet build_cache_paths(std::string_view cache_base, std::string_view cwd, std::string_view server_name) {
    const auto project = project_dir(cwd);
    const auto base = join_path(std::string(cache_base), project);
    return CachePathSet{
        .base_logs = base,
        .errors = join_path(base, "errors"),
        .messages = join_path(base, "messages"),
        .mcp_logs = join_path(base, "mcp-logs-" + sanitize_path(server_name)),
    };
}

} // namespace cc::utils::cache_paths
