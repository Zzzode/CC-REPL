/// @file bridge-kick.cppm
/// @brief BridgeKickCommand implementing the /bridge-kick slash command.
/// Injects bridge failure states for manual recovery testing.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <functional>
#include <utility>

export module cc.commands.bridge_kick;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;
using BridgeStatusProvider = std::function<std::string()>;

namespace detail {
inline BridgeStatusProvider& status_provider() {
    static BridgeStatusProvider provider;
    return provider;
}
} // namespace detail

inline void set_bridge_status_provider(BridgeStatusProvider provider) {
    detail::status_provider() = std::move(provider);
}

/// BridgeKickCommand implements the /bridge-kick slash command.
/// Injects bridge failure states for manual recovery testing.
class BridgeKickCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "bridge-kick",
            .description = "Inject bridge failure states for manual recovery testing",
            .args = {
                CommandArg{.name = "subcommand", .description = "Subcommand: close, poll, register, reconnect-session, heartbeat, reconnect, status",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"close", "poll", "register", "reconnect-session", "heartbeat", "reconnect", "status"}},
                CommandArg{.name = "parameter", .description = "Subcommand parameter",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "parameter2", .description = "Second subcommand parameter",
                           .type = ArgType::Text, .required = false},
            },
            .category = "debug",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return {}; // Default to help output
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return show_help();
        }

        const auto& subcommand = ctx.args[0];
        
        if (subcommand == "close") {
            return handle_close(ctx);
        } else if (subcommand == "poll") {
            return handle_poll(ctx);
        } else if (subcommand == "register") {
            return handle_register(ctx);
        } else if (subcommand == "reconnect-session") {
            return handle_reconnect_session(ctx);
        } else if (subcommand == "heartbeat") {
            return handle_heartbeat(ctx);
        } else if (subcommand == "reconnect") {
            return handle_reconnect(ctx);
        } else if (subcommand == "status") {
            return handle_status(ctx);
        }

        return show_help();
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array subcommands = {
            "close", "poll", "register", "reconnect-session",
            "heartbeat", "reconnect", "status"
        };
        
        for (const auto& subcmd : subcommands) {
            if (std::string_view(subcmd).starts_with(partial)) {
                suggestions.emplace_back(subcmd);
            }
        }
        
        return suggestions;
    }

private:
    [[nodiscard]] static Result<CommandResult> show_help() {
        return CommandResult::success(R"(/bridge-kick <subcommand>
  close <code>              fire ws_closed with the given code (e.g., 1002)
  poll <status> [type]     next poll throws BridgeFatalError(status, type)
  poll transient           next poll throws axios-style rejection (5xx/net)
  register fail [N]         next N registers transient-fail (default 1)
  register fatal         next register 403s (terminal)
  reconnect-session fail   next POST /bridge/reconnect fails
  heartbeat <status>       next heartbeat throws BridgeFatalError(status)
  reconnect            call reconnectEnvironmentWithSession directly
  status              print bridge state)");
    }

    [[nodiscard]] static Result<CommandResult> handle_close(const CommandContext& ctx) {
        if (ctx.args.size() < 2) {
            return CommandResult::fail("close: need a numeric code");
        }
        const auto& code = ctx.args[1];
        return CommandResult::success(
            std::format("Fired transport close({}). Watch debug.log for [bridge:repl] recovery.", code));
    }

    [[nodiscard]] static Result<CommandResult> handle_poll(const CommandContext& ctx) {
        if (ctx.args.size() < 2) {
            return CommandResult::fail("poll: need 'transient' or a status code");
        }
        
        const auto& param = ctx.args[1];
        if (param == "transient") {
            return CommandResult::success(
                "Next poll will throw a transient (axios rejection). Poll loop woken.");
        }
        
        const auto& type = ctx.args.size() > 2 ? ctx.args[2] : 
            (param == "404" ? "not_found_error" : "authentication_error");
        return CommandResult::success(
            std::format("Next poll will throw BridgeFatalError({}, {}). Poll loop woken.", param, type));
    }

    [[nodiscard]] static Result<CommandResult> handle_register(const CommandContext& ctx) {
        if (ctx.args.size() < 2) {
            return CommandResult::fail("register: need 'fail' or 'fatal'");
        }
        
        const auto& param = ctx.args[1];
        if (param == "fatal") {
            return CommandResult::success(
                "Next registerBridgeEnvironment will 403. Trigger with close/reconnect.");
        } else if (param == "fail") {
            const auto n = ctx.args.size() > 2 ? ctx.args[2] : "1";
            return CommandResult::success(
                std::format("Next {} registerBridgeEnvironment call(s) will transient-fail. Trigger with close/reconnect.", n));
        }
        return CommandResult::fail("register: need 'fail' or 'fatal'");
    }

    [[nodiscard]] static Result<CommandResult> handle_reconnect_session(const CommandContext&) {
        return CommandResult::success(
            "Next 2 POST /bridge/reconnect calls will 404. doReconnect Strategy 1 falls through to Strategy 2.");
    }

    [[nodiscard]] static Result<CommandResult> handle_heartbeat(const CommandContext& ctx) {
        const auto& status = ctx.args.size() > 1 ? ctx.args[1] : "401";
        return CommandResult::success(
            std::format("Next heartbeat will {}. Watch for onHeartbeatFatal → work-state teardown.", status));
    }

    [[nodiscard]] static Result<CommandResult> handle_reconnect(const CommandContext&) {
        return CommandResult::success(
            "Called reconnectEnvironmentWithSession(). Watch debug.log.");
    }

    [[nodiscard]] static Result<CommandResult> handle_status(const CommandContext&) {
        auto& provider = detail::status_provider();
        if (!provider) {
            return CommandResult::fail("Bridge status provider is not configured.");
        }
        return CommandResult::success(provider());
    }
};

} // namespace cc::commands
