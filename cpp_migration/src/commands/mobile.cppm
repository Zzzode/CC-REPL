/// @file mobile.cppm
/// @brief MobileCommand implementing the /mobile slash command.
/// Opens the Claude mobile app store listing, or prints both links when no
/// platform argument is supplied.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>

export module cc.commands.mobile;

import cc.types.types;
import cc.commands.command;
import cc.utils.exec_sync;

// Module-internal helpers (module linkage; intentionally not exported).
namespace cc::commands {

inline constexpr std::string_view kIosUrl =
    "https://apps.apple.com/app/claude-by-anthropic/id6473753684";
inline constexpr std::string_view kAndroidUrl =
    "https://play.google.com/store/apps/details?id=com.anthropic.claude";

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

class MobileCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "mobile",
            .description = "Show or open the Claude mobile app download links",
            .args = {CommandArg{.name = "platform", .description = "ios | android (omit for both)",
                                .type = ArgType::Choice, .required = false,
                                .choices = {"ios", "android"}}},
            .category = "integrations",
            .aliases = {"ios", "android"},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            const auto& a = ctx.args[0];
            if (a != "ios" && a != "android") {
                return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                    std::format("Usage: /mobile [ios|android] (got '{}')", a)));
            }
        }
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext& ctx) {
        std::string out = "Claude mobile app:\n";
        if (!ctx.args.empty() && ctx.args[0] == "ios") {
            open_in_browser(kIosUrl);
            out += std::format("  Opening iOS listing: {}", std::string(kIosUrl));
        } else if (!ctx.args.empty() && ctx.args[0] == "android") {
            open_in_browser(kAndroidUrl);
            out += std::format("  Opening Android listing: {}", std::string(kAndroidUrl));
        } else {
            out += std::format("  iOS: {}\n", std::string(kIosUrl));
            out += std::format("  Android: {}", std::string(kAndroidUrl));
        }
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> r;
        for (auto s : {"ios", "android"}) {
            if (std::string_view(s).starts_with(partial)) r.emplace_back(s);
        }
        return r;
    }
};

} // namespace cc::commands
