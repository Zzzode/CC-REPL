module;
#include <string>
#include <string_view>
export module cc.commands.version;

import cc.constants.product;

export namespace cc::commands::version {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "version"; }

[[nodiscard]] inline auto run(std::string_view detail = {}) -> CommandResponse {
    std::string msg = "cc-repl ";
    msg += cc::constants::product::CC_REPL_VERSION;
    msg += " (C++23, built ";
    msg += cc::constants::product::BUILD_DATE;
    msg += ")";
    if (!detail.empty()) {
        msg += " [";
        msg += detail;
        msg += "]";
    }
    return {.ok = true, .message = std::move(msg)};
}

} // namespace cc::commands::version
