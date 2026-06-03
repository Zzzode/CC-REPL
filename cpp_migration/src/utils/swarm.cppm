// C++23 Swarm Utilities Module
// Provides swarm constants and spawn utilities for multi-agent systems
module;

#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>

export module cc.utils.swarm;

import cc.utils.string;

export namespace cc::utils::swarm {

// Simple shell quoting helper (wraps value in single quotes)
inline std::string shell_quote(std::string_view sv) {
    std::string result = "'";
    for (char c : sv) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += '\'';
    return result;
}


constexpr std::string_view TEAM_LEAD_NAME = "team-lead";
constexpr std::string_view SWARM_SESSION_NAME = "claude-swarm";
constexpr std::string_view SWARM_VIEW_WINDOW_NAME = "swarm-view";
constexpr std::string_view TMUX_COMMAND = "tmux";
constexpr std::string_view HIDDEN_SESSION_NAME = "claude-hidden";


constexpr std::string_view TEAMMATE_COMMAND_ENV_VAR = "CLAUDE_CODE_TEAMMATE_COMMAND";
constexpr std::string_view TEAMMATE_COLOR_ENV_VAR = "CLAUDE_CODE_AGENT_COLOR";
constexpr std::string_view PLAN_MODE_REQUIRED_ENV_VAR = "CLAUDE_CODE_PLAN_MODE_REQUIRED";


constexpr std::array<std::string_view, 12> TEAMMATE_ENV_VARS = {
    "CLAUDE_CODE_USE_BEDROCK",
    "CLAUDE_CODE_USE_VERTEX",
    "CLAUDE_CODE_USE_FOUNDRY",
    "ANTHROPIC_BASE_URL",
    "CLAUDE_CONFIG_DIR",
    "CLAUDE_CODE_REMOTE",
    "CLAUDE_CODE_REMOTE_MEMORY_DIR",
    "HTTPS_PROXY",
    "https_proxy",
    "HTTP_PROXY",
    "http_proxy",
    "NO_PROXY"
};


[[nodiscard]] inline auto get_swarm_socket_name() -> std::string {

    static std::string socket_name;
    if (socket_name.empty()) {


        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
        socket_name = std::format("claude-swarm-{}", millis);
    }
    return socket_name;
}


[[nodiscard]] inline auto get_teammate_command() -> std::string {

    if (const char* env = std::getenv(std::string(TEAMMATE_COMMAND_ENV_VAR).c_str())) {
        return std::string(env);
    }


    return "claude";
}


[[nodiscard]] inline auto build_inherited_env_vars() -> std::vector<std::string> {
    std::vector<std::string> env_vars;


    env_vars.emplace_back("CLAUDECODE=1");
    env_vars.emplace_back("CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1");


    for (const auto& var_name : TEAMMATE_ENV_VARS) {
        if (const char* value = std::getenv(std::string(var_name).c_str())) {
            env_vars.emplace_back(std::format("{}={}", var_name, shell_quote(std::string_view(value))));
        }
    }

    return env_vars;
}


struct BuildCliFlagsOptions {
    bool plan_mode_required = false;
    std::optional<std::string> permission_mode;
};

[[nodiscard]] inline auto build_inherited_cli_flags(const BuildCliFlagsOptions& options = {}) -> std::vector<std::string> {
    std::vector<std::string> flags;


    if (!options.plan_mode_required) {
        if (options.permission_mode == "bypassPermissions") {
            flags.emplace_back("--dangerously-skip-permissions");
        } else if (options.permission_mode == "acceptEdits") {
            flags.emplace_back("--permission-mode");
            flags.emplace_back("acceptEdits");
        }
    }




    return flags;
}


struct TeammateSpawnConfig {
    std::optional<std::string> agent_color;
    bool plan_mode_required = false;
    std::optional<std::string> permission_mode;
    std::vector<std::string> extra_args;
};

[[nodiscard]] inline auto build_teammate_command(const TeammateSpawnConfig& config = {}) -> std::string {
    std::vector<std::string> parts;


    for (const auto& env_var : build_inherited_env_vars()) {
        parts.push_back(env_var);
    }


    if (config.agent_color) {
        parts.push_back(std::format("{}={}", TEAMMATE_COLOR_ENV_VAR, *config.agent_color));
    }


    if (config.plan_mode_required) {
        parts.push_back(std::format("{}=true", PLAN_MODE_REQUIRED_ENV_VAR));
    }


    parts.push_back(get_teammate_command());


    auto flags = build_inherited_cli_flags({
        .plan_mode_required = config.plan_mode_required,
        .permission_mode = config.permission_mode
    });
    for (const auto& flag : flags) {
        parts.push_back(flag);
    }


    for (const auto& arg : config.extra_args) {
        parts.push_back(arg);
    }


    std::string command;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) command += " ";
        command += parts[i];
    }

    return command;
}

} // namespace cc::utils::swarm
