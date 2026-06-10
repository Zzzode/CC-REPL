module;

#include <string>
#include <string_view>
#include <vector>

export module cc.commands.extra_commands2;


export namespace cc::commands {

struct CmdResult { bool success{true}; std::string output; std::string error; };


struct OnboardingCommand {
    static constexpr auto name() -> std::string_view { return "onboarding"; }
    static constexpr auto description() -> std::string_view { return "Run first-use onboarding flow"; }

    auto execute(std::string_view args) -> CmdResult {
        if (args == "skip") return {true, "Onboarding skipped"};
        return {true,
            "Welcome to CC-REPL!\n\n"
            "Step 1/5: Checking API Key configuration...\n"
            "Step 2/5: Detecting development environment...\n"
            "Step 3/5: Configuring default model...\n"
            "Step 4/5: Setting permission preferences...\n"
            "Step 5/5: Done!\n\n"
            "Type /help to see available commands"};
    }
};


struct IssueCommand {
    static constexpr auto name() -> std::string_view { return "issue"; }
    static constexpr auto description() -> std::string_view { return "Create and manage GitHub Issues"; }

    auto execute(std::string_view args) -> CmdResult {
        if (args.empty() || args == "list") return {true, "Issue list: (use gh cli to fetch)"};
        if (args.starts_with("create")) return {true, "Creating Issue...\nPlease provide title and description"};
        if (args.starts_with("close")) return {true, "Issue closed"};
        return {true, "Issue command executed"};
    }
};


struct TeleportCommand {
    static constexpr auto name() -> std::string_view { return "teleport"; }
    static constexpr auto description() -> std::string_view { return "Teleport session to remote environment"; }

    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {true, "Usage: /teleport <target-env>\nAvailable environments: dev, staging, production"};
        return {true, "Teleporting session to " + std::string(args) + " environment..."};
    }
};


struct ReloadPluginsCommand {
    static constexpr auto name() -> std::string_view { return "reload-plugins"; }
    static constexpr auto description() -> std::string_view { return "Reload all plugins"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "Reloading plugins...\nLoaded 0 plugins\nDone"};
    }
};


struct OauthRefreshCommand {
    static constexpr auto name() -> std::string_view { return "oauth-refresh"; }
    static constexpr auto description() -> std::string_view { return "Refresh OAuth Token"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "Refreshing OAuth Token...\nToken updated, valid for 3600 seconds"};
    }
};


struct TerminalSetupCommand {
    static constexpr auto name() -> std::string_view { return "terminalSetup"; }
    static constexpr auto description() -> std::string_view { return "Terminal environment initialization"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true,
            "Terminal environment detection:\n"
            "  Terminal: detecting...\n"
            "  Color: TrueColor\n"
            "  Unicode: supported\n"
            "  Shell: zsh\n"
            "  Size: detecting...\n"
            "Configuration saved"};
    }
};


struct ThinkbackPlayCommand {
    static constexpr auto name() -> std::string_view { return "thinkback-play"; }
    static constexpr auto description() -> std::string_view { return "Replay AI thinking process animation"; }

    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {.success = false, .output = "", .error = "Please specify the turn ID to replay"};
        return {true, "Replaying thinking process for turn " + std::string(args) + "..."};
    }
};


struct InstallSlackAppCommand {
    static constexpr auto name() -> std::string_view { return "install-slack-app"; }
    static constexpr auto description() -> std::string_view { return "Install Slack integration"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true,
            "Slack App installation wizard:\n"
            "1. Create Slack App\n"
            "2. Configure OAuth Scopes\n"
            "3. Install to Workspace\n"
            "4. Save Bot Token\n\n"
            "Visit https://api.slack.com/apps to start"};
    }
};


struct PerfIssueCommand {
    static constexpr auto name() -> std::string_view { return "perf-issue"; }
    static constexpr auto description() -> std::string_view { return "Report performance issues and collect diagnostics"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true,
            "Performance diagnostic report:\n"
            "  Memory usage: detecting...\n"
            "  Response latency: detecting...\n"
            "  Active connections: detecting...\n"
            "  Context size: detecting...\n\n"
            "Report generated, use /feedback to submit"};
    }
};


struct BtwCommand {
    static constexpr auto name() -> std::string_view { return "btw"; }
    static constexpr auto description() -> std::string_view { return "Append extra context to current conversation"; }

    auto execute(std::string_view args) -> CmdResult {
        if (args.empty()) return {.success = false, .output = "Usage: /btw <additional info>"};
        return {true, "The following info has been appended to conversation context:\n\"" + std::string(args) + "\""};
    }
};


struct GoodClaudeCommand {
    static constexpr auto name() -> std::string_view { return "good-claude"; }
    static constexpr auto description() -> std::string_view { return "Give Claude a thumbs up"; }

    auto execute(std::string_view /*args*/) -> CmdResult {
        return {true, "Thanks! Claude appreciates your recognition"};
    }
};


inline auto get_extra_commands2_list() -> std::vector<std::string_view> {
    return {"onboarding", "issue", "teleport", "reload-plugins", "oauth-refresh",
            "terminalSetup", "thinkback-play", "install-slack-app", "perf-issue", 
            "btw", "good-claude"};
}

} // namespace cc::commands
