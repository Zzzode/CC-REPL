module;
#include <string>
#include <string_view>
export module cc.commands.autofix_pr;
export namespace cc::commands::autofix_pr {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "autofix_pr"; }
[[nodiscard]] inline auto run(std::string_view pr = {}) -> CommandResponse { return {.ok = true, .message = "Autofix PR workflow prepared" + (pr.empty() ? std::string{} : ": " + std::string(pr))}; }
}
