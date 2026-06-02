// OutputStyle command - deprecated, redirects to /config
module;
#include <string>
#include <string_view>
export module cc.commands.output_style;
export namespace cc::commands::output_style {

struct CommandResponse { bool ok{true}; std::string message; };

[[nodiscard]] inline auto name() -> std::string_view { return "output-style"; }

[[nodiscard]] inline auto run(std::string_view style = {}) -> CommandResponse {
    (void)style;
    return {
        .ok = true,
        .message = "The /output-style command is deprecated.\n"
                   "Use `/config` to manage output styles instead.\n\n"
                   "Example: /config set outputStyle verbose"
    };
}

}
