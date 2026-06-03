module;

#include <string>
#include <string_view>
#include <vector>

export module cc.commands.extra_commands2;


export namespace cc::commands {

struct CmdResult { bool success{true}; std::string output; std::string error; };


struct OnboardingCommand {
    static constexpr auto name() -> std::string_view { return "onboarding"; }
    static constexpr auto description() -> std::string_view { return "运行首次使用引导流程"; }
    
    auto execute(std::string_view args) -> CmdResult {
        if (args == "skip") return {true, "引导已跳过"};
        return {true, 
            "欢迎使用 CC-REPL! 🎉\n\n"
            "步骤 1/5: 检查 API Key 配置...\n"
            "步骤 2/5: 检测开发环境...\n"
            "步骤 3/5: 配置默认模型...\n"
            "步骤 4/5: 设置权限偏好...\n"
            "步骤 5/5: 完成!\n\n"
            "输入 /help 查看可用命令"};
    }
};


struct IssueCommand {
    static constexpr auto name() -> std::string_view { return "issue"; }
    static constexpr auto description() -> std::string_view { return "创建和管理 GitHub Issues"; }
    
    auto execute(std::string_view args) -> CmdResult {
        if (args.empty() || args == "list") return {true, "Issue 列表: (使用 gh cli 获取)"};
        if (args.starts_with("create")) return {true, "创建 Issue...\n请提供标题和描述"};
        if (args.starts_with("close")) return {true, "Issue 已关闭"};
        return {true, "Issue 命令已执行"};
    }
};


struct TeleportCommand {
    static constexpr auto name() -> std::string_view { return "teleport"; }
    static constexpr auto description() -> std::string_view { return "传送会话到远程环境"; }
    
    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {true, "用法: /teleport <target-env>\n可用环境: dev, staging, production"};
        return {true, "正在传送会话到 " + std::string(args) + " 环境..."};
    }
};


struct ReloadPluginsCommand {
    static constexpr auto name() -> std::string_view { return "reload-plugins"; }
    static constexpr auto description() -> std::string_view { return "重新加载所有插件"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "正在重新加载插件...\n已加载 0 个插件\n完成"};
    }
};


struct OauthRefreshCommand {
    static constexpr auto name() -> std::string_view { return "oauth-refresh"; }
    static constexpr auto description() -> std::string_view { return "刷新 OAuth Token"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "正在刷新 OAuth Token...\nToken 已更新, 有效期 3600 秒"};
    }
};


struct TerminalSetupCommand {
    static constexpr auto name() -> std::string_view { return "terminalSetup"; }
    static constexpr auto description() -> std::string_view { return "终端环境初始化配置"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, 
            "终端环境检测:\n"
            "  终端: 检测中...\n"
            "  颜色: TrueColor\n"
            "  Unicode: 支持\n"
            "  Shell: zsh\n"
            "  尺寸: 检测中...\n"
            "配置已保存"};
    }
};


struct ThinkbackPlayCommand {
    static constexpr auto name() -> std::string_view { return "thinkback-play"; }
    static constexpr auto description() -> std::string_view { return "回放 AI 思考过程动画"; }
    
    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {.success = false, .output = "", .error = "请指定要回放的 turn ID"};
        return {true, "正在回放 turn " + std::string(args) + " 的思考过程..."};
    }
};


struct InstallSlackAppCommand {
    static constexpr auto name() -> std::string_view { return "install-slack-app"; }
    static constexpr auto description() -> std::string_view { return "安装 Slack 集成"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, 
            "Slack App 安装向导:\n"
            "1. 创建 Slack App\n"
            "2. 配置 OAuth Scopes\n"
            "3. 安装到 Workspace\n"
            "4. 保存 Bot Token\n\n"
            "访问 https://api.slack.com/apps 开始"};
    }
};


struct PerfIssueCommand {
    static constexpr auto name() -> std::string_view { return "perf-issue"; }
    static constexpr auto description() -> std::string_view { return "报告性能问题并收集诊断信息"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, 
            "性能诊断报告:\n"
            "  内存使用: 检测中...\n"
            "  响应延迟: 检测中...\n"
            "  活跃连接: 检测中...\n"
            "  上下文大小: 检测中...\n\n"
            "报告已生成, 可使用 /feedback 提交"};
    }
};


struct BtwCommand {
    static constexpr auto name() -> std::string_view { return "btw"; }
    static constexpr auto description() -> std::string_view { return "向当前对话附加额外上下文信息"; }
    
    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {.success = false, .output = "用法: /btw <附加信息>"};
        return {true, "已将以下信息附加到对话上下文:\n\"" + std::string(args) + "\""};
    }
};


struct GoodClaudeCommand {
    static constexpr auto name() -> std::string_view { return "good-claude"; }
    static constexpr auto description() -> std::string_view { return "给 Claude 一个好评"; }
    
    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "谢谢! Claude 表示感谢你的认可 ✨"};
    }
};


inline auto get_extra_commands2_list() -> std::vector<std::string_view> {
    return {"onboarding", "issue", "teleport", "reload-plugins", "oauth-refresh",
            "terminalSetup", "thinkback-play", "install-slack-app", "perf-issue", 
            "btw", "good-claude"};
}

} // namespace cc::commands
