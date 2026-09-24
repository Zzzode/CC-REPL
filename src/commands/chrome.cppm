/// @file chrome.cppm
/// @brief ChromeCommand implementing the /chrome slash command.
/// Opens the Loom in Chrome setup page and lists reconnect/permissions links.
module;


export module cc.commands.chrome;

import std;

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
            .description = "Open Loom in Chrome setup",
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
        // No browser-extension backend is reachable from this build.
        return CommandResult::success(
            "Loom in Chrome setup:\n"
            "  Install:        (no browser extension is published for this build)\n"
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
