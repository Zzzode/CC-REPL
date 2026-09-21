/// @file fast.cppm
/// @brief FastCommand implementing the /fast slash command.
/// Toggles fast mode.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.fast;

import cc.types.types;
import cc.commands.command;
import cc.state.app_state;

export namespace cc::commands {

using namespace cc::core;

/// Action type ordinal for SetFastMode (from cc::state::ActionType in store.cppm).
inline constexpr int ACTION_SET_FAST_MODE = 42;

/// FastCommand implements the /fast slash command.
/// Toggles fast mode via AppState dispatch.
class FastCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "fast",
            .description = "Toggle fast mode",
            .args = {
                CommandArg{.name = "state", .description = "Explicitly set state: on or off",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"on", "off"}},
            },
            .category = "mode",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Fallback for when AppState bridge is not available.
        static bool fallback_fast_mode = false;

        // Read current state from AppState when available, else use static fallback.
        bool current = fallback_fast_mode;
        std::optional<std::string> current_model_id;
        if (const void* raw = ctx.get_app_state()) {
            const auto* state = static_cast<const cc::state::AppState*>(raw);
            current = state->fast_mode;
            if (!state->current_model.model_id.empty()) {
                current_model_id = state->current_model.model_id;
            }
        }

        // Determine the new value.
        bool new_value;
        if (!ctx.args.empty()) {
            const auto& arg = ctx.args[0];
            if (arg == "on") {
                new_value = true;
            } else if (arg == "off") {
                new_value = false;
            } else {
                // Unrecognised arg — treat as toggle.
                new_value = !current;
            }
        } else {
            // No arg: toggle.
            new_value = !current;
        }

        // Keep fallback in sync so non-AppState callers still see the right value.
        fallback_fast_mode = new_value;

        // Dispatch SetFastMode action with bool payload.
        ctx.dispatch_action(ACTION_SET_FAST_MODE, &new_value);

        // Build response message.
        if (new_value) {
            std::string msg = "⚡ Fast mode ON";
            if (current_model_id.has_value()) {
                msg += std::format(" (active for model: {})", *current_model_id);
            }
            return CommandResult::success(std::move(msg));
        }
        return CommandResult::success("Fast mode OFF");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        
        static constexpr std::array states = {"on", "off"};
        
        for (const auto& state : states) {
            if (std::string_view(state).starts_with(partial)) {
                suggestions.emplace_back(state);
            }
        }
        
        return suggestions;
    }
};

} // namespace cc::commands
