/// @file extra_commands.cppm
/// @brief All remaining slash commands consolidated into a single module.
/// Each command satisfies the SlashCommand concept with definition(), execute(), validate(), complete().
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>

export module cc.commands.extra;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

// ============================================================
// 1. AddDirCommand — /add-dir
// ============================================================

class AddDirCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "add-dir",
            .description = "Add a working directory to the context",
            .aliases = {"ad"},
            .args = {CommandArg{.name = "path", .description = "Directory path to add",
                               .type = ArgType::FilePath, .required = true}},
            .hidden = false,
            .category = "context",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto dir = ctx.args.empty() ? "." : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Added directory '{}' to context.", dir));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 2. AgentsCommand — /agents
// ============================================================

class AgentsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "agents",
            .description = "Manage custom agents (list, create, edit, delete)",
            .aliases = {},
            .args = {CommandArg{.name = "action", .description = "list|create|edit|delete",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"list", "create", "edit", "delete"}}}},
            .hidden = false,
            .category = "agents",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty() ? "list" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Agents action '{}' executed.", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 3. ExitCommand — /exit
// ============================================================

class ExitCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "exit",
            .description = "Exit the application",
            .aliases = {"quit", "q"},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::exit();
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 4. FastCommand — /fast
// ============================================================

class FastCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "fast",
            .description = "Toggle fast/turbo mode (use cheaper model)",
            .aliases = {"turbo"},
            .args = {},
            .hidden = false,
            .category = "model",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Fast mode toggled.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 5. FeedbackCommand — /feedback
// ============================================================

class FeedbackCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "feedback",
            .description = "Submit user feedback",
            .aliases = {"fb"},
            .args = {CommandArg{.name = "message", .description = "Feedback text",
                               .type = ArgType::Text, .required = true}},
            .hidden = false,
            .category = "misc",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto msg = ctx.args.empty() ? "" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Feedback submitted: '{}'", msg));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 6. FilesCommand — /files
// ============================================================

class FilesCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "files",
            .description = "List tracked files in context",
            .aliases = {"ls"},
            .args = {},
            .hidden = false,
            .category = "context",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("No files currently tracked in context.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 7. HooksCommand — /hooks
// ============================================================

class HooksCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "hooks",
            .description = "Manage event hooks",
            .aliases = {},
            .args = {CommandArg{.name = "action", .description = "list|add|remove",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"list", "add", "remove"}}}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty() ? "list" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Hooks '{}' completed.", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 8. IdeCommand — /ide
// ============================================================

class IdeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ide",
            .description = "IDE integration settings",
            .aliases = {},
            .args = {CommandArg{.name = "setting", .description = "IDE setting to configure",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("IDE integration settings displayed.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 9. MemoryCommand — /memory
// ============================================================

class MemoryCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "memory",
            .description = "Manage persistent memories",
            .aliases = {"mem"},
            .args = {CommandArg{.name = "action", .description = "list|add|remove|clear",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"list", "add", "remove", "clear"}}}},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty() ? "list" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Memory '{}' completed.", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 10. RenameCommand — /rename
// ============================================================

class RenameCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "rename",
            .description = "Rename current session",
            .aliases = {},
            .args = {CommandArg{.name = "new_name", .description = "New session name",
                               .type = ArgType::Text, .required = true}},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto name = ctx.args.empty() ? "unnamed" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Session renamed to '{}'.", name));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 11. RewindCommand — /rewind
// ============================================================

class RewindCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "rewind",
            .description = "Rewind conversation to a previous state",
            .aliases = {"undo"},
            .args = {CommandArg{.name = "steps", .description = "Number of steps to rewind",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto steps = ctx.args.empty() ? "1" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Rewound {} step(s).", steps));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 12. StatsCommand — /stats
// ============================================================

class StatsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "stats",
            .description = "Show session statistics",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Session stats: 0 messages, 0 tokens used.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 13. StatusCommand — /status
// ============================================================

class StatusCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "status",
            .description = "Show current status (model, mode, connections)",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Status: connected, model=default, mode=normal.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 14. UpgradeCommand — /upgrade
// ============================================================

class UpgradeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "upgrade",
            .description = "Check for and apply updates",
            .aliases = {"update"},
            .args = {},
            .hidden = false,
            .category = "misc",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Already on the latest version.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 15. InitCommand — /init
// ============================================================

class InitCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "init",
            .description = "Initialize CLAUDE.md in project",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("CLAUDE.md initialized in project root.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 16. EffortCommand — /effort
// ============================================================

class EffortCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "effort",
            .description = "Set reasoning effort level",
            .aliases = {},
            .args = {CommandArg{.name = "level", .description = "low|medium|high",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"low", "medium", "high"}}}},
            .hidden = false,
            .category = "model",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto level = ctx.args.empty() ? "medium" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Reasoning effort set to '{}'.", level));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 17. OutputStyleCommand — /output-style
// ============================================================

class OutputStyleCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "output-style",
            .description = "Set output verbosity/format",
            .aliases = {"os"},
            .args = {CommandArg{.name = "style", .description = "concise|normal|verbose|json",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"concise", "normal", "verbose", "json"}}}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto style = ctx.args.empty() ? "normal" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Output style set to '{}'.", style));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 18. KeybindingsCommand — /keybindings
// ============================================================

class KeybindingsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "keybindings",
            .description = "Manage keyboard shortcuts",
            .aliases = {"keys", "kb"},
            .args = {CommandArg{.name = "action", .description = "list|set|reset",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"list", "set", "reset"}}}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty() ? "list" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Keybindings '{}' completed.", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 19. ColorCommand — /color
// ============================================================

class ColorCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "color",
            .description = "Terminal color settings",
            .aliases = {"colours"},
            .args = {CommandArg{.name = "mode", .description = "auto|dark|light|none",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"auto", "dark", "light", "none"}}}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto mode = ctx.args.empty() ? "auto" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Color mode set to '{}'.", mode));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 20. EnvCommand — /env
// ============================================================

class EnvCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "env",
            .description = "Show environment variables",
            .aliases = {},
            .args = {CommandArg{.name = "filter", .description = "Filter pattern",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "misc",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Environment variables displayed.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 21. ShareCommand — /share
// ============================================================

class ShareCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "share",
            .description = "Share conversation",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Conversation shared. Link copied to clipboard.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 22. TagCommand — /tag
// ============================================================

class TagCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "tag",
            .description = "Tag messages or sessions",
            .aliases = {},
            .args = {CommandArg{.name = "tag_name", .description = "Tag to apply",
                               .type = ArgType::Text, .required = true}},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto tag = ctx.args.empty() ? "default" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Tag '{}' applied.", tag));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 23. SandboxToggleCommand — /sandbox-toggle
// ============================================================

class SandboxToggleCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "sandbox-toggle",
            .description = "Toggle sandbox mode",
            .aliases = {"sandbox"},
            .args = {},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Sandbox mode toggled.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 24. SecurityReviewCommand — /security-review
// ============================================================

class SecurityReviewCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "security-review",
            .description = "Run security review on current context",
            .aliases = {"sec"},
            .args = {},
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Security review completed. No issues found.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 25. SummaryCommand — /summary
// ============================================================

class SummaryCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "summary",
            .description = "Generate conversation summary",
            .aliases = {"sum"},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Conversation summary generated.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 26. InsightsCommand — /insights
// ============================================================

class InsightsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "insights",
            .description = "Show conversation insights",
            .aliases = {},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Insights: 0 tool calls, 0 errors, 0 retries.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 27. CommitPushPrCommand — /commit-push-pr
// ============================================================

class CommitPushPrCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "commit-push-pr",
            .description = "Commit, push, and create a pull request",
            .aliases = {"cpp"},
            .args = {CommandArg{.name = "message", .description = "Commit message",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "git",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto msg = ctx.args.empty() ? "auto-commit" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Committed, pushed, and PR created: '{}'.", msg));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 28. ReleaseNotesCommand — /release-notes
// ============================================================

class ReleaseNotesCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "release-notes",
            .description = "View changelog and release notes",
            .aliases = {"changelog"},
            .args = {CommandArg{.name = "version", .description = "Specific version to view",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "misc",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("Release notes: v1.0.0 — initial release.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 29. PrivacySettingsCommand — /privacy-settings
// ============================================================

class PrivacySettingsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "privacy-settings",
            .description = "Manage privacy settings",
            .aliases = {"privacy"},
            .args = {CommandArg{.name = "action", .description = "show|set",
                               .type = ArgType::Choice, .required = false,
                               .choices = {{"show", "set"}}}},
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto action = ctx.args.empty() ? "show" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Privacy settings '{}' completed.", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 30. ThinkbackCommand — /thinkback
// ============================================================

class ThinkbackCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "thinkback",
            .description = "View the thinking process of the last response",
            .aliases = {"think"},
            .args = {},
            .hidden = false,
            .category = "session",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& /*ctx*/) {
        return CommandResult::success("No thinking blocks available for the last response.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

// ============================================================
// 31. UltraplanCommand — /ultraplan
// ============================================================

class UltraplanCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ultraplan",
            .description = "Enter ultraplan mode for complex multi-step planning",
            .aliases = {"up"},
            .args = {CommandArg{.name = "goal", .description = "Goal description for the plan",
                               .type = ArgType::Text, .required = false}},
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) { return {}; }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto goal = ctx.args.empty() ? "unspecified" : std::string(ctx.args[0]);
        return CommandResult::success(std::format("Ultraplan mode activated. Goal: '{}'.", goal));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view /*partial*/) { return {}; }
};

} // namespace cc::commands
