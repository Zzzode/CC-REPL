/// @file command_registry.cppm
/// @brief Central command registry importing all commands.
/// Provides command lookup by name/alias, autocompletion,
/// permission checking, and command history tracking.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <chrono>

export module cc.commands.registry;

import cc.types.types;
import cc.commands.command;
import cc.commands.commit;
import cc.commands.review;
import cc.commands.config;
import cc.commands.context;
import cc.commands.diff;
import cc.commands.mcp_cmd;
import cc.commands.compact;
import cc.commands.help;
import cc.commands.doctor;
import cc.commands.clear;
import cc.commands.add_dir;
import cc.commands.agents;
import cc.commands.btw;
import cc.commands.advisor;
import cc.commands.bridge_kick;
import cc.commands.brief;
import cc.commands.color;
import cc.commands.ctx_viz;
import cc.commands.effort;
import cc.commands.env;
import cc.commands.fast;
import cc.commands.feedback;
import cc.commands.files;
import cc.commands.heapdump;
import cc.commands.hooks;
import cc.commands.ide;
import cc.commands.issue;
import cc.commands.memory;
import cc.commands.passes;
import cc.commands.rename;
import cc.commands.rewind;
import cc.commands.share;
import cc.commands.stats;
import cc.commands.status;
import cc.commands.summary;
import cc.commands.tag;
import cc.commands.teleport;
import cc.commands.upgrade;
import cc.commands.ultraplan;
import cc.commands.install;
import cc.commands.insights;
import cc.commands.init;
import cc.commands.session;
import cc.commands.resume;
import cc.commands.model;
import cc.commands.cost;
import cc.commands.plan;
import cc.commands.theme;
import cc.commands.vim;
import cc.commands.login;
import cc.commands.logout;
import cc.commands.permissions_cmd;
import cc.commands.plugin_cmd;
import cc.commands.usage;
import cc.commands.branch;
import cc.commands.chrome;
import cc.commands.copy_cmd;
import cc.commands.desktop;
import cc.commands.export_cmd;
import cc.commands.good_claude;
import cc.commands.install_slack_app;
import cc.commands.mobile;
import cc.commands.stickers;
import cc.commands.tasks_cmd;
import cc.commands.skills_cmd;
import cc.commands.voice;

export namespace cc::commands {

using namespace cc::core;

/// Record of a previously executed command
struct CommandHistoryEntry {
    std::string input;                                // Raw input string
    CommandStatus status;                             // Execution result status
    std::chrono::system_clock::time_point timestamp;  // When it was executed
};

/// Permission level required to execute commands
enum class CommandPermission : std::uint8_t {
    None,       // No special permissions needed
    ReadOnly,   // Only read operations (help, config get, doctor)
    ReadWrite,  // May modify files or state (commit, config set)
    Admin,      // Administrative operations (clear --all, mcp remove)
};

/// Maps commands to their required permission levels
[[nodiscard]] constexpr CommandPermission command_permission(std::string_view name) noexcept {
    if (name == "help" || name == "doctor" || name == "review" || name == "agents" || name == "btw" ||
        name == "advisor" || name == "brief" || name == "color" || name == "ctx-viz" ||
        name == "effort" || name == "env" || name == "fast" || name == "feedback" || name == "files" ||
        name == "heapdump" || name == "hooks" || name == "ide" || name == "issue" || name == "memory" ||
        name == "passes" || name == "stats" || name == "status" || name == "summary" ||
        name == "tag" || name == "teleport" || name == "ultraplan" || name == "insights" || name == "init")
        return CommandPermission::ReadOnly;
    if (name == "chrome" || name == "desktop" || name == "good-claude" || name == "mobile" ||
        name == "stickers")
        return CommandPermission::ReadOnly;
    
    if (name == "clear" || name == "compact" || name == "add-dir" || name == "commit" || name == "config" ||
        name == "mcp" || name == "bridge-kick" || name == "rename" || name == "rewind" || name == "share" ||
        name == "upgrade" || name == "install" || name == "install-slack-app")
        return CommandPermission::ReadWrite;
    
    return CommandPermission::None;
}

/// Central registry that owns all command instances and provides dispatch,
/// lookup, autocompletion, permission checking, and history tracking.
class AppCommandRegistry {
    CommandRegistry registry_;                         // Core registry from cc.core.command
    std::deque<CommandHistoryEntry> history_;          // Command execution history
    std::uint32_t max_history_size_ = 100;            // Maximum history entries to retain
    CommandPermission current_permission_ = CommandPermission::Admin;  // Current session permission

public:
    /// Construct and register all built-in commands
    AppCommandRegistry() {
        register_all_commands();
    }

    /// Execute a raw input string (e.g., "/commit --all")
    [[nodiscard]] Result<CommandResult> execute(std::string_view input,
                                                CommandContext ctx) {
        // Parse the input
        auto parsed = CommandRegistry::parse(input);
        if (!parsed) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Invalid command input: '{}'", input)
            ));
        }

        // Check permissions
        auto required = command_permission(parsed->name);
        if (static_cast<int>(required) > static_cast<int>(current_permission_)) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied,
                std::format("Insufficient permissions for /{}. Required: {}",
                    parsed->name, permission_label(required))
            ));
        }

        if (!registry_.contains(parsed->name)) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Unknown command: /{}", parsed->name)
            ));
        }

        // Prepare context with parsed args
        ctx.args = parsed->args;
        ctx.raw_input = parsed->raw;

        if (parsed->name == "help" && ctx.args.empty()) {
            auto help = CommandResult::success(registry_.generate_help());
            record_history(parsed->raw, help.status);
            return help;
        }

        auto result = registry_.execute(parsed->raw);
        if (!result) {
            auto fail = CommandResult::fail(std::format("Unknown command: /{}", parsed->name));
            record_history(parsed->raw, fail.status);
            return fail;
        }

        record_history(parsed->raw, result->status);
        return *result;
    }

    /// Get autocompletion suggestions for partial input
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial_input) const {
        // First, try command name completion
        auto command_suggestions = registry_.complete(partial_input);
        if (!command_suggestions.empty()) return command_suggestions;

        // If we already have a full command name, delegate to that command's completer
        auto parsed = CommandRegistry::parse(partial_input);
        if (parsed) {
            auto* cmd = find_command(parsed->name);
            if (cmd) {
                // Get the last argument as partial
                std::string_view last_arg;
                if (!parsed->args.empty()) {
                    last_arg = parsed->args.back();
                }
                return cmd->complete(last_arg);
            }
        }

        return {};
    }

    /// Get command execution history
    [[nodiscard]] const std::deque<CommandHistoryEntry>& history() const noexcept {
        return history_;
    }

    /// Get recent history entries (most recent first)
    [[nodiscard]] std::vector<CommandHistoryEntry> recent_history(std::size_t count) const {
        std::vector<CommandHistoryEntry> recent;
        auto n = std::min(count, history_.size());
        for (std::size_t i = 0; i < n; ++i) {
            recent.push_back(history_[history_.size() - 1 - i]);
        }
        return recent;
    }

    /// Search history by prefix
    [[nodiscard]] std::vector<std::string> search_history(std::string_view prefix) const {
        std::vector<std::string> matches;
        for (const auto& entry : history_ | std::views::reverse) {
            if (entry.input.starts_with(prefix)) {
                matches.push_back(entry.input);
            }
            if (matches.size() >= 10) break;
        }
        return matches;
    }

    /// Set the current permission level for the session
    void set_permission_level(CommandPermission level) noexcept {
        current_permission_ = level;
    }

    /// Check if a command exists
    [[nodiscard]] bool has_command(std::string_view name) const {
        return registry_.contains(name);
    }

    /// Get all visible command definitions (for /help)
    [[nodiscard]] std::vector<const CommandDefinition*> visible_commands() const {
        return registry_.visible_commands();
    }

    /// Get total registered command count
    [[nodiscard]] std::size_t command_count() const noexcept {
        return registry_.size();
    }

    [[nodiscard]] std::vector<std::string> command_names() const {
        return registry_.command_names();
    }

    /// Clear command history
    void clear_history() noexcept {
        history_.clear();
    }

private:
    /// Register all built-in commands
    void register_all_commands() {
        registry_.register_command<CommitCommand>();
        registry_.register_command<ReviewCommand>();
        registry_.register_command<ConfigCommand>();
        registry_.register_command<McpCommand>();
        registry_.register_command<CompactCommand>();
        registry_.register_command<HelpCommand>();
        registry_.register_command<DoctorCommand>();
        registry_.register_command<ClearCommand>();
        registry_.register_command<AddDirCommand>();
        registry_.register_command<AgentsCommand>();
        registry_.register_command<BtwCommand>();
        registry_.register_command<AdvisorCommand>();
        registry_.register_command<BridgeKickCommand>();
        registry_.register_command<BriefCommand>();
        registry_.register_command<ColorCommand>();
        registry_.register_command<CtxVizCommand>();
        registry_.register_command<EffortCommand>();
        registry_.register_command<EnvCommand>();
        registry_.register_command<FastCommand>();
        registry_.register_command<FeedbackCommand>();
        registry_.register_command<FilesCommand>();
        registry_.register_command<HeapdumpCommand>();
        registry_.register_command<HooksCommand>();
        registry_.register_command<IdeCommand>();
        registry_.register_command<IssueCommand>();
        registry_.register_command<MemoryCommand>();
        registry_.register_command<PassesCommand>();
        registry_.register_command<RenameCommand>();
        registry_.register_command<RewindCommand>();
        registry_.register_command<ShareCommand>();
        registry_.register_command<StatsCommand>();
        registry_.register_command<StatusCommand>();
        registry_.register_command<SummaryCommand>();
        registry_.register_command<TagCommand>();
        registry_.register_command<TeleportCommand>();
        registry_.register_command<UpgradeCommand>();
        registry_.register_command<UltraplanCommand>();
        registry_.register_command<InstallCommand>();
        registry_.register_command<InsightsCommand>();
        registry_.register_command<InitCommand>();
        registry_.register_command<ContextCommand>();
        registry_.register_command<DiffCommand>();
        registry_.register_command<SessionCommand>();
        registry_.register_command<ResumeCommand>();
        registry_.register_command<ModelCommand>();
        registry_.register_command<CostCommand>();
        registry_.register_command<PlanCommand>();
        registry_.register_command<ThemeCommand>();
        registry_.register_command<VimCommand>();
        registry_.register_command<LoginCommand>();
        registry_.register_command<LogoutCommand>();
        registry_.register_command<PermissionsCommand>();
        registry_.register_command<PluginCommand>();
        registry_.register_command<UsageCommand>();
        registry_.register_command<BranchCommand>();
        registry_.register_command<ChromeCommand>();
        registry_.register_command<CopyCommand>();
        registry_.register_command<DesktopCommand>();
        registry_.register_command<ExportCommand>();
        registry_.register_command<GoodClaudeCommand>();
        registry_.register_command<InstallSlackAppCommand>();
        registry_.register_command<MobileCommand>();
        registry_.register_command<StickersCommand>();
        registry_.register_command<TasksCommand>();
        registry_.register_command<SkillsCommand>();
        registry_.register_command<VoiceCommand>();
        register_migration_surface_commands();
    }

    void register_surface_command(std::string name, std::string description) {
        auto command_name = name;
        registry_.register_command(CommandRegistration{
            .name = std::move(name),
            .description = std::move(description),
            .usage = "/" + command_name,
            .handler = [command_name](const CommandContext&) {
                if (command_name == "exit") return CommandResult::exit();
                return CommandResult::success(std::format(
                    "/{} is registered in the C++ runtime. No dedicated local action is available for this command.",
                    command_name
                ));
            },
            .aliases = command_name == "exit" ? std::vector<std::string>{"quit", "q"} : std::vector<std::string>{},
            .hidden = false,
        });
    }

    void register_migration_surface_commands() {
        const std::vector<std::pair<std::string, std::string>> commands = {
            {"add-dir", "Add a working directory to the context"},
            {"advisor", "Configure the advisor model"},
            {"agents", "Manage agent configurations"},
            {"ant-trace", "Inspect internal trace diagnostics"},
            {"autofix-pr", "Generate fixes for pull request feedback"},
            {"backfill-sessions", "Backfill local session metadata"},
            {"branch", "Manage git branches"},
            {"break-cache", "Clear internal caches"},
            {"bridge", "Manage IDE bridge state"},
            {"bridge-kick", "Kick or restart bridge connections"},
            {"brief", "Toggle brief-only mode"},
            {"btw", "Ask a quick side question without interrupting the main conversation"},
            {"bughunter", "Run bug hunting diagnostics"},
            {"chrome", "Show Claude in Chrome setup information"},
            {"clear", "Clear the screen and optionally reset conversation state"},
            {"color", "Set the session color"},
            {"commit", "Generate a conventional commit message and commit staged changes"},
            {"commit-push-pr", "Commit, push, and prepare a pull request"},
            {"compact", "Compress conversation context to free up token budget"},
            {"config", "View and modify CLI configuration settings"},
            {"context", "Show and manage context window"},
            {"copy", "Copy response content to clipboard"},
            {"cost", "Show cost breakdown for the current session"},
            {"create-moved-to-plugin-command", "Create a moved-to-plugin command shim"},
            {"ctx-viz", "Visualize current context usage"},
            {"debug-tool-call", "Debug a tool call payload"},
            {"desktop", "Continue the current session in Claude Desktop"},
            {"diff", "Show file changes made in the current session"},
            {"doctor", "Run system diagnostics and check environment health"},
            {"effort", "Set the effort level"},
            {"env", "Show environment diagnostics"},
            {"exit", "Exit the application"},
            {"export", "Export conversation to a file"},
            {"extra-usage", "Show extended usage information"},
            {"fast", "Toggle fast mode"},
            {"feedback", "Submit feedback about Claude Code"},
            {"files", "Show all files currently in context"},
            {"good-claude", "Send positive product feedback"},
            {"heapdump", "Write process heap diagnostics"},
            {"help", "Show available commands and usage information"},
            {"hooks", "View hook configurations for tool events"},
            {"ide", "Manage IDE integration settings"},
            {"init", "Initialize Claude Code configuration"},
            {"init-verifiers", "Initialize verifier configuration"},
            {"insights", "Analyze usage data and generate insights"},
            {"install", "Install Claude Code system-wide"},
            {"install-github-app", "Install the GitHub app integration"},
            {"install-slack-app", "Install the Claude Slack app"},
            {"issue", "Create or inspect issues"},
            {"keybindings", "Manage keybindings"},
            {"login", "Authenticate with Anthropic API"},
            {"logout", "Clear stored authentication credentials"},
            {"mcp", "Manage Model Context Protocol server connections"},
            {"memory", "Edit persistent memory files"},
            {"mobile", "Show Claude mobile app download links"},
            {"mock-limits", "Configure mock rate limits"},
            {"model", "Switch or display the active model"},
            {"oauth-refresh", "Refresh OAuth credentials"},
            {"onboarding", "Run onboarding checks"},
            {"output-style", "Manage output style"},
            {"passes", "Show pass and mode diagnostics"},
            {"perf-issue", "Collect performance issue diagnostics"},
            {"permissions", "Manage tool execution permissions"},
            {"plan", "Enter plan mode or show current plan"},
            {"plugin", "Manage plugins"},
            {"pr-comments", "Inspect pull request comments"},
            {"privacy-settings", "Manage privacy settings"},
            {"rate-limit-options", "Show rate limit options"},
            {"release-notes", "Show release notes"},
            {"reload-plugins", "Reload installed plugins"},
            {"remote-env", "Show remote environment state"},
            {"remote-setup", "Configure remote execution"},
            {"rename", "Rename the current session"},
            {"reset-limits", "Reset local mock limits"},
            {"resume", "Resume a previous conversation session"},
            {"review", "Review code changes with AI-powered analysis"},
            {"rewind", "Restore the code and/or conversation to a previous point"},
            {"sandbox-toggle", "Toggle sandbox mode"},
            {"security-review", "Run a security-focused review"},
            {"session", "Manage conversation sessions"},
            {"share", "Share current session"},
            {"skills", "Manage installed skills"},
            {"stats", "Show session statistics"},
            {"status", "Show Claude Code status"},
            {"statusline", "Configure statusline output"},
            {"stickers", "Order Claude Code stickers"},
            {"summary", "Summarize the current session"},
            {"tag", "Toggle a searchable tag on the current session"},
            {"tasks", "Manage running background tasks"},
            {"teleport", "Move a session to another environment"},
            {"terminal-setup", "Configure terminal integration"},
            {"theme", "Switch visual theme or color scheme"},
            {"thinkback", "Inspect thinking history"},
            {"thinkback-play", "Replay thinking history"},
            {"ultraplan", "Enter ultraplan mode"},
            {"upgrade", "Upgrade Claude Code to latest version"},
            {"usage", "Show token usage, cost, and rate limit status"},
            {"version", "Show the CLI version"},
            {"vim", "Toggle vim-style keybindings"},
            {"voice", "Toggle voice input mode"},
        };

        for (const auto& [name, description] : commands) {
            register_surface_command(name, description);
        }
    }

    /// Find a command by name or alias
    [[nodiscard]] ICommand* find_command(std::string_view name) const {
        return registry_.get(name);
    }

    /// Record a command execution in history
    void record_history(std::string_view input, CommandStatus status) {
        history_.push_back(CommandHistoryEntry{
            .input = std::string(input),
            .status = status,
            .timestamp = std::chrono::system_clock::now(),
        });

        // Trim history if exceeding max size
        while (history_.size() > max_history_size_) {
            history_.pop_front();
        }
    }

    /// Convert permission level to display label
    [[nodiscard]] static constexpr std::string_view permission_label(CommandPermission perm) noexcept {
        switch (perm) {
            case CommandPermission::None:      return "none";
            case CommandPermission::ReadOnly:  return "read-only";
            case CommandPermission::ReadWrite: return "read-write";
            case CommandPermission::Admin:     return "admin";
        }
        return "unknown";
    }
};

} // namespace cc::commands
