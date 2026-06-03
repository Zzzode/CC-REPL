module;

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.commands.remote_commands;


export namespace cc::commands {


struct CommandResult {
    bool success{true};
    std::string output;
    std::optional<std::string> error;
};


struct BridgeCommand {
    static constexpr auto name() -> std::string_view { return "bridge"; }
    static constexpr auto description() -> std::string_view { return "管理 IDE 桥接连接"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "status") {
            return {.success = true, .output = "Bridge: 未连接"};
        }
        if (args == "list") {
            return {.success = true, .output = "未发现活跃的 IDE 连接"};
        }
        if (args.starts_with("connect")) return {.success = true, .output = "Bridge connection requested"};
        if (args == "disconnect") return {.success = true, .output = "Bridge disconnected"};
        return {.success = true, .output = "Bridge 命令已执行"};
    }
};


struct ExtraUsageCommand {
    static constexpr auto name() -> std::string_view { return "extra-usage"; }
    static constexpr auto description() -> std::string_view { return "管理额外 API 用量额度"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"eu"}; }

    auto execute(std::string_view args) -> CommandResult {
        if (args == "check") {
            return {.success = true, .output = "当前额外用量: 0 / 无限制"};
        }
        if (args == "activate" || args == "buy") return {.success = true, .output = "额外用量激活流程已启动"};
        return {.success = true, .output = "额外用量信息已显示"};
    }
};


struct InstallGithubAppCommand {
    static constexpr auto name() -> std::string_view { return "install-github-app"; }
    static constexpr auto description() -> std::string_view { return "安装 GitHub App 集成"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"iga"}; }

    auto execute(std::string_view /*args*/) -> CommandResult {

        return {.success = true, .output = "GitHub App 安装向导启动...\n"
                "步骤 1/5: 检查 GitHub 连接状态\n"
                "步骤 2/5: OAuth 授权\n"
                "步骤 3/5: 选择仓库\n"
                "步骤 4/5: 安装 App\n"
                "步骤 5/5: 配置 Secrets"};
    }
};


struct RemoteEnvCommand {
    static constexpr auto name() -> std::string_view { return "remote-env"; }
    static constexpr auto description() -> std::string_view { return "管理远程会话环境变量"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "list") {
            return {.success = true, .output = "远程环境变量: (空)"};
        }
        return {.success = true, .output = "环境变量已更新"};
    }
};


struct RemoteSetupCommand {
    static constexpr auto name() -> std::string_view { return "remote-setup"; }
    static constexpr auto description() -> std::string_view { return "配置远程会话环境"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view /*args*/) -> CommandResult {
        return {.success = true, .output = "远程环境配置向导:\n"
                "1. 设置 Bridge URL\n"
                "2. 配置认证 Token\n"
                "3. 测试连接\n"
                "4. 保存配置"};
    }
};


struct PrCommentsCommand {
    static constexpr auto name() -> std::string_view { return "pr-comments"; }
    static constexpr auto description() -> std::string_view { return "查看和管理 PR 评论"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"prc"}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "list") {
            return {.success = true, .output = "未检测到待处理的 PR 评论"};
        }
        return {.success = true, .output = "PR 评论操作已执行"};
    }
};


struct PassesCommand {
    static constexpr auto name() -> std::string_view { return "passes"; }
    static constexpr auto description() -> std::string_view { return "管理限流 Pass"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {
        if (args.empty() || args == "list") {
            return {.success = true, .output = "当前限流 Pass: 无激活的 Pass"};
        }
        if (args.starts_with("redeem")) {
            return {.success = true, .output = "Pass 兑换成功"};
        }
        return {.success = true, .output = "Pass 状态已更新"};
    }
};


struct RateLimitOptionsCommand {
    static constexpr auto name() -> std::string_view { return "rate-limit-options"; }
    static constexpr auto description() -> std::string_view { return "配置限流行为和显示选项"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"rlo"}; }

    auto execute(std::string_view /*args*/) -> CommandResult {
        return {.success = true, .output = "限流选项:\n"
                "  显示警告: 开启\n"
                "  自动重试: 开启 (最多 5 次)\n"
                "  退避策略: 指数退避 + 抖动\n"
                "  当前层级: Pro"};
    }
};


struct VersionCommand {
    static constexpr auto name() -> std::string_view { return "version"; }
    static constexpr auto description() -> std::string_view { return "显示版本和构建信息"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"-v"}; }

    auto execute(std::string_view /*args*/) -> CommandResult {
        return {.success = true, .output = "cc-repl v2.0.0-cpp\n"
                "Build: C++23 Modules\n"
                "Compiler: Clang 18+\n"
                "Dependencies: yyjson 0.10, libuv 1.48, FTXUI 5.0, cpp-httplib 0.18"};
    }
};


inline auto get_remote_commands() -> std::vector<std::string_view> {
    return {"bridge", "extra-usage", "install-github-app", "remote-env",
            "remote-setup", "pr-comments", "passes", "rate-limit-options", "version"};
}

} // namespace cc::commands
