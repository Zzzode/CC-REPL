module;
#include <string>
#include <string_view>
export module cc.commands.bughunter;
export namespace cc::commands::bughunter {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "bughunter"; }
[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse { return {.ok = true, .message = "Bughunter command ready" + (target.empty() ? std::string{} : ": " + std::string(target))}; }
}
