/// @file upgrade.cppm
/// @brief UpgradeCommand implementing the /upgrade slash command.
/// Reports the current version and opens the Loom upgrade page. A full OAuth
/// subscription check requires the backend; this opens the upgrade flow in the
/// browser so the user can complete it there.
module;


export module cc.commands.upgrade;

import std;

import cc.types.types;
import cc.commands.command;
import cc.constants.product;
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

/// UpgradeCommand implements the /upgrade slash command.
class UpgradeCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "upgrade",
            .description = "Upgrade your Loom plan to Max",
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
        // No upgrade backend is reachable from this build.
        std::string out = "Loom upgrade:\n";
        out += std::format("  Current version: {}\n", cc::constants::product::LOOM_VERSION);
        out += "  No upgrade backend is configured for this build.\n\n";
        out += "Complete the upgrade in your browser, then re-authenticate with /login.";
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
