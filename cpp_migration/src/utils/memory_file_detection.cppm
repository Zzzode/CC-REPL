module;

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.memory_file_detection;

export namespace cc::utils::memory_file_detection {

enum class SessionFileType {
    SessionMemory,
    SessionTranscript,
};

enum class MemoryScope {
    Personal,
    Team,
};

struct MemoryDetectionConfig {
    std::string claude_config_home_dir;
    std::string memory_base_dir;
    std::string auto_mem_path;
    bool auto_memory_enabled = false;
    bool team_memory_enabled = false;
    std::string team_mem_path;
    bool windows = false;
};

namespace detail {
    [[nodiscard]] inline std::string to_posix(std::string value) {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }

    [[nodiscard]] inline std::string trim_trailing_slashes(std::string value) {
        while (value.size() > 1 && (value.back() == '/' || value.back() == '\\')) value.pop_back();
        return value;
    }

    [[nodiscard]] inline std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    [[nodiscard]] inline std::string windows_path_to_posix_path(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
            std::string out = "/";
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(path[0]))));
            if (path.size() > 2 && path[2] != '/') out.push_back('/');
            out += path.substr(2);
            return out;
        }
        return path;
    }

    [[nodiscard]] inline std::string comparable(std::string value, bool windows) {
        value = to_posix(value);
        if (windows) value = lower(value);
        return value;
    }

    [[nodiscard]] inline bool starts_with_path(std::string_view path, std::string_view prefix) {
        if (prefix.empty()) return false;
        return path.starts_with(prefix);
    }

    [[nodiscard]] inline std::string normalize_path(std::string path) {
        return std::filesystem::path(path).lexically_normal().string();
    }

    [[nodiscard]] inline bool contains_any_agent_memory_dir(std::string_view cmp) {
        return cmp.find("/agent-memory/") != std::string_view::npos ||
               cmp.find("/agent-memory-local/") != std::string_view::npos;
    }

    [[nodiscard]] inline std::string posix_path_to_windows_path(std::string path) {
        if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == '/') {
            std::string out;
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(path[1]))));
            out += ':';
            out += path.substr(2);
            std::replace(out.begin(), out.end(), '/', '\\');
            return out;
        }
        return path;
    }
} // namespace detail

[[nodiscard]] inline std::optional<SessionFileType> detect_session_file_type(
    std::string_view file_path,
    const MemoryDetectionConfig& config
) {
    const std::string normalized = detail::comparable(std::string(file_path), config.windows);
    const std::string config_dir = detail::comparable(config.claude_config_home_dir, config.windows);
    if (!detail::starts_with_path(normalized, config_dir)) return std::nullopt;
    if (normalized.find("/session-memory/") != std::string::npos && normalized.ends_with(".md")) {
        return SessionFileType::SessionMemory;
    }
    if (normalized.find("/projects/") != std::string::npos && normalized.ends_with(".jsonl")) {
        return SessionFileType::SessionTranscript;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<SessionFileType> detect_session_pattern_type(std::string_view pattern) {
    const std::string normalized = detail::to_posix(std::string(pattern));
    if (normalized.find("session-memory") != std::string::npos &&
        (normalized.find(".md") != std::string::npos || normalized.ends_with('*'))) {
        return SessionFileType::SessionMemory;
    }
    if (normalized.find(".jsonl") != std::string::npos ||
        (normalized.find("projects") != std::string::npos && normalized.find("*.jsonl") != std::string::npos)) {
        return SessionFileType::SessionTranscript;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool is_auto_mem_file(std::string_view file_path, const MemoryDetectionConfig& config) {
    if (!config.auto_memory_enabled || config.auto_mem_path.empty()) return false;
    const auto path = detail::comparable(std::string(file_path), config.windows);
    const auto auto_mem = detail::comparable(detail::trim_trailing_slashes(config.auto_mem_path), config.windows);
    return path == auto_mem || path.starts_with(auto_mem + "/");
}

[[nodiscard]] inline bool is_team_mem_file(std::string_view file_path, const MemoryDetectionConfig& config) {
    if (!config.team_memory_enabled || config.team_mem_path.empty()) return false;
    const auto path = detail::comparable(std::string(file_path), config.windows);
    const auto team = detail::comparable(detail::trim_trailing_slashes(config.team_mem_path), config.windows);
    return path == team || path.starts_with(team + "/");
}

[[nodiscard]] inline std::optional<MemoryScope> memory_scope_for_path(std::string_view file_path, const MemoryDetectionConfig& config) {
    if (is_team_mem_file(file_path, config)) return MemoryScope::Team;
    if (is_auto_mem_file(file_path, config)) return MemoryScope::Personal;
    return std::nullopt;
}

[[nodiscard]] inline bool is_auto_managed_memory_file(std::string_view file_path, const MemoryDetectionConfig& config) {
    if (is_auto_mem_file(file_path, config)) return true;
    if (is_team_mem_file(file_path, config)) return true;
    if (detect_session_file_type(file_path, config).has_value()) return true;
    if (config.auto_memory_enabled && detail::contains_any_agent_memory_dir(detail::comparable(std::string(file_path), config.windows))) return true;
    return false;
}

[[nodiscard]] inline bool is_memory_directory(std::string_view dir_path, const MemoryDetectionConfig& config) {
    const std::string normalized = detail::normalize_path(std::string(dir_path));
    const std::string cmp = detail::comparable(normalized, config.windows);
    if (config.auto_memory_enabled && detail::contains_any_agent_memory_dir(cmp)) return true;
    if (config.team_memory_enabled && is_team_mem_file(normalized, config)) return true;
    if (config.auto_memory_enabled && !config.auto_mem_path.empty()) {
        const auto auto_mem_dir = detail::comparable(detail::trim_trailing_slashes(config.auto_mem_path), config.windows);
        const auto auto_mem_path = detail::comparable(config.auto_mem_path, config.windows);
        if (cmp == auto_mem_dir || cmp.starts_with(auto_mem_path)) return true;
    }

    const std::string config_dir = detail::comparable(config.claude_config_home_dir, config.windows);
    const std::string memory_base = detail::comparable(config.memory_base_dir, config.windows);
    const bool under_config = detail::starts_with_path(cmp, config_dir);
    const bool under_memory_base = detail::starts_with_path(cmp, memory_base);
    if (!under_config && !under_memory_base) return false;
    if (cmp.find("/session-memory/") != std::string::npos || cmp.ends_with("/session-memory")) return true;
    if (under_config && (cmp.find("/projects/") != std::string::npos || cmp.ends_with("/projects"))) return true;
    if (config.auto_memory_enabled && (cmp.find("/memory/") != std::string::npos || cmp.ends_with("/memory"))) return true;
    return false;
}

[[nodiscard]] inline bool is_shell_command_targeting_memory(std::string_view command, const MemoryDetectionConfig& config) {
    const std::string command_cmp = detail::comparable(std::string(command), config.windows);
    std::vector<std::string> dirs = {config.claude_config_home_dir, config.memory_base_dir};
    if (config.auto_memory_enabled && !config.auto_mem_path.empty()) dirs.push_back(detail::trim_trailing_slashes(config.auto_mem_path));

    bool mentions_memory_root = false;
    for (const auto& dir : dirs) {
        if (!dir.empty() && command_cmp.find(detail::comparable(dir, config.windows)) != std::string::npos) {
            mentions_memory_root = true;
            break;
        }
        if (config.windows && !dir.empty() && command_cmp.find(detail::comparable(detail::windows_path_to_posix_path(dir), true)) != std::string::npos) {
            mentions_memory_root = true;
            break;
        }
    }
    if (!mentions_memory_root) return false;

    static const std::regex path_regex(R"((?:[A-Za-z]:[/\\]|/)[^\s'"]+)");
    const std::string input(command);
    for (std::sregex_iterator it(input.begin(), input.end(), path_regex), end; it != end; ++it) {
        std::string clean = it->str();
        while (!clean.empty() && (clean.back() == ',' || clean.back() == ';' || clean.back() == '|' || clean.back() == '&' || clean.back() == '>')) {
            clean.pop_back();
        }
        const std::string native = config.windows ? detail::posix_path_to_windows_path(clean) : clean;
        if (is_auto_managed_memory_file(native, config) || is_memory_directory(native, config)) return true;
    }
    return false;
}

[[nodiscard]] inline bool is_auto_managed_memory_pattern(std::string_view pattern, const MemoryDetectionConfig& config) {
    if (detect_session_pattern_type(pattern).has_value()) return true;
    const std::string normalized = detail::to_posix(std::string(pattern));
    return config.auto_memory_enabled &&
           (normalized.find("agent-memory/") != std::string::npos || normalized.find("agent-memory-local/") != std::string::npos);
}

} // namespace cc::utils::memory_file_detection
