/// @file btw.cppm
/// @brief BtwCommand implementing the /btw slash command.
/// Asks a quick side question without interrupting the main conversation.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <span>

export module cc.commands.btw;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

struct BtwOptions {
    std::optional<std::string> question;
};

class BtwCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "btw",
            .description = "Ask a quick side question without interrupting the main conversation",
            .args = {
                CommandArg{.name = "<question>", .description = "Your question", .type = ArgType::Text, .required = true},
            },
            .category = "conversation",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        if (!opts.question || opts.question->empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Please provide a question: /btw <your question>"
            ));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        if (!opts.question) {
            return CommandResult::fail("Usage: /btw <your question>");
        }
        
        return CommandResult::inject(std::format("Side question: {}", *opts.question));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }

private:
    [[nodiscard]] static BtwOptions parse_options(std::span<const std::string> args) {
        BtwOptions opts;
        if (!args.empty()) {
            // Join all args into a single question
            std::string question;
            for (const auto& arg : args) {
                if (!question.empty()) question += " ";
                question += arg;
            }
            opts.question = std::move(question);
        }
        return opts;
    }
};

} // namespace cc::commands
