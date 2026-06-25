/// @file install.cppm
/// @brief InstallCommand implementing the /install slash command.
/// Reports the current native build and installation guidance. The native
/// binary is its own self-contained distribution, so (unlike the TS CLI) there
/// is no npm installer to invoke.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.install;

import cc.types.types;
import cc.commands.command;
import cc.constants.product;

export namespace cc::commands {

using namespace cc::core;

/// InstallCommand implements the /install slash command.
/// The C++ build ships as a self-contained native binary; there is no npm
/// installer to run, so this reports the current build and how to update it.
class InstallCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install",
            .description = "Show native build info and installation guidance",
            .args = {},
            .category = "maintenance",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        std::string out = "Claude Code (native C++ build):\n";
        out += std::format("  Version: {}\n", cc::constants::product::CC_REPL_VERSION);

        out += "\nThe native build has no npm installer. To update:\n";
        out += "  1. Pull the latest source.\n";
        out += "  2. Reconfigure with CMake (cmake --preset debug).\n";
        out += "  3. Build (cmake --build --preset debug --target cc_repl).\n";
        out += "  4. Copy the resulting binary (build/debug/bin/cc-repl) onto your PATH.";
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
