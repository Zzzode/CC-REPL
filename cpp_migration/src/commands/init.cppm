/// @file init.cppm
/// @brief InitCommand implementing the /init slash command.
/// Injects a codebase-analysis prompt that drives CLAUDE.md generation,
/// mirroring the default prompt of the TypeScript /init command.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>

export module cc.commands.init;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Codebase-analysis prompt injected into the conversation so the assistant
/// generates (or updates) a CLAUDE.md for the current repository. Mirrors the
/// default prompt used by the TypeScript /init command.
inline const std::string kInitPrompt = R"INITPROMPT(Please analyze this codebase and create a CLAUDE.md file, which will be given to future instances of Claude Code to operate in this repository.

What to add:
1. Commands that will be commonly used, such as how to build, lint, and run tests. Include the necessary commands to develop in this codebase, such as how to run a single test.
2. High-level code architecture and structure so that future instances can be productive more quickly. Focus on the "big picture" architecture that requires reading multiple files to understand.

Usage notes:
- If there's already a CLAUDE.md, suggest improvements to it.
- When you make the initial CLAUDE.md, do not repeat yourself and do not include obvious instructions like "Provide helpful error messages to users", "Write unit tests for all utilities", "Never include sensitive information (API keys, tokens) in code or commits".
- Avoid listing every component or file structure that can be easily discovered.
- Don't include generic development practices.
- If there are Cursor rules (in .cursor/rules/ or .cursorrules) or Copilot rules (in .github/copilot-instructions.md), make sure to include the important parts.
- If there is a README.md, make sure to include the important parts.
- Do not make up information such as "Common Development Tasks", "Tips for Development", "Support and Documentation" unless this is expressly included in other files that you read.
- Be sure to prefix the file with the following text:

```
# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
```
)INITPROMPT";

/// InitCommand implements the /init slash command.
/// Injects a codebase-analysis prompt that drives CLAUDE.md generation.
class InitCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "init",
            .description = "Initialize a CLAUDE.md file with codebase documentation",
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
        return CommandResult::inject(kInitPrompt);
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
