module;
#include <string>
#include <string_view>
export module cc.commands.reset_limits;
export namespace cc::commands::reset_limits {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "reset_limits"; }
[[nodiscard]] inline auto run(std::string_view scope = {}) -> CommandResponse { return {.ok = true, .message = "Reset limits command ready" + (scope.empty() ? std::string{} : ": " + std::string(scope))}; }
}
