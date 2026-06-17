/// @file chrome.cppm
/// @brief ChromeCommand implementing the /chrome slash command.
/// Opens the Claude in Chrome setup page and lists reconnect/permissions links.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>

export module cc.commands.chrome;

import cc.types.types;
import cc.commands.command;
import cc.utils.exec_sync;

// Module-internal helpers (module linkage; intentionally not exported).
namespace cc::commands {

inline void open_in_browser(std::string_view url) {
#if defined(__APPLE__)
    cc::utils::exec_sync_status("open " + std::string(url));
#elif defined(__linux__)
    cc::utils::exec_sync_status("xdg-open " + std::string(url));
#else
    (void)url;
#endif
}

} // namespace cc::commands

export namespace cc::commands {

using namespace cc::core;

class ChromeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "chrome",
            .description = "Open Claude in Chrome setup",
            .args = {},
            .category = "integrations",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        open_in_browser("https://claude.ai/chrome");
        return CommandResult::success(
            "Claude in Chrome setup:\n"
            "  Install:        https://claude.ai/chrome\n"
            "  Reconnect:      https://clau.de/chrome/reconnect\n"
            "  Permissions:    https://clau.de/chrome/permissions\n"
            "  CLI flags:      --chrome or --no-chrome\n"
            "\n"
            "Opening the install page in your default browser.");
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
