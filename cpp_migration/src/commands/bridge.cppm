module;
#include <string>
#include <string_view>
export module cc.commands.bridge;
export namespace cc::commands::bridge {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "bridge"; }
[[nodiscard]] inline auto run(std::string_view action = {}) -> CommandResponse { return {.ok = true, .message = "Bridge command ready" + (action.empty() ? std::string{} : ": " + std::string(action))}; }
}
