/// @file heapdump.cppm
/// @brief HeapdumpCommand implementing the /heapdump slash command.
/// Heapdump generation command metadata and runtime guidance.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.heapdump;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// HeapdumpCommand implements the /heapdump slash command.
/// Heapdump generation command metadata and runtime guidance.
class HeapdumpCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "heapdump",
            .description = "Dump the JS heap to ~/Desktop",
            .args = {},
            .category = "debug",
            .aliases = {},
            .hidden = true,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        return CommandResult::success(
            "Heap diagnostics are registered for parity with the TypeScript CLI. "
            "The C++ runtime does not expose a V8 heap snapshot; use platform memory diagnostics for this build."
        );
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
