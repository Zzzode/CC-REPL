/// @file config.cppm
/// @brief ConfigCommand implementing the /config slash command.
/// Lists, gets, sets settings; opens config file in editor;
/// shows config file locations; validates config values.
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
#include <unordered_map>
#include <filesystem>
#include <array>

export module cc.commands.config;

import cc.types.types;
import cc.commands.command;
import cc.config.config;

export namespace cc::commands {

using namespace cc::core;

/// Subcommand for /config
enum class ConfigAction : std::uint8_t {
    List,       // List all settings
    Get,        // Get a specific setting
    Set,        // Set a specific setting
    Open,       // Open config file in editor
    Path,       // Show config file locations
};

/// Known configuration keys with their metadata
struct ConfigKeyInfo {
    std::string_view key;
    std::string_view description;
    std::string_view type;          // "string", "bool", "int", "enum"
    std::optional<std::string_view> default_value;
};

/// ConfigCommand implements the /config slash command.
/// Provides get/set/list/open/path subcommands for managing settings.
class ConfigCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "config",
            .description = "View and modify CLI configuration settings",
            .args = {
                CommandArg{.name = "action", .description = "Subcommand: list, get, set, open, path",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"list", "get", "set", "open", "path"}},
                CommandArg{.name = "key", .description = "Configuration key (for get/set)",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "value", .description = "New value (for set)",
                           .type = ArgType::Text, .required = false},
            },
            .category = "session",
            .aliases = {"cfg"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // Default to 'list'

        auto action = parse_action(ctx.args[0]);
        if (!action) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Unknown config action: '{}'. Use: list, get, set, open, path",
                           ctx.args[0])
            ));
        }

        // 'get' and 'set' require a key
        if (*action == ConfigAction::Get && ctx.args.size() < 2) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "/config get requires a key name"
            ));
        }
        if (*action == ConfigAction::Set && ctx.args.size() < 3) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "/config set requires a key and value"
            ));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Default action: list
        auto action = ctx.args.empty()
            ? ConfigAction::List
            : parse_action(ctx.args[0]).value_or(ConfigAction::List);

        switch (action) {
            case ConfigAction::List: return execute_list();
            case ConfigAction::Get:  return execute_get(ctx.args[1]);
            case ConfigAction::Set:  return execute_set(ctx.args[1], ctx.args[2]);
            case ConfigAction::Open: return execute_open();
            case ConfigAction::Path: return execute_path();
        }
        return CommandResult::fail("Unknown config action");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        // First argument: subcommands
        static constexpr std::array actions = {"list", "get", "set", "open", "path"};
        std::vector<std::string> suggestions;

        for (auto act : actions) {
            if (std::string_view(act).starts_with(partial)) {
                suggestions.emplace_back(act);
            }
        }

        // If partial looks like a config key, suggest known keys
        if (partial.contains('.')) {
            for (const auto& info : known_keys()) {
                if (info.key.starts_with(partial)) {
                    suggestions.emplace_back(info.key);
                }
            }
        }
        return suggestions;
    }

private:
    ConfigManager config_manager_;

    /// Parse action string to enum
    [[nodiscard]] static std::optional<ConfigAction> parse_action(std::string_view str) {
        if (str == "list" || str == "ls")  return ConfigAction::List;
        if (str == "get")                   return ConfigAction::Get;
        if (str == "set")                   return ConfigAction::Set;
        if (str == "open" || str == "edit") return ConfigAction::Open;
        if (str == "path" || str == "paths") return ConfigAction::Path;
        return std::nullopt;
    }

    /// List all current configuration settings
    [[nodiscard]] Result<CommandResult> execute_list() {
        const auto& settings = config_manager_.settings();
        std::string output = "Current Configuration:\n\n";

        output += std::format("  model.default_model     = {}\n", settings.model.default_model);
        output += std::format("  model.max_output_tokens = {}\n", settings.model.max_output_tokens);
        output += std::format("  model.extended_thinking = {}\n",
                             settings.model.extended_thinking ? "true" : "false");
        output += std::format("  model.context_window    = {}\n", settings.model.context_window_size);
        output += "\n";
        output += std::format("  display.show_thinking   = {}\n",
                             settings.display.show_thinking ? "true" : "false");
        output += std::format("  display.show_tokens     = {}\n",
                             settings.display.show_token_usage ? "true" : "false");
        output += std::format("  display.theme           = {}\n", settings.display.theme);
        output += "\n";
        output += std::format("  network.timeout         = {}s\n", settings.network.timeout_seconds);
        output += std::format("  network.max_retries     = {}\n", settings.network.max_retries);
        output += std::format("  network.verify_ssl      = {}\n",
                             settings.network.verify_ssl ? "true" : "false");
        output += std::format("  network.api_key         = {}\n",
                             settings.network.api_key ? "***" : "(not set)");
        output += "\n";
        output += std::format("  permissions.allow_bash  = {}\n",
                             settings.permissions.allow_bash ? "true" : "false");
        output += std::format("  permissions.allow_write = {}\n",
                             settings.permissions.allow_file_write ? "true" : "false");

        return CommandResult::success(std::move(output));
    }

    /// Get a specific configuration value
    [[nodiscard]] Result<CommandResult> execute_get(std::string_view key) {
        auto value = resolve_key(key);
        if (!value) {
            return CommandResult::fail(std::format("Unknown config key: '{}'", key));
        }
        return CommandResult::success(std::format("{} = {}", key, *value));
    }

    /// Set a specific configuration value
    [[nodiscard]] Result<CommandResult> execute_set(std::string_view key, std::string_view value) {
        // Validate the value before applying
        if (auto result = validate_value(key, value); !result) {
            return std::unexpected(result.error());
        }

        auto apply_result = apply_setting(key, value);
        if (!apply_result) return std::unexpected(apply_result.error());

        // Persist changes
        if (auto save_result = config_manager_.save(); !save_result) {
            return std::unexpected(save_result.error());
        }

        return CommandResult::success(std::format("Set {} = {}", key, value));
    }

    /// Open config file in the user's editor
    [[nodiscard]] Result<CommandResult> execute_open() {
        auto path = config_manager_.project_config_path();
        if (!std::filesystem::exists(path)) {
            path = config_manager_.global_config_path();
        }
        // Return path for the shell to open with $EDITOR
        return CommandResult::success(
            std::format("Opening config file: {}\nRun: $EDITOR {}", path.string(), path.string())
        );
    }

    /// Show config file locations
    [[nodiscard]] Result<CommandResult> execute_path() {
        auto global = config_manager_.global_config_path();
        auto project = config_manager_.project_config_path();

        std::string output = "Configuration file locations:\n\n";
        output += std::format("  Global:  {} {}\n", global.string(),
                             std::filesystem::exists(global) ? "(exists)" : "(not found)");
        output += std::format("  Project: {} {}\n", project.string(),
                             std::filesystem::exists(project) ? "(exists)" : "(not found)");
        output += "\nPriority: CLI flags > env vars > project config > global config > defaults\n";

        return CommandResult::success(std::move(output));
    }

    /// Resolve a dotted config key to its current value
    [[nodiscard]] std::optional<std::string> resolve_key(std::string_view key) const {
        const auto& s = config_manager_.settings();
        if (key == "model.default_model")      return s.model.default_model;
        if (key == "model.max_output_tokens")  return std::to_string(s.model.max_output_tokens);
        if (key == "model.extended_thinking")  return s.model.extended_thinking ? "true" : "false";
        if (key == "display.show_thinking")    return s.display.show_thinking ? "true" : "false";
        if (key == "display.theme")            return s.display.theme;
        if (key == "network.timeout")          return std::to_string(s.network.timeout_seconds);
        if (key == "network.max_retries")      return std::to_string(s.network.max_retries);
        if (key == "permissions.allow_bash")   return s.permissions.allow_bash ? "true" : "false";
        return std::nullopt;
    }

    /// Validate a value before applying it
    [[nodiscard]] static VoidResult validate_value(std::string_view key, std::string_view value) {
        // Boolean keys
        if (key.ends_with("thinking") || key.starts_with("permissions.") ||
            key == "network.verify_ssl" || key == "display.show_tokens") {
            if (value != "true" && value != "false") {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("'{}' must be 'true' or 'false', got: '{}'", key, value)
                ));
            }
        }
        // Integer keys
        if (key == "model.max_output_tokens" || key == "network.timeout" ||
            key == "network.max_retries") {
            try { (void)std::stoul(std::string(value)); }
            catch (...) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest,
                    std::format("'{}' must be a positive integer, got: '{}'", key, value)
                ));
            }
        }
        return {};
    }

    /// Apply a validated setting to the config manager
    [[nodiscard]] VoidResult apply_setting(std::string_view key, std::string_view value) {
        auto& s = config_manager_.settings_mut();
        if (key == "model.default_model")          { s.model.default_model = value; return {}; }
        if (key == "model.max_output_tokens")      { s.model.max_output_tokens = std::stoul(std::string(value)); return {}; }
        if (key == "model.extended_thinking")      { s.model.extended_thinking = (value == "true"); return {}; }
        if (key == "display.show_thinking")        { s.display.show_thinking = (value == "true"); return {}; }
        if (key == "display.theme")                { s.display.theme = value; return {}; }
        if (key == "network.timeout")              { s.network.timeout_seconds = std::stoul(std::string(value)); return {}; }
        if (key == "network.max_retries")          { s.network.max_retries = std::stoul(std::string(value)); return {}; }
        if (key == "permissions.allow_bash")       { s.permissions.allow_bash = (value == "true"); return {}; }

        return std::unexpected(Error::make(ErrorCode::ConfigNotFound,
            std::format("Unknown or read-only key: '{}'", key)));
    }

    /// Get all known configuration keys with metadata
    [[nodiscard]] static std::vector<ConfigKeyInfo> known_keys() {
        return {
            {"model.default_model",     "LLM model to use",             "string", "claude-sonnet-4-20250514"},
            {"model.max_output_tokens", "Maximum output token count",   "int",    "16384"},
            {"model.extended_thinking", "Enable extended thinking",     "bool",   "false"},
            {"display.show_thinking",   "Show thinking blocks",         "bool",   "true"},
            {"display.theme",           "Color theme (auto/dark/light)","enum",   "auto"},
            {"network.timeout",         "Request timeout (seconds)",    "int",    "120"},
            {"network.max_retries",     "Max retry attempts",           "int",    "3"},
            {"permissions.allow_bash",  "Allow bash execution",         "bool",   "true"},
        };
    }
};

} // namespace cc::commands
