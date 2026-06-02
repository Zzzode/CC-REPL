/// @file memory.cppm
/// @brief MemoryCommand implementing the /memory slash command.
/// Edit persistent memory files (CLAUDE.md) that provide instructions across sessions.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <filesystem>

export module cc.commands.memory;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;
namespace fs = std::filesystem;

/// MemoryCommand implements the /memory slash command.
/// Manages persistent memory files (CLAUDE.md) that provide instructions across sessions.
class MemoryCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "memory",
            .description = "Edit persistent memory files (CLAUDE.md)",
            .args = {CommandArg{.name = "action", .description = "view or edit", .type = ArgType::Text, .required = false}},
            .category = "config",
            .aliases = {"mem"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        // Memory file locations (in priority order)
        struct MemFile {
            std::string path;
            std::string label;
        };

        auto home = std::string(std::getenv("HOME") ? std::getenv("HOME") : "~");
        std::vector<MemFile> locations = {
            {home + "/.claude/CLAUDE.md", "user (global)"},
            {".claude/CLAUDE.md", "project (local)"},
            {"CLAUDE.md", "project root"},
        };

        std::string output;
        output += "Memory files (CLAUDE.md):\n\n";

        bool any_exists = false;
        for (const auto& loc : locations) {
            std::error_code ec;
            bool exists = fs::exists(loc.path, ec);
            if (exists) {
                auto size = fs::file_size(loc.path, ec);
                output += std::format("  [{}] {} ({} bytes)\n", loc.label, loc.path, size);
                any_exists = true;
            } else {
                output += std::format("  [{}] {} (not found)\n", loc.label, loc.path);
            }
        }

        if (!any_exists) {
            output += "\nNo memory files found. Create one with:\n";
            output += "  mkdir -p .claude && touch .claude/CLAUDE.md\n";
        }

        output += "\nTo edit a memory file, ask the assistant to read and modify it.";

        return CommandResult::success(std::move(output));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view prefix) {
        std::vector<std::string> options = {"view", "edit"};
        std::vector<std::string> result;
        for (const auto& opt : options) {
            if (opt.starts_with(prefix)) result.push_back(opt);
        }
        return result;
    }
};

} // namespace cc::commands
