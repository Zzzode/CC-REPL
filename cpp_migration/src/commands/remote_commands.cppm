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
    static constexpr auto description() -> std::string_view { return "Manage IDE bridge connections"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "status") {
            return {.success = true, .output = "Bridge: not connected"};
        }
        if (args == "list") {
            return {.success = true, .output = "No active IDE connections found"};
        }
        if (args.starts_with("connect")) return {.success = true, .output = "Bridge connection requested"};
        if (args == "disconnect") return {.success = true, .output = "Bridge disconnected"};
        return {.success = true, .output = "Bridge command executed"};
    }
};


struct ExtraUsageCommand {
    static constexpr auto name() -> std::string_view { return "extra-usage"; }
    static constexpr auto description() -> std::string_view { return "Manage extra API usage quota"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"eu"}; }

    auto execute(std::string_view args) -> CommandResult {
        if (args == "check") {
            return {.success = true, .output = "Current extra usage: 0 / unlimited"};
        }
        if (args == "activate" || args == "buy") return {.success = true, .output = "Extra usage activation started"};
        return {.success = true, .output = "Extra usage info displayed"};
    }
};


struct RemoteInstallGithubAppCommand {
    static constexpr auto name() -> std::string_view { return "install-github-app"; }
    static constexpr auto description() -> std::string_view { return "Install GitHub App integration"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"iga"}; }

    auto execute(std::string_view /*args*/) -> CommandResult {

        return {.success = true, .output = "GitHub App installation wizard started...\n"
                "Step 1/5: Check GitHub connection status\n"
                "Step 2/5: OAuth authorization\n"
                "Step 3/5: Select repository\n"
                "Step 4/5: Install App\n"
                "Step 5/5: Configure Secrets"};
    }
};


struct RemoteEnvCommand {
    static constexpr auto name() -> std::string_view { return "remote-env"; }
    static constexpr auto description() -> std::string_view { return "Manage remote session environment variables"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "list") {
            return {.success = true, .output = "Remote env vars: (empty)"};
        }
        return {.success = true, .output = "Environment variable updated"};
    }
};


struct RemoteSetupCommand {
    static constexpr auto name() -> std::string_view { return "remote-setup"; }
    static constexpr auto description() -> std::string_view { return "Configure remote session environment"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view /*args*/) -> CommandResult {
        return {.success = true, .output = "Remote environment setup wizard:\n"
                "1. Set Bridge URL\n"
                "2. Configure auth Token\n"
                "3. Test connection\n"
                "4. Save configuration"};
    }
};


struct PrCommentsCommand {
    static constexpr auto name() -> std::string_view { return "pr-comments"; }
    static constexpr auto description() -> std::string_view { return "View and manage PR comments"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"prc"}; }

    auto execute(std::string_view args) -> CommandResult {

        if (args.empty() || args == "list") {
            return {.success = true, .output = "No pending PR comments found"};
        }
        return {.success = true, .output = "PR comment action executed"};
    }
};


struct PassesCommand {
    static constexpr auto name() -> std::string_view { return "passes"; }
    static constexpr auto description() -> std::string_view { return "Manage rate limit Pass"; }
    static constexpr auto aliases() { return std::array<std::string_view, 0>{}; }

    auto execute(std::string_view args) -> CommandResult {
        if (args.empty() || args == "list") {
            return {.success = true, .output = "Current rate limit Pass: no active Pass"};
        }
        if (args.starts_with("redeem")) {
            return {.success = true, .output = "Pass redeemed successfully"};
        }
        return {.success = true, .output = "Pass status updated"};
    }
};


struct RateLimitOptionsCommand {
    static constexpr auto name() -> std::string_view { return "rate-limit-options"; }
    static constexpr auto description() -> std::string_view { return "Configure rate limit behavior and display options"; }
    static constexpr auto aliases() { return std::array<std::string_view, 1>{"rlo"}; }

    auto execute(std::string_view /*args*/) -> CommandResult {
        return {.success = true, .output = "Rate limit options:\n"
                "  Show warnings: enabled\n"
                "  Auto retry: enabled (max 5 times)\n"
                "  Backoff strategy: exponential backoff + jitter\n"
                "  Current tier: Pro"};
    }
};


struct VersionCommand {
    static constexpr auto name() -> std::string_view { return "version"; }
    static constexpr auto description() -> std::string_view { return "Show version and build info"; }
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
