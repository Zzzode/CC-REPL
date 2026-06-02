module;
#include <string>
#include <string_view>
export module cc.commands.extra_usage;
export namespace cc::commands::extra_usage {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "extra_usage"; }
[[nodiscard]] inline auto run(std::string_view window = {}) -> CommandResponse { return {.ok = true, .message = "Extra usage command ready" + (window.empty() ? std::string{} : ": " + std::string(window))}; }
}
