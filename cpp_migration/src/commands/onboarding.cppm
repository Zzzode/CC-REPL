module;
#include <string>
#include <string_view>
export module cc.commands.onboarding;
export namespace cc::commands::onboarding {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "onboarding"; }
[[nodiscard]] inline auto run(std::string_view step = {}) -> CommandResponse { return {.ok = true, .message = "Onboarding command ready" + (step.empty() ? std::string{} : ": " + std::string(step))}; }
}
