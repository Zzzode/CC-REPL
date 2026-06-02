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

// 常量定义
constexpr std::string_view TEAM_LEAD_NAME = "team-lead";
constexpr std::string_view SWARM_SESSION_NAME = "claude-swarm";
constexpr std::string_view SWARM_VIEW_WINDOW_NAME = "swarm-view";
constexpr std::string_view TMUX_COMMAND = "tmux";
constexpr std::string_view HIDDEN_SESSION_NAME = "claude-hidden";

// 环境变量名称
constexpr std::string_view TEAMMATE_COMMAND_ENV_VAR = "CLAUDE_CODE_TEAMMATE_COMMAND";
constexpr std::string_view TEAMMATE_COLOR_ENV_VAR = "CLAUDE_CODE_AGENT_COLOR";
constexpr std::string_view PLAN_MODE_REQUIRED_ENV_VAR = "CLAUDE_CODE_PLAN_MODE_REQUIRED";

// 需要转发给 teammate 的环境变量列表
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

// 获取 Swarm Socket 名称
[[nodiscard]] inline auto get_swarm_socket_name() -> std::string {
    // 使用进程 ID 确保唯一性
    static std::string socket_name;
    if (socket_name.empty()) {
        // 在实际环境中获取真正的 PID
        // 这里使用时间戳作为替代
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
        socket_name = std::format("claude-swarm-{}", millis);
    }
    return socket_name;
}

// 获取队友命令
[[nodiscard]] inline auto get_teammate_command() -> std::string {
    // 检查环境变量
    if (const char* env = std::getenv(std::string(TEAMMATE_COMMAND_ENV_VAR).c_str())) {
        return std::string(env);
    }

    // 默认返回当前可执行文件路径（简化处理）
    return "claude";
}

// 构建继承的环境变量字符串
[[nodiscard]] inline auto build_inherited_env_vars() -> std::vector<std::string> {
    std::vector<std::string> env_vars;

    // 总是添加这些
    env_vars.emplace_back("CLAUDECODE=1");
    env_vars.emplace_back("CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1");

    // 添加需要继承的环境变量
    for (const auto& var_name : TEAMMATE_ENV_VARS) {
        if (const char* value = std::getenv(std::string(var_name).c_str())) {
            env_vars.emplace_back(std::format("{}={}", var_name, shell_quote(std::string_view(value))));
        }
    }

    return env_vars;
}

// 构建继承的 CLI 标志
struct BuildCliFlagsOptions {
    bool plan_mode_required = false;
    std::optional<std::string> permission_mode;
};

[[nodiscard]] inline auto build_inherited_cli_flags(const BuildCliFlagsOptions& options = {}) -> std::vector<std::string> {
    std::vector<std::string> flags;

    // 处理权限模式
    if (!options.plan_mode_required) {
        if (options.permission_mode == "bypassPermissions") {
            flags.emplace_back("--dangerously-skip-permissions");
        } else if (options.permission_mode == "acceptEdits") {
            flags.emplace_back("--permission-mode");
            flags.emplace_back("acceptEdits");
        }
    }

    // 注意：其他标志（如 --model, --settings, --plugin-dir）需要从状态中获取
    // 这里提供一个基础实现

    return flags;
}

// 构建完整的队友命令
struct TeammateSpawnConfig {
    std::optional<std::string> agent_color;
    bool plan_mode_required = false;
    std::optional<std::string> permission_mode;
    std::vector<std::string> extra_args;
};

[[nodiscard]] inline auto build_teammate_command(const TeammateSpawnConfig& config = {}) -> std::string {
    std::vector<std::string> parts;

    // 添加环境变量
    for (const auto& env_var : build_inherited_env_vars()) {
        parts.push_back(env_var);
    }

    // 添加颜色环境变量
    if (config.agent_color) {
        parts.push_back(std::format("{}={}", TEAMMATE_COLOR_ENV_VAR, *config.agent_color));
    }

    // 添加计划模式环境变量
    if (config.plan_mode_required) {
        parts.push_back(std::format("{}=true", PLAN_MODE_REQUIRED_ENV_VAR));
    }

    // 添加基本命令
    parts.push_back(get_teammate_command());

    // 添加 CLI 标志
    auto flags = build_inherited_cli_flags({
        .plan_mode_required = config.plan_mode_required,
        .permission_mode = config.permission_mode
    });
    for (const auto& flag : flags) {
        parts.push_back(flag);
    }

    // 添加额外参数
    for (const auto& arg : config.extra_args) {
        parts.push_back(arg);
    }

    // 连接成单个命令字符串
    std::string command;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) command += " ";
        command += parts[i];
    }

    return command;
}

} // namespace cc::utils::swarm
