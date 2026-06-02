module;
#include <string>
#include <string_view>
export module cc.commands.ant_trace;
export namespace cc::commands::ant_trace {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "ant_trace"; }
[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse { return {.ok = true, .message = "ANT trace command ready" + (target.empty() ? std::string{} : ": " + std::string(target))}; }
}
