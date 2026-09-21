module;

#include <string>
#include <string_view>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <array>

export module cc.utils.git_settings;
import cc.utils.bash_execution;

export namespace cc::utils {

namespace detail {

// Helper: run a git config command and return trimmed output
inline std::optional<std::string> git_config_get(std::string_view key) {
    std::string cmd = "git config --get " + std::string(key) + " 2>/dev/null";
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) return std::nullopt;

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0 || output.empty()) return std::nullopt;

    // Trim trailing whitespace/newline
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
        output.pop_back();
    }

    return output.empty() ? std::nullopt : std::optional(std::move(output));
}

} // namespace detail

// Get a git config value by key
inline std::optional<std::string> get_git_config(std::string_view key) {
    return detail::git_config_get(key);
}

// Get a git config boolean value
inline std::optional<bool> get_git_config_bool(std::string_view key) {
    auto value = detail::git_config_get(key);
    if (!value) return std::nullopt;

    // Git recognizes these as true
    if (*value == "true" || *value == "yes" || *value == "on" || *value == "1") return true;
    if (*value == "false" || *value == "no" || *value == "off" || *value == "0") return false;
    return std::nullopt;
}

// Get the configured user name
inline std::optional<std::string> get_git_user_name() {
    return detail::git_config_get("user.name");
}

// Get the configured user email
inline std::optional<std::string> get_git_user_email() {
    return detail::git_config_get("user.email");
}

// Get the configured editor
inline std::optional<std::string> get_git_editor() {
    // Priority: GIT_EDITOR env > core.editor config > VISUAL env > EDITOR env
    if (auto* env = std::getenv("GIT_EDITOR")) return std::string(env);
    if (auto editor = detail::git_config_get("core.editor")) return editor;
    if (auto* env = std::getenv("VISUAL")) return std::string(env);
    if (auto* env = std::getenv("EDITOR")) return std::string(env);
    return std::nullopt;
}

// Get the URL of a remote (default "origin")
inline std::optional<std::string> get_remote_url(std::string_view remote = "origin") {
    std::string key = "remote." + std::string(remote) + ".url";
    return detail::git_config_get(key);
}

} // namespace cc::utils
