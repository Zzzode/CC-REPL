module;
#include <string>
#include <string_view>
export module cc.commands.backfill_sessions;
export namespace cc::commands::backfill_sessions {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "backfill_sessions"; }
[[nodiscard]] inline auto run(std::string_view scope = {}) -> CommandResponse { return {.ok = true, .message = "Session backfill command ready" + (scope.empty() ? std::string{} : ": " + std::string(scope))}; }
}
