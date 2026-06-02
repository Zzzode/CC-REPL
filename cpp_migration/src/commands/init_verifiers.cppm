module;
#include <string>
#include <string_view>
export module cc.commands.init_verifiers;
export namespace cc::commands::init_verifiers {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "init_verifiers"; }
[[nodiscard]] inline auto run(std::string_view profile = {}) -> CommandResponse { return {.ok = true, .message = "Verifier initialization command ready" + (profile.empty() ? std::string{} : ": " + std::string(profile))}; }
}
