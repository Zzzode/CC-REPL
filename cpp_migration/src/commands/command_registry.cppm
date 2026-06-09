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

export namespace cc::commands {

using namespace cc::core;

/// Defined in command_registry_init.cpp (module implementation unit).
/// Registers all built-in commands into the given registry.
void register_default_commands(CommandRegistry& registry);

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
        name == "stickers" || name == "ant-trace" || name == "bughunter" || name == "debug-tool-call" ||
        name == "extra-usage" || name == "init-verifiers" || name == "onboarding" ||
        name == "perf-issue" || name == "pr-comments" || name == "rate-limit-options" ||
        name == "remote-env" || name == "statusline" || name == "terminal-setup" ||
        name == "thinkback-play" || name == "version")
        return CommandPermission::ReadOnly;
    
    if (name == "clear" || name == "compact" || name == "add-dir" || name == "commit" || name == "config" ||
        name == "mcp" || name == "bridge-kick" || name == "rename" || name == "rewind" || name == "share" ||
        name == "upgrade" || name == "install" || name == "install-slack-app" ||
        name == "autofix-pr" || name == "backfill-sessions" || name == "break-cache" ||
        name == "bridge" || name == "create-moved-to-plugin-command" || name == "install-github-app" ||
        name == "mock-limits" || name == "oauth-refresh" || name == "reload-plugins" ||
        name == "remote-setup" || name == "reset-limits")
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

        auto result = registry_.execute(parsed->raw, ctx);
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
    /// Register all built-in commands (defined in command_registry_init.cpp)
    void register_all_commands() {
        register_default_commands(registry_);
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
