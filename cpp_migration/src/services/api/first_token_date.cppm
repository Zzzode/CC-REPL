/// @file first_token_date.cppm
/// @brief Track first token usage date for billing
///
/// Ports src/services/api/firstTokenDate.ts. The TypeScript original GETs
/// `/api/organization/claude_code_first_token_date` on the OAuth base API URL,
/// reads `response.data.first_token_date` (string | null), validates it parses
/// as a date, and stores it in the global config under `claudeCodeFirstTokenDate`.
///
/// The C++ port keeps the HTTP fetch + parse + validation logic identical. The
/// "store in config" step is exposed via an injectable sink (a callable taking
/// the validated timestamp string, or std::nullopt for null) so callers and
/// tests can route it to the real global config writer without this module
/// having to depend on the entire config stack. A default no-op sink preserves
/// the previous behaviour when no sink is supplied.
module;

#include <chrono>
#include <ctime>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

export module cc.services.api.first_token_date;

import cc.utils.http;
import cc.utils.json;
import cc.constants.oauth;
import cc.utils.teleport_utils;
import cc.utils.user_agent; // get_user_agent()

export namespace cc::services::api {

using Clock = std::chrono::system_clock;

/// Sink invoked with the validated first-token date (ISO string) or
/// std::nullopt when the API returned null. Default is a no-op so callers
/// that only want the parse result are not forced to wire config I/O.
using StoreSink = std::function<void(const std::optional<std::string>&)>;

namespace detail {

/**
 * Validate that `value` parses as a real calendar date (mirrors
 * `new Date(value).getTime()` + isNaN check in TS). Returns false for empty
 * strings.
 */
[[nodiscard]] inline bool is_valid_iso_date(std::string_view value) {
    if (value.empty()) return false;
    // Accept ISO-8601 (e.g. "2024-01-15T00:00:00Z" or "2024-01-15").
    std::tm tm{};
    std::string copy(value);
    auto* end = strptime(copy.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) {
        end = strptime(copy.c_str(), "%Y-%m-%d", &tm);
    }
    if (!end) return false;
    // strptime does not touch tm_wday / tm_yday reliably; a valid parse is
    // enough signal that the date is well-formed.
    return true;
}

[[nodiscard]] inline std::string base_api_url() {
    return std::string(cc::constants::oauth::prod_oauth_config.base_api_url);
}

} // namespace detail

/**
 * Fetch the user's first Claude Code token date (TS
 * fetchAndStoreClaudeCodeFirstTokenDate). Performs the HTTP GET, validates the
 * returned date, and forwards the result to `store_sink` (matching the TS
 * saveGlobalConfig step). Returns an error string on transport/HTTP failure
 * so callers see honest failures instead of silent no-ops.
 *
 * The TS guard `if (config.claudeCodeFirstTokenDate !== undefined) return`
 * (skip if already cached) is left to the caller — pass a `store_sink` whose
 * side effect checks the existing config value.
 */
[[nodiscard]] inline std::expected<std::optional<std::string>, std::string>
fetch_first_token_date(const StoreSink& store_sink = {}) {
    auto credentials = cc::utils::teleport::prepare_api_request();
    if (!credentials) return std::unexpected(credentials.error());

    const auto url =
        std::format("{}/api/organization/claude_code_first_token_date",
                    detail::base_api_url());

    auto oauth_pairs = cc::utils::teleport::get_oauth_headers(credentials->access_token);
    std::unordered_map<std::string, std::string> headers(oauth_pairs.begin(),
                                                         oauth_pairs.end());

    // TS sets User-Agent: getClaudeCodeUserAgent(); use the shared helper.
    headers["User-Agent"] = cc::utils::get_user_agent();

    cc::utils::HttpClient http{cc::utils::HttpConfig{.timeout_ms = 10'000, .max_retries = 0}};
    auto response = http.get(url, headers);
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(
            std::format("first_token_date request failed with HTTP {}", response->status));
    }

    // TS: const firstTokenDate = response.data?.first_token_date ?? null
    std::optional<std::string> first_token_date;
    if (auto parsed = cc::utils::json::parse(response->body); parsed && parsed->root().is_obj()) {
        auto node = parsed->root().get("first_token_date");
        if (node.is_str()) {
            first_token_date = std::string(node.as_str());
        } else if (node.is_null() || !node.valid()) {
            first_token_date = std::nullopt;
        }
    }

    // TS: validate the date if non-null; reject invalid dates (do not save).
    if (first_token_date && !detail::is_valid_iso_date(*first_token_date)) {
        return std::unexpected(
            std::format("Received invalid first_token_date from API: {}", *first_token_date));
    }

    if (store_sink) store_sink(first_token_date);
    return first_token_date;
}

/**
 * Convenience setter preserving the original module entry point signature.
 * Stores the given time_point into the supplied sink (no-op by default).
 */
inline void set_first_token_date(Clock::time_point tp, const StoreSink& store_sink = {}) {
    if (!store_sink) return;
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    store_sink(std::optional<std::string>(std::string(buf)));
}

} // namespace cc::services::api
