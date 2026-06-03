/// @file stickers.cppm
/// @brief StickersCommand implementing the /stickers slash command.
module;

#include <cstdlib>
#include <string>
#include <vector>

export module cc.commands.stickers;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

namespace stickers_detail {

[[nodiscard]] bool open_url(std::string_view url) {
#if defined(_WIN32)
    std::string command = "start \"\" \"" + std::string(url) + "\"";
#elif defined(__APPLE__)
    std::string command = "open \"" + std::string(url) + "\"";
#else
    std::string command = "xdg-open \"" + std::string(url) + "\"";
#endif
    return std::system(command.c_str()) == 0;
}

} // namespace stickers_detail

class StickersCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "stickers",
            .description = "Order Claude Code stickers",
            .args = {},
            .category = "fun",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        constexpr std::string_view url = "https://www.stickermule.com/claudecode";
        if (stickers_detail::open_url(url)) {
            return CommandResult::success("Opening sticker page in browser...");
        }
        return CommandResult::success("Could not open browser. Visit: https://www.stickermule.com/claudecode");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
