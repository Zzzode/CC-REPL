/// @file ide.cppm
/// @brief IdeCommand implementing the /ide slash command.
/// Detects running IDEs via the ~/.claude/ide/ lockfiles and reports each
/// one's MCP connection endpoint, transport, and workspace.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.ide;

import cc.types.types;
import cc.commands.command;
import cc.utils.ide_integration;

export namespace cc::commands {

using namespace cc::core;

/// IdeCommand implements the /ide slash command.
/// Scans the IDE lockfile directory for live editors and reports each one's
/// name, PID, MCP endpoint, transport, and workspace.
class IdeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "ide",
            .description = "Detect running IDEs and their MCP endpoints",
            .args = {},
            .category = "configuration",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        cc::utils::ide::IdeLockfileScanner scanner;
        auto lockfiles = scanner.scan();

        if (lockfiles.empty()) {
            return CommandResult::success(
                "No running IDE detected.\n\n"
                "To connect an IDE:\n"
                "  - Install the Claude Code extension in VS Code / Cursor / Windsurf / JetBrains,\n"
                "  - or launch your editor from a Claude Code session so it writes a lockfile to "
                + scanner.lockfile_dir().string() + ".");
        }

        std::string out = std::format("Detected {} running IDE(s):\n\n", lockfiles.size());
        for (const auto& lf : lockfiles) {
            out += std::format("  {} (pid {})\n", lf.name, lf.pid);
            out += std::format("    MCP endpoint: {}\n", lf.mcp_url());
            if (lf.transport) {
                out += std::format("    Transport: {}\n", *lf.transport);
            }
            if (!lf.workspace_folder.empty()) {
                out += std::format("    Workspace: {}\n", lf.workspace_folder.string());
            }
            out += "\n";
        }
        out += "The most recently active IDE is used automatically for diffs, "
               "file navigation, and MCP tool bridging.";
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
