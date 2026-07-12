/// @file brief.cppm
/// @brief BriefCommand implementing the /brief slash command.
/// Toggles brief-only mode via AppState dispatch.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.brief;

import cc.types.types;
import cc.commands.command;
import cc.state.app_state;

export namespace cc::commands {

using namespace cc::core;

/// Ordinal of ActionType::SetBriefOnly in the cc::state::ActionType enum.
/// Keep in sync with store.cppm enum ordering.
constexpr int ACTION_SET_BRIEF_ONLY = 24;

/// BriefCommand implements the /brief slash command.
/// Toggles brief-only mode.
class BriefCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "brief",
            .description = "Toggle brief-only mode",
            .args = {},
            .category = "mode",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Try the AppState bridge first (set by app.cppm).
        if (const void* raw_state = ctx.get_app_state(); raw_state != nullptr) {
            const auto* state = static_cast<const cc::state::AppState*>(raw_state);
            bool current = state->is_brief_only;

            // Determine new value: explicit arg wins, otherwise toggle.
            bool new_val = current;
            if (!ctx.args.empty()) {
                const auto& arg = ctx.args.front();
                if (arg == "on" || arg == "true" || arg == "1" || arg == "enable") {
                    new_val = true;
                } else if (arg == "off" || arg == "false" || arg == "0" || arg == "disable") {
                    new_val = false;
                } else {
                    new_val = !current;
                }
            } else {
                new_val = !current;
            }

            ctx.dispatch_action(ACTION_SET_BRIEF_ONLY, &new_val);

            if (new_val) {
                return CommandResult::success("Brief-only mode enabled");
            } else {
                return CommandResult::success("Brief-only mode disabled");
            }
        }

        // Fallback: static local when no AppState bridge is wired (e.g. unit tests).
        static bool brief_mode = false;
        if (!ctx.args.empty()) {
            const auto& arg = ctx.args.front();
            if (arg == "on" || arg == "true" || arg == "1" || arg == "enable") {
                brief_mode = true;
            } else if (arg == "off" || arg == "false" || arg == "0" || arg == "disable") {
                brief_mode = false;
            } else {
                brief_mode = !brief_mode;
            }
        } else {
            brief_mode = !brief_mode;
        }

        if (brief_mode) {
            return CommandResult::success("Brief-only mode enabled");
        } else {
            return CommandResult::success("Brief-only mode disabled");
        }
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands