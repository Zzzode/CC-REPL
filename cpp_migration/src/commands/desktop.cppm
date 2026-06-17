/// @file desktop.cppm
/// @brief DesktopCommand implementing the /desktop slash command.
/// Opens the Claude Desktop download page for the current platform.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>

export module cc.commands.desktop;

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

/// Platform-appropriate Claude Desktop download URL.
[[nodiscard]] inline constexpr std::string_view desktop_download_url() {
#if defined(_WIN32)
    return "https://claude.ai/api/desktop/win32/x64/exe/latest/redirect";
#elif defined(__APPLE__)
    return "https://claude.ai/api/desktop/darwin/universal/dmg/latest/redirect";
#else
    return "https://clau.de/desktop";
#endif
}

} // namespace cc::commands

export namespace cc::commands {

using namespace cc::core;

class DesktopCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "desktop",
            .description = "Open the Claude Desktop download page",
            .args = {},
            .category = "integrations",
            .aliases = {"app"},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        auto url = desktop_download_url();
        open_in_browser(url);
        std::string out = "Claude Desktop:\n";
        out += std::format("  Opening: {}\n", std::string(url));
        out += "  Learn more: https://clau.de/desktop";
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
