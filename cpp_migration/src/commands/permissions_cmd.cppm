/// @file permissions_cmd.cppm
/// @brief PermissionsCommand implementing the /permissions slash command.
/// Show current permission mode, switch modes, list allowed/denied tools, reset.
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
#include <array>

export module cc.commands.permissions_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Permission mode controlling tool execution behavior
enum class PermissionMode : std::uint8_t {
    Default,    // Ask before risky operations
    Auto,       // Allow all operations without asking
    Plan,       // Read-only, no tool execution
};

/// Convert permission mode to string
[[nodiscard]] constexpr std::string_view permission_mode_str(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Default: return "default";
        case PermissionMode::Auto:    return "auto";
        case PermissionMode::Plan:    return "plan";
    }
    return "unknown";
}

/// PermissionsCommand implements the /permissions slash command.
/// Manages tool execution permission modes and per-tool allow/deny lists.
class PermissionsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "permissions",
            .description = "Manage tool execution permissions",
            .aliases = {"perms"},
            .args = {
                CommandArg{.name = "action", .description = "show | mode | allow | deny | reset",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"show", "mode", "allow", "deny", "reset"}}},
                CommandArg{.name = "value", .description = "Mode name or tool name",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "config",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid_actions = {"show", "mode", "allow", "deny", "reset"};
        if (std::ranges::find(valid_actions, action) == valid_actions.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: show|mode|allow|deny|reset", action)));
        }
        if (action == "mode" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Usage: /permissions mode <default|auto|plan>"));
        }
        if ((action == "allow" || action == "deny") && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /permissions {} <tool_name>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_status());

        auto action = std::string(ctx.args[0]);

        if (action == "show") return CommandResult::success(format_status());

        if (action == "mode") {
            return set_mode(std::string(ctx.args[1]));
        }
        if (action == "allow") {
            return allow_tool(std::string(ctx.args[1]));
        }
        if (action == "deny") {
            return deny_tool(std::string(ctx.args[1]));
        }
        if (action == "reset") {
            return reset_permissions();
        }

        return CommandResult::success(format_status());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"show", "mode", "allow", "deny", "reset"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        // Also suggest mode values and tool names
        for (auto m : {"default", "auto", "plan"}) {
            if (std::string_view(m).starts_with(partial)) {
                suggestions.emplace_back(m);
            }
        }
        return suggestions;
    }

private:
    PermissionMode mode_ = PermissionMode::Default;
    std::vector<std::string> allowed_tools_;
    std::vector<std::string> denied_tools_;

    [[nodiscard]] std::string format_status() const {
        std::string out = std::format("Permission Mode: {}\n", permission_mode_str(mode_));
        out += std::format("Allowed tools ({}):", allowed_tools_.size());
        for (const auto& t : allowed_tools_) out += std::format(" {}", t);
        out += std::format("\nDenied tools ({}):", denied_tools_.size());
        for (const auto& t : denied_tools_) out += std::format(" {}", t);
        return out;
    }

    [[nodiscard]] Result<CommandResult> set_mode(const std::string& mode_str) {
        if (mode_str == "default") mode_ = PermissionMode::Default;
        else if (mode_str == "auto") mode_ = PermissionMode::Auto;
        else if (mode_str == "plan") mode_ = PermissionMode::Plan;
        else {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid mode: '{}'. Use: default|auto|plan", mode_str)));
        }
        return CommandResult::success(std::format("Permission mode set to: {}", mode_str));
    }

    [[nodiscard]] Result<CommandResult> allow_tool(const std::string& tool) {
        // Remove from denied if present
        std::erase(denied_tools_, tool);
        if (std::ranges::find(allowed_tools_, tool) == allowed_tools_.end()) {
            allowed_tools_.push_back(tool);
        }
        return CommandResult::success(std::format("Tool '{}' added to allow list.", tool));
    }

    [[nodiscard]] Result<CommandResult> deny_tool(const std::string& tool) {
        std::erase(allowed_tools_, tool);
        if (std::ranges::find(denied_tools_, tool) == denied_tools_.end()) {
            denied_tools_.push_back(tool);
        }
        return CommandResult::success(std::format("Tool '{}' added to deny list.", tool));
    }

    [[nodiscard]] Result<CommandResult> reset_permissions() {
        mode_ = PermissionMode::Default;
        allowed_tools_.clear();
        denied_tools_.clear();
        return CommandResult::success("Permissions reset to defaults.");
    }
};

} // namespace cc::commands
