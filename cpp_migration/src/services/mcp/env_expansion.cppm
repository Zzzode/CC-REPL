module;
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
export module cc.services.mcp.env_expansion;

export namespace cc::services::mcp {

// Expand environment variables in a string (${VAR} syntax)
auto expand_env_vars(std::string_view input) -> std::string {
    std::string result;
    result.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        // Look for ${...} pattern
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            auto end = input.find('}', i + 2);
            if (end != std::string_view::npos) {
                auto var_name = input.substr(i + 2, end - i - 2);
                // Resolve from environment
                if (auto* val = std::getenv(std::string(var_name).c_str())) {
                    result += val;
                }
                i = end + 1;
                continue;
            }
        }
        result += input[i];
        ++i;
    }
    return result;
}

// Expand environment variables in all values of a config map
auto expand_mcp_config_vars(std::map<std::string, std::string> config)
    -> std::map<std::string, std::string> {
    for (auto& [key, value] : config) {
        value = expand_env_vars(value);
    }
    return config;
}

// Get all available expansion variables from environment
auto get_expansion_vars() -> std::map<std::string, std::string> {
    std::map<std::string, std::string> vars;
    // Standard MCP-relevant variables
    const char* known_vars[] = {
        "HOME", "USER", "PATH", "CLAUDE_CONFIG_DIR",
        "MCP_SERVER_PATH", "MCP_AUTH_TOKEN"
    };
    for (const auto* var : known_vars) {
        if (auto* val = std::getenv(var)) {
            vars[var] = val;
        }
    }
    return vars;
}

} // namespace cc::services::mcp
