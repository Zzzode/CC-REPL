module;
#include <string>
#include <string_view>
export module cc.commands.mock_limits;
export namespace cc::commands::mock_limits {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "mock_limits"; }
[[nodiscard]] inline auto run(std::string_view mode = {}) -> CommandResponse { return {.ok = true, .message = "Synthetic limit command ready" + (mode.empty() ? std::string{} : ": " + std::string(mode))}; }
}
