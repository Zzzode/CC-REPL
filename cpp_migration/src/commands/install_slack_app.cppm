/// @file install_slack_app.cppm
/// @brief InstallSlackAppCommand implementing the /install-slack-app slash command.
module;

#include <cstdlib>
#include <string>
#include <vector>

export module cc.commands.install_slack_app;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

namespace install_slack_app_detail {

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

} // namespace install_slack_app_detail

class InstallSlackAppCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install-slack-app",
            .description = "Install the Claude Slack app",
            .args = {},
            .category = "integrations",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        constexpr std::string_view url = "https://slack.com/marketplace/A08SF47R6P4-claude";
        if (install_slack_app_detail::open_url(url)) {
            return CommandResult::success("Opening Slack app installation page in browser...");
        }
        return CommandResult::success("Could not open browser. Visit: https://slack.com/marketplace/A08SF47R6P4-claude");
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
