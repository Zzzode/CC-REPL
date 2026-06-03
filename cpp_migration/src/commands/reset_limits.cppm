module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.reset_limits;

import cc.services.rate_limit.claude_ai_limits_hook;

export namespace cc::commands::reset_limits {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "reset_limits"; }

[[nodiscard]] inline auto run(std::string_view scope = {}) -> CommandResponse {
    if (!scope.empty() && scope != "rate-limit" && scope != "all") {
        return {.ok = false, .message = "reset-limits supports: rate-limit, all"};
    }
    cc::services::rate_limit::clear_rate_limit_state();
    const auto state = cc::services::rate_limit::check_rate_limit_state();
    return {.ok = true, .message = std::format(
        "Rate limit state reset: active={}, total_retries={}",
        state.is_rate_limited ? "true" : "false", state.total_retries)};
}
}
