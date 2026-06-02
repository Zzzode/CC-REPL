/// @file ctx-viz.cppm
/// @brief CtxVizCommand implementing the /ctx-viz slash command.
/// Context visualization command for inspecting context-window state.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.ctx_viz;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// CtxVizCommand implements the /ctx-viz slash command.
/// Context visualization command for inspecting context-window state.
class CtxVizCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ctx-viz",
            .description = "Context visualization",
            .args = {},
            .category = "debug",
            .aliases = {},
            .hidden = true,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto detail = ctx.args.empty() ? std::string{"summary"} : ctx.args.front();
        return CommandResult::success(std::format(
            "Context visualization mode: {}\n"
            "Input bytes: {}\n"
            "Arguments: {}\n"
            "Use /context for the full context listing and /cost for token usage.",
            detail,
            ctx.raw_input.size(),
            ctx.args.size()
        ));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions{"summary", "tokens", "files", "tools"};
        std::erase_if(suggestions, [partial](const auto& value) {
            return !partial.empty() && !value.starts_with(partial);
        });
        return suggestions;
    }
};

} // namespace cc::commands
