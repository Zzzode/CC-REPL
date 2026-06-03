/// @file feedback.cppm
/// @brief FeedbackCommand implementing the /feedback slash command.
/// Submits feedback about Claude Code.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

export module cc.commands.feedback;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// FeedbackCommand implements the /feedback slash command.
/// Submits feedback about Claude Code.
class FeedbackCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "feedback",
            .description = "Submit feedback about Claude Code",
            .aliases = {"bug"},
            .args = {
                CommandArg{.name = "text", .description = "Feedback text",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "support",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            return CommandResult::success(
                "Please provide feedback text.\n"
                "Usage: /feedback <your feedback here>");
        }

        auto path = feedback_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                std::format("Failed to create feedback directory: {}", ec.message())));
        }

        std::ofstream out(path, std::ios::app);
        if (!out) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                std::format("Failed to open feedback log: {}", path.string())));
        }

        const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << timestamp << '\t';
        for (std::size_t i = 0; i < ctx.args.size(); ++i) {
            if (i > 0) out << ' ';
            out << ctx.args[i];
        }
        out << '\n';
        if (!out) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                std::format("Failed to write feedback log: {}", path.string())));
        }
        return CommandResult::success(
            "Thank you for your feedback! It has been recorded.");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }

private:
    [[nodiscard]] static std::filesystem::path feedback_path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            return std::filesystem::path{xdg} / "cc-repl" / "feedback.log";
        }
        if (const char* home = std::getenv("HOME"); home && *home) {
            return std::filesystem::path{home} / ".config" / "cc-repl" / "feedback.log";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "feedback.log";
    }
};

} // namespace cc::commands
