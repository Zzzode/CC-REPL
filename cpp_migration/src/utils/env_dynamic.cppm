module;
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.env_dynamic;

extern "C" char** environ;

export namespace cc::utils {

// Read environment variable in real-time (not cached)
std::optional<std::string> get_env_dynamic(std::string_view key) {
    const char* val = std::getenv(std::string(key).c_str());
    if (val) return std::string(val);
    return std::nullopt;
}

// Set an environment variable in the current process
void set_env_dynamic(std::string_view key, std::string_view value) {
    setenv(std::string(key).c_str(), std::string(value).c_str(), 1);
}

// Unset an environment variable in the current process
void unset_env_dynamic(std::string_view key) {
    unsetenv(std::string(key).c_str());
}

// Collect all CLAUDE_* and ANTHROPIC_* environment variables
std::map<std::string, std::string> get_all_claude_env_vars() {
    std::map<std::string, std::string> result;

    for (char** env = environ; *env != nullptr; ++env) {
        std::string_view entry(*env);
        auto eq = entry.find('=');
        if (eq == std::string_view::npos) continue;

        std::string_view key = entry.substr(0, eq);
        std::string_view value = entry.substr(eq + 1);

        if (key.starts_with("CLAUDE_") || key.starts_with("ANTHROPIC_")) {
            result[std::string(key)] = std::string(value);
        }
    }

    return result;
}

} // namespace cc::utils
