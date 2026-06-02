module;
#include <string>
#include <string_view>
export module cc.commands.debug_tool_call;
export namespace cc::commands::debug_tool_call {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "debug_tool_call"; }
[[nodiscard]] inline auto run(std::string_view tool = {}) -> CommandResponse { return {.ok = true, .message = "Tool-call debugger ready" + (tool.empty() ? std::string{} : ": " + std::string(tool))}; }
}
