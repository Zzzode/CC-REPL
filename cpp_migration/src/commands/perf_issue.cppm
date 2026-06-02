module;
#include <string>
#include <string_view>
export module cc.commands.perf_issue;
export namespace cc::commands::perf_issue {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "perf_issue"; }
[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse { return {.ok = true, .message = "Performance issue command ready" + (target.empty() ? std::string{} : ": " + std::string(target))}; }
}
