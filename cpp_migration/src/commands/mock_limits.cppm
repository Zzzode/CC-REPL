module;
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
export module cc.commands.mock_limits;

import cc.services.rate_limit.claude_ai_limits_hook;

export namespace cc::commands::mock_limits {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "mock_limits"; }

[[nodiscard]] inline int parse_retry_after(std::string_view text) {
    if (text.empty()) return 60;
    int seconds = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (ec != std::errc{} || ptr != text.data() + text.size() || seconds <= 0) return 60;
    return seconds;
}

[[nodiscard]] inline auto run(std::string_view mode = {}) -> CommandResponse {
    if (mode == "clear" || mode == "reset" || mode == "off") {
        cc::services::rate_limit::clear_rate_limit_state();
        return {.ok = true, .message = "Synthetic rate limit cleared"};
    }

    const int retry_after = parse_retry_after(mode);
    const bool retry = cc::services::rate_limit::handle_rate_limit_response(
        429, std::to_string(retry_after));
    const auto state = cc::services::rate_limit::check_rate_limit_state();
    return {.ok = true, .message = std::format(
        "Synthetic rate limit active: retry_after={}s, retry_allowed={}, total_retries={}",
        retry_after, retry ? "true" : "false", state.total_retries)};
}
}
