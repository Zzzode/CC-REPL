/// @file runtime_surface_commands.cppm
/// @brief Typed slash-command adapters for migrated command helper modules.
module;

#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

export module cc.commands.runtime_surface_commands;

import cc.types.types;
import cc.commands.command;
import cc.commands.ant_trace;
import cc.commands.autofix_pr;
import cc.commands.backfill_sessions;
import cc.commands.break_cache;
import cc.commands.bridge;
import cc.commands.bughunter;
import cc.commands.commit_push_pr;
import cc.commands.create_moved_to_plugin_command;
import cc.commands.debug_tool_call;
import cc.commands.extra_usage;
import cc.commands.exit;
import cc.commands.init_verifiers;
import cc.commands.install_github_app.flow;
import cc.commands.install_github_app.github_actions;
import cc.commands.install_github_app.steps;
import cc.commands.keybindings_cmd;
import cc.commands.mock_limits;
import cc.commands.oauth_refresh;
import cc.commands.onboarding;
import cc.commands.output_style;
import cc.commands.perf_issue;
import cc.commands.pr_comments;
import cc.commands.privacy_settings;
import cc.commands.rate_limit_options;
import cc.commands.release_notes;
import cc.commands.reload_plugins;
import cc.commands.remote_env;
import cc.commands.remote_setup;
import cc.commands.reset_limits;
import cc.commands.sandbox_toggle;
import cc.commands.security_review;
import cc.commands.statusline;
import cc.commands.terminal_setup;
import cc.commands.thinkback;
import cc.commands.thinkback_play;
import cc.commands.version;

export namespace cc::commands {

using namespace cc::core;

namespace detail {

[[nodiscard]] std::string join_args(const std::vector<std::string>& args) {
    std::string joined;
    for (const auto& arg : args) {
        if (!joined.empty()) joined += ' ';
        joined += arg;
    }
    return joined;
}

[[nodiscard]] std::uint16_t parse_port(std::string_view text, std::uint16_t fallback = 22) {
    std::uint16_t value = 0;
    auto begin = text.data();
    auto end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value == 0) return fallback;
    return value;
}

template <typename Response>
[[nodiscard]] CommandResult from_response(Response response) {
    return response.ok ? CommandResult::success(std::move(response.message))
                       : CommandResult::fail(std::move(response.message));
}

class BasicCommand {
public:
    [[nodiscard]] VoidResult validate(const CommandContext&) { return {}; }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace detail

#define CC_RUNTIME_HELPER_COMMAND(TYPE_NAME, COMMAND_NAME, DESCRIPTION, CATEGORY, QUALIFIED_RUN) \
class TYPE_NAME final : public detail::BasicCommand { \
public: \
    [[nodiscard]] static CommandDefinition definition() { \
        return CommandDefinition{ \
            .name = COMMAND_NAME, \
            .description = DESCRIPTION, \
            .args = {CommandArg{.name = "args", .description = "Command arguments", .type = ArgType::Text, .required = false}}, \
            .category = CATEGORY, \
        }; \
    } \
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) { \
        return detail::from_response(QUALIFIED_RUN(detail::join_args(ctx.args))); \
    } \
};

CC_RUNTIME_HELPER_COMMAND(AntTraceCommand, "ant-trace", "Inspect internal trace diagnostics", "diagnostics", ant_trace::run)
CC_RUNTIME_HELPER_COMMAND(AutofixPrCommand, "autofix-pr", "Generate fixes for pull request feedback", "git", autofix_pr::run)
CC_RUNTIME_HELPER_COMMAND(BackfillSessionsCommand, "backfill-sessions", "Backfill local session metadata", "session", backfill_sessions::run)
CC_RUNTIME_HELPER_COMMAND(BreakCacheCommand, "break-cache", "Clear internal caches", "diagnostics", break_cache::run)
CC_RUNTIME_HELPER_COMMAND(BridgeCommand, "bridge", "Manage IDE bridge state", "bridge", bridge::run)
CC_RUNTIME_HELPER_COMMAND(BughunterCommand, "bughunter", "Run bug hunting diagnostics", "diagnostics", bughunter::run)
CC_RUNTIME_HELPER_COMMAND(CommitPushPrCommand, "commit-push-pr", "Commit, push, and prepare a pull request", "git", commit_push_pr::run)
CC_RUNTIME_HELPER_COMMAND(CreateMovedToPluginCommand, "create-moved-to-plugin-command", "Create a moved-to-plugin command shim", "plugins", create_moved_to_plugin_command::run)
CC_RUNTIME_HELPER_COMMAND(DebugToolCallCommand, "debug-tool-call", "Debug a tool call payload", "diagnostics", debug_tool_call::run)
CC_RUNTIME_HELPER_COMMAND(ExtraUsageCommand, "extra-usage", "Show extended usage information", "usage", extra_usage::run)
CC_RUNTIME_HELPER_COMMAND(InitVerifiersCommand, "init-verifiers", "Initialize verifier configuration", "verification", init_verifiers::run)
CC_RUNTIME_HELPER_COMMAND(MockLimitsCommand, "mock-limits", "Configure mock rate limits", "usage", mock_limits::run)
CC_RUNTIME_HELPER_COMMAND(OnboardingCommand, "onboarding", "Run onboarding checks", "setup", onboarding::run)
CC_RUNTIME_HELPER_COMMAND(OutputStyleCommand, "output-style", "Manage output style", "config", output_style::run)
CC_RUNTIME_HELPER_COMMAND(PerfIssueCommand, "perf-issue", "Collect performance issue diagnostics", "diagnostics", perf_issue::run)
CC_RUNTIME_HELPER_COMMAND(PrCommentsCommand, "pr-comments", "Inspect pull request comments", "git", pr_comments::run)
CC_RUNTIME_HELPER_COMMAND(ReloadPluginsCommand, "reload-plugins", "Reload installed plugins", "plugins", reload_plugins::run)
CC_RUNTIME_HELPER_COMMAND(ResetLimitsCommand, "reset-limits", "Reset local mock limits", "usage", reset_limits::run)
CC_RUNTIME_HELPER_COMMAND(SecurityReviewCommand, "security-review", "Run a security-focused review", "review", security_review::run)
CC_RUNTIME_HELPER_COMMAND(StatuslineCommand, "statusline", "Configure statusline output", "terminal", statusline::run)
CC_RUNTIME_HELPER_COMMAND(TerminalSetupCommand, "terminal-setup", "Configure terminal integration", "terminal", terminal_setup::run)
CC_RUNTIME_HELPER_COMMAND(ThinkbackPlayCommand, "thinkback-play", "Replay thinking history", "thinking", thinkback_play::run)
CC_RUNTIME_HELPER_COMMAND(VersionCommand, "version", "Show the CLI version", "system", version::run)

#undef CC_RUNTIME_HELPER_COMMAND

class ExitCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "exit",
            .description = "Exit the application",
            .args = {CommandArg{.name = "--force", .description = "Exit without confirmation", .type = ArgType::None, .required = false}},
            .category = "system",
            .aliases = {"quit", "q"},
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        const bool force = !ctx.args.empty() && (ctx.args.front() == "--force" || ctx.args.front() == "-f");
        execute_exit(force);
        return CommandResult::exit();
    }
};

class InstallGithubAppCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install-github-app",
            .description = "Install the GitHub app integration",
            .args = {
                CommandArg{.name = "repo", .description = "Target repository", .type = ArgType::Text, .required = false},
                CommandArg{.name = "--workflow", .description = "Generate workflow YAML instead of running checks", .type = ArgType::None, .required = false},
            },
            .category = "git",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (!ctx.args.empty() && ctx.args.front() == "--workflow") {
            return CommandResult::success(generate_workflow_yaml("claude-sonnet-4-20250514", true));
        }

        std::string message = "GitHub app installation flow\n";
        InstallState state{.current_step = InstallStep::CheckGitHub, .data = {}, .errors = {}};
        message += "- " + get_step_description(state.current_step) + "\n";
        while (auto next = advance_step(state)) {
            message += "- " + get_step_description(*next) + "\n";
            if (*next == InstallStep::Success) break;
        }

        if (auto gh = check_github_cli(); !gh) {
            message += "\nGitHub CLI check failed: " + gh.error() + "\n";
            return CommandResult::fail(message);
        }

        if (!ctx.args.empty()) {
            auto repo = ctx.args.front();
            if (auto install = install_github_app_to_repo(repo); !install) {
                message += "\nRepository installation failed: " + install.error() + "\n";
                return CommandResult::fail(message);
            }
            message += "\nRepository prepared: " + repo + "\n";
        }

        return CommandResult::success(message);
    }
};

class KeybindingsCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "keybindings",
            .description = "Manage keybindings",
            .args = {
                CommandArg{.name = "action", .description = "show, set, reset, or export", .type = ArgType::Choice, .required = false, .choices = {"show", "set", "reset", "export"}},
                CommandArg{.name = "name", .description = "Action name for set", .type = ArgType::Text, .required = false},
                CommandArg{.name = "keys", .description = "Key binding for set", .type = ArgType::Text, .required = false},
            },
            .category = "input",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty() || ctx.args.front() == "show") return CommandResult::success(show_keybindings());
        if (ctx.args.front() == "export") return CommandResult::success(export_keybindings());
        if (ctx.args.front() == "reset") {
            reset_keybindings();
            return CommandResult::success("Keybindings reset to defaults");
        }
        if (ctx.args.front() == "set") {
            if (ctx.args.size() < 3) return CommandResult::fail("keybindings set requires action and keys");
            auto result = set_keybinding(ctx.args[1], ctx.args[2]);
            if (!result) return CommandResult::fail(result.error());
            return CommandResult::success("Keybinding updated");
        }
        return CommandResult::fail("Unknown keybindings action: " + ctx.args.front());
    }
};

class OauthRefreshCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "oauth-refresh",
            .description = "Refresh OAuth credentials",
            .args = {CommandArg{.name = "account", .description = "Optional account suffix", .type = ArgType::Text, .required = false}},
            .category = "auth",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        return detail::from_response(oauth_refresh::run(detail::join_args(ctx.args)));
    }
};

class RateLimitOptionsCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "rate-limit-options",
            .description = "Show rate limit options",
            .args = {CommandArg{.name = "mode", .description = "status or optimize", .type = ArgType::Choice, .required = false, .choices = {"status", "optimize"}}},
            .category = "usage",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (!ctx.args.empty() && (ctx.args.front() == "optimize" || ctx.args.front() == "suggest")) {
            return CommandResult::success(suggest_rate_limit_optimization());
        }
        return CommandResult::success(show_rate_limit_status() + "\n" + suggest_rate_limit_optimization());
    }
};

class PrivacySettingsCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "privacy-settings",
            .description = "Manage privacy settings",
            .args = {CommandArg{.name = "action", .description = "show or set", .type = ArgType::Text, .required = false}},
            .category = "settings",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty() || ctx.args.front() == "show") return CommandResult::success(show_privacy_info());
        if (ctx.args.front() != "set") return CommandResult::fail("Unknown privacy-settings action: " + ctx.args.front());
        auto settings = get_privacy_settings();
        for (std::size_t i = 1; i + 1 < ctx.args.size(); i += 2) {
            const auto& key = ctx.args[i];
            const auto& value = ctx.args[i + 1];
            if (key == "telemetry") settings.telemetry = value == "true" || value == "1";
            else if (key == "crash_reports") settings.crash_reports = value == "true" || value == "1";
            else if (key == "usage_stats") settings.usage_stats = value == "true" || value == "1";
            else if (key == "data_retention") settings.data_retention = value;
            else return CommandResult::fail("Unknown privacy setting: " + key);
        }
        set_privacy_settings(settings);
        return CommandResult::success(show_privacy_info());
    }
};

class ReleaseNotesCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "release-notes",
            .description = "Show release notes",
            .args = {CommandArg{.name = "version", .description = "Optional version or --since version", .type = ArgType::Text, .required = false}},
            .category = "system",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.size() >= 2 && ctx.args.front() == "--since") {
            return CommandResult::success(get_changelog_since(ctx.args[1]));
        }
        auto notes = get_release_notes(ctx.args.empty() ? std::nullopt : std::optional<std::string>{ctx.args.front()});
        if (!notes) return CommandResult::fail(notes.error());
        return CommandResult::success(*notes);
    }
};

class RemoteEnvCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "remote-env",
            .description = "Show remote environment state",
            .category = "remote",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(get_remote_env_summary());
    }
};

class RemoteSetupCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "remote-setup",
            .description = "Configure remote execution",
            .args = {
                CommandArg{.name = "host", .description = "Remote host, or status/test", .type = ArgType::Text, .required = false},
                CommandArg{.name = "port", .description = "Remote port", .type = ArgType::Number, .required = false},
                CommandArg{.name = "auth", .description = "ssh_key, token, or oauth", .type = ArgType::Choice, .required = false, .choices = {"ssh_key", "token", "oauth"}},
            },
            .category = "remote",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty() || ctx.args.front() == "status") {
            return CommandResult::success(get_remote_status());
        }

        const bool test_only = ctx.args.front() == "test";
        const auto offset = test_only ? std::size_t{1} : std::size_t{0};
        if (ctx.args.size() <= offset) {
            return CommandResult::fail("remote-setup requires a host");
        }

        RemoteSetupConfig config{
            .host = ctx.args[offset],
            .port = ctx.args.size() > offset + 1 ? detail::parse_port(ctx.args[offset + 1]) : std::uint16_t{22},
            .auth_method = ctx.args.size() > offset + 2 ? ctx.args[offset + 2] : "ssh_key",
        };

        auto result = test_only ? test_remote_connection(config) : setup_remote(config);
        if (!result) return CommandResult::fail(result.error());
        return CommandResult::success(test_only ? "Remote connection test succeeded"
                                                : "Remote execution configured");
    }
};

class SandboxToggleCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "sandbox-toggle",
            .description = "Toggle sandbox mode",
            .args = {CommandArg{.name = "action", .description = "status, info, or toggle", .type = ArgType::Choice, .required = false, .choices = {"status", "info", "toggle"}}},
            .category = "security",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty() || ctx.args.front() == "toggle") {
            auto enabled = toggle_sandbox();
            return CommandResult::success(std::format("Sandbox {}", enabled ? "enabled" : "disabled"));
        }
        if (ctx.args.front() == "status") return CommandResult::success(get_sandbox_status());
        if (ctx.args.front() == "info") return CommandResult::success(show_sandbox_info());
        return CommandResult::fail("Unknown sandbox action: " + ctx.args.front());
    }
};

class ThinkbackCommand final : public detail::BasicCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "thinkback",
            .description = "Inspect thinking history",
            .args = {CommandArg{.name = "action", .description = "export, clear, or replay", .type = ArgType::Text, .required = false}},
            .category = "thinking",
        };
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty() || ctx.args.front() == "export") return CommandResult::success(export_thinking_log());
        if (ctx.args.front() == "clear") {
            clear_thinking_history();
            return CommandResult::success("Thinking history cleared");
        }
        if (ctx.args.front() == "replay") {
            std::size_t index = 0;
            if (ctx.args.size() > 1) {
                auto parsed = detail::parse_port(ctx.args[1], 1);
                index = parsed == 0 ? 0 : static_cast<std::size_t>(parsed - 1);
            }
            return CommandResult::success(replay_thinking(index));
        }
        return CommandResult::fail("Unknown thinkback action: " + ctx.args.front());
    }
};

} // namespace cc::commands
