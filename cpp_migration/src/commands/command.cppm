/// @file command.cppm
/// @brief Slash command module for handling CLI commands like /commit, /review, /clear,
/// /config, /help, etc. Provides command registry, parsing, and execution.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <optional>
#include <sstream>
#include <algorithm>
#include <format>
#include <memory>
#include <numeric>
#include <span>

export module cc.commands.command;

import cc.types.types;

export namespace cc::core {

// ============================================================
// Command Context
// ============================================================

/// Slash command argument type
enum class ArgType : std::uint8_t {
    None,
    Text,
    String = Text,
    FilePath,
    Number,
    Boolean,
    Choice,
};

/// Slash command argument definition
struct CommandArg {
    std::string name;
    std::string description;
    ArgType type{ArgType::Text};
    bool required{false};
    std::vector<std::string> choices{};
    std::optional<std::string> default_value{};
};

/// Registered command metadata used by help, completion and dispatch.
struct CommandDefinition {
    std::string name;
    std::string description;
    std::vector<CommandArg> args{};
    std::string category{"general"};
    std::vector<std::string> aliases{};
    bool hidden{false};
};

/// Parsed command invocation.
struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
    std::string raw;
};

/// Command execution status.
enum class CommandStatus : std::uint8_t {
    Succeeded,
    Failed,
    Injected,
};

/// Context available to command handlers
struct CommandContext {
    std::vector<std::string> args;
    std::string raw_input;
};

// ============================================================
// Command Result
// ============================================================

/// Result of command execution
struct CommandResult {
    bool ok{true};
    std::string message;
    std::optional<std::string> metadata{};
    CommandStatus status{CommandStatus::Succeeded};

    [[nodiscard]] static CommandResult success(std::string message) {
        return CommandResult{true, std::move(message), std::nullopt, CommandStatus::Succeeded};
    }

    [[nodiscard]] static CommandResult fail(std::string message) {
        return CommandResult{false, std::move(message), std::nullopt, CommandStatus::Failed};
    }

    [[nodiscard]] static CommandResult inject(std::string prompt) {
        return CommandResult{true, std::move(prompt), std::nullopt, CommandStatus::Injected};
    }

    [[nodiscard]] static CommandResult exit() {
        return CommandResult{true, "Goodbye!", "EXIT", CommandStatus::Succeeded};
    }
};

// ============================================================
// Command Handler
// ============================================================

using CommandHandler = std::function<CommandResult(const CommandContext&)>;

/// Polymorphic command interface used by the registry.
class ICommand {
public:
    virtual ~ICommand() = default;
    [[nodiscard]] virtual const CommandDefinition& definition() const noexcept = 0;
    [[nodiscard]] virtual VoidResult validate(const CommandContext& ctx) = 0;
    [[nodiscard]] virtual Result<CommandResult> execute(const CommandContext& ctx) = 0;
    [[nodiscard]] virtual std::vector<std::string> complete(std::string_view partial) { (void)partial; return {}; }
};

template <typename Command>
class CommandModel final : public ICommand {
    Command command_{};
    CommandDefinition definition_{Command::definition()};

public:
    [[nodiscard]] const CommandDefinition& definition() const noexcept override { return definition_; }
    [[nodiscard]] VoidResult validate(const CommandContext& ctx) override { return command_.validate(ctx); }
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) override { return command_.execute(ctx); }
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) override { return command_.complete(partial); }
};

/// Registered command information
struct CommandRegistration {
    std::string name;
    std::string description;
    std::string usage;
    CommandHandler handler;
    std::vector<std::string> aliases;
    bool hidden = false;
};

// ============================================================
// Command Registry
// ============================================================

/// Registry for slash commands
class CommandRegistry {
    std::unordered_map<std::string, CommandRegistration> legacy_commands_;
    std::unordered_map<std::string, std::unique_ptr<ICommand>> commands_;
    std::unordered_map<std::string, std::string> alias_map_;

public:
    CommandRegistry() = default;

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;
    CommandRegistry(CommandRegistry&&) noexcept = default;
    CommandRegistry& operator=(CommandRegistry&&) noexcept = default;

    /// Register a command
    void register_command(CommandRegistration cmd) {
        auto name = cmd.name;
        legacy_commands_[name] = std::move(cmd);
        for (const auto& alias : legacy_commands_[name].aliases) {
            alias_map_[alias] = name;
        }
    }

    template <typename Command>
    void register_command() {
        auto command = std::make_unique<CommandModel<Command>>();
        auto name = command->definition().name;
        for (const auto& alias : command->definition().aliases) {
            alias_map_[alias] = name;
        }
        commands_[name] = std::move(command);
    }

    /// Check if a command exists
    [[nodiscard]] bool has_command(const std::string& name) const {
        return contains(name);
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        auto key = std::string(name);
        if (commands_.contains(key) || legacy_commands_.contains(key)) return true;
        auto alias = alias_map_.find(key);
        return alias != alias_map_.end() && (commands_.contains(alias->second) || legacy_commands_.contains(alias->second));
    }

    /// Get command by name or alias
    [[nodiscard]] const CommandRegistration* get_command(const std::string& name) const {
        auto it = legacy_commands_.find(name);
        if (it != legacy_commands_.end()) {
            return &it->second;
        }
        auto alias_it = alias_map_.find(name);
        if (alias_it != alias_map_.end()) {
            return get_command(alias_it->second);
        }
        return nullptr;
    }

    [[nodiscard]] ICommand* get(std::string_view name) const {
        auto key = std::string(name);
        auto it = commands_.find(key);
        if (it != commands_.end()) return it->second.get();

        auto alias_it = alias_map_.find(key);
        if (alias_it != alias_map_.end()) {
            auto target = commands_.find(alias_it->second);
            if (target != commands_.end()) return target->second.get();
        }

        return nullptr;
    }

    [[nodiscard]] static std::optional<ParsedCommand> parse(std::string_view input) {
        if (input.empty() || input.front() != '/') return std::nullopt;
        ParsedCommand parsed;
        parsed.raw = std::string(input);
        input.remove_prefix(1);
        std::istringstream iss{std::string(input)};
        iss >> parsed.name;
        std::string arg;
        while (iss >> arg) parsed.args.push_back(arg);
        if (parsed.name.empty()) return std::nullopt;
        return parsed;
    }

    /// Execute a command from input
    [[nodiscard]] std::optional<CommandResult> execute(const std::string& input) const {
        if (input.empty() || input[0] != '/') {
            return std::nullopt;
        }

        std::istringstream iss(input.substr(1));
        std::string cmd_name;
        iss >> cmd_name;

        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        if (auto* typed_cmd = get(cmd_name)) {
            CommandContext ctx{args, input};
            if (auto validation = typed_cmd->validate(ctx); !validation) {
                return CommandResult::fail(validation.error().message);
            }
            auto result = typed_cmd->execute(ctx);
            if (!result) {
                return CommandResult::fail(result.error().message);
            }
            return *result;
        }

        const CommandRegistration* cmd = get_command(cmd_name);
        if (!cmd) {
            return CommandResult{false, std::format("Unknown command: /{}", cmd_name), std::nullopt};
        }

        CommandContext ctx{args, input};
        return cmd->handler(ctx);
    }

    /// Get all visible commands
    [[nodiscard]] std::vector<CommandRegistration> get_visible_commands() const {
        std::vector<CommandRegistration> result;
        for (const auto& [name, cmd] : legacy_commands_) {
            if (!cmd.hidden) {
                result.push_back(cmd);
            }
        }
        return result;
    }

    /// Generate help text
    [[nodiscard]] std::string generate_help() const {
        std::string help = "Available commands:\n\n";
        for (const auto& [name, cmd] : legacy_commands_) {
            if (cmd.hidden) continue;
            help += std::format("/{} - {}\n", name, cmd.description);
            help += std::format("  Usage: {}\n", cmd.usage);
            if (!cmd.aliases.empty()) {
                help += std::format("  Aliases: {}\n", 
                    std::accumulate(cmd.aliases.begin(), cmd.aliases.end(), std::string(),
                        [](const std::string& a, const std::string& b) {
                            return a.empty() ? b : a + ", " + b;
                        }));
            }
            help += "\n";
        }
        for (const auto& [name, cmd] : commands_) {
            const auto& def = cmd->definition();
            if (def.hidden) continue;
            help += std::format("/{} - {}\n", name, def.description);
        }
        return help;
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial_input) const {
        std::vector<std::string> result;
        auto prefix = std::string(partial_input);
        if (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
        for (const auto& [name, _] : commands_) {
            if (name.starts_with(prefix)) result.push_back('/' + name);
        }
        for (const auto& [name, _] : legacy_commands_) {
            if (name.starts_with(prefix)) result.push_back('/' + name);
        }
        return result;
    }

    [[nodiscard]] std::vector<const CommandDefinition*> visible_commands() const {
        std::vector<const CommandDefinition*> result;
        for (const auto& [_, cmd] : commands_) {
            if (!cmd->definition().hidden) result.push_back(&cmd->definition());
        }
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return commands_.size() + legacy_commands_.size();
    }
};

/// Create a default command registry with built-in commands
CommandRegistry create_default_registry() {
    CommandRegistry registry;

    // /help command
    registry.register_command(CommandRegistration{
        "help",
        "Show this help message",
        "/help",
        [](const CommandContext&) -> CommandResult {
            return {true, "Available commands: /help, /clear, /exit, /config, /cost", std::nullopt};
        },
        {"?"},
        false
    });

    // /clear command
    registry.register_command(CommandRegistration{
        "clear",
        "Clear the current conversation",
        "/clear",
        [](const CommandContext&) -> CommandResult {
            return {true, "Conversation cleared", std::nullopt};
        },
        {"cls"},
        false
    });

    // /exit command
    registry.register_command(CommandRegistration{
        "exit",
        "Exit the application",
        "/exit",
        [](const CommandContext&) -> CommandResult {
            return {true, "Goodbye!", "EXIT"};
        },
        {"quit", "q"},
        false
    });

    // /config command
    registry.register_command(CommandRegistration{
        "config",
        "Show or modify configuration",
        "/config [key] [value]",
        [](const CommandContext& ctx) -> CommandResult {
            if (ctx.args.empty()) {
                return {true, "Current configuration: use /config <key> <value> to update settings", std::nullopt};
            }
            return {true, std::format("Config set: {} = {}", ctx.args[0], 
                ctx.args.size() > 1 ? ctx.args[1] : ""), std::nullopt};
        },
        {"set"},
        false
    });

    // /cost command
    registry.register_command(CommandRegistration{
        "cost",
        "Show session cost and usage",
        "/cost",
        [](const CommandContext&) -> CommandResult {
            return {true, "Session cost and usage: local counters are tracked by the cost command module", std::nullopt};
        },
        {},
        false
    });

    return registry;
}

} // namespace cc::core
