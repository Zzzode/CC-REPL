/// @file ultrareview_quota.cppm
/// @brief Ultra review quota management
///
/// Ports src/services/api/ultrareviewQuota.ts. The TypeScript original GETs
/// `/v1/ultrareview/quota` on the OAuth base API URL with OAuth bearer +
/// organization headers, returning `{reviews_used, reviews_limit,
/// reviews_remaining, is_overage}`, or `null` when the user is not a
/// claude.ai subscriber or the endpoint errors. We mirror that exactly:
/// `fetch_ultrareview_quota()` performs the HTTP fetch (returning an honest
/// error on failure) and `can_use_ultrareview()` reads the result.
///
/// Backwards-compat: callers in commands/review/ultrareview.cppm read
/// `.remaining` / `.total`, so those field names are preserved alongside the
/// TS field names.
module;

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

export module cc.services.api.ultrareview_quota;

import cc.utils.http;
import cc.utils.json;
import cc.constants.oauth;
import cc.utils.teleport_utils;

export namespace cc::services::api {

// Mirrors UltrareviewQuotaResponse in ultrareviewQuota.ts.
// - reviews_used / reviews_limit / reviews_remaining / is_overage are the
//   exact TS field names.
// - remaining / total / is_unlimited are kept for the pre-existing callers in
//   commands/review/ultrareview.cppm (mapped from reviews_remaining /
//   reviews_limit).
struct UltrareviewQuota {
    std::uint32_t reviews_used{0};
    std::uint32_t reviews_limit{0};
    std::uint32_t reviews_remaining{0};
    bool is_overage{false};

    // Legacy aliases (do not break existing call sites).
    std::uint32_t remaining{0};
    std::uint32_t total{0};
    bool is_unlimited{false};
};

namespace detail {

// Local equivalent of bridge_enabled.cppm's detail::is_claude_ai_subscriber:
// the C++ migration intentionally avoids a remote GrowthBook dependency and
// treats a present OAuth token as the subscriber signal. Kept private to this
// module so the API surface stays self-contained.
[[nodiscard]] inline bool is_claude_ai_subscriber() {
    for (const char* name : {"CC_OAUTH_TOKEN", "CC_REPL_REMOTE_OAUTH_TOKEN",
                             "CLAUDE_CODE_OAUTH_TOKEN", "ANTHROPIC_OAUTH_TOKEN"}) {
        if (const char* token = std::getenv(name); token && token[0] != '\0') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::expected<UltrareviewQuota, std::string> parse_quota_json(
    std::string_view body) {
    auto parsed = cc::utils::json::parse(body);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("ultrareview quota response is not a JSON object");
    }
    auto root = parsed->root();

    UltrareviewQuota q;
    q.reviews_used = static_cast<std::uint32_t>(root.get("reviews_used").as_int());
    q.reviews_limit = static_cast<std::uint32_t>(root.get("reviews_limit").as_int());
    q.reviews_remaining =
        static_cast<std::uint32_t>(root.get("reviews_remaining").as_int());
    if (auto v = root.get("is_overage"); v.is_bool()) {
        q.is_overage = v.as_bool();
    }
    // Map to legacy fields consumed by commands/review/ultrareview.cppm.
    q.remaining = q.reviews_remaining;
    q.total = q.reviews_limit;
    return q;
}

} // namespace detail

/**
 * Peek the ultrareview quota for display and nudge decisions (TS
 * fetchUltrareviewQuota). Returns std::nullopt — matching the TS `null`
 * return — when the user is not a claude.ai subscriber. Returns an honest
 * error string when the endpoint or credentials are unavailable; returns
 * the parsed quota on success.
 */
[[nodiscard]] inline std::expected<std::optional<UltrareviewQuota>, std::string>
fetch_ultrareview_quota() {
    // TS: if (!isClaudeAISubscriber()) return null
    if (!detail::is_claude_ai_subscriber()) return std::nullopt;

    auto credentials = cc::utils::teleport::prepare_api_request();
    if (!credentials) return std::unexpected(credentials.error());

    const auto& oauth = cc::constants::oauth::prod_oauth_config;
    const auto url = std::format("{}/v1/ultrareview/quota", oauth.base_api_url);

    // Build headers exactly as the TS version does: OAuth bearer headers plus
    // the x-organization-uuid override.
    auto oauth_pairs = cc::utils::teleport::get_oauth_headers(credentials->access_token);
    std::unordered_map<std::string, std::string> headers(oauth_pairs.begin(),
                                                         oauth_pairs.end());
    headers["x-organization-uuid"] = credentials->org_uuid;

    cc::utils::HttpConfig http_config;
    http_config.timeout_ms = 5'000;
    http_config.max_retries = 0;
    cc::utils::HttpClient http{http_config};
    auto response = http.get(url, headers);
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(
            std::format("ultrareview quota request failed with HTTP {}", response->status));
    }

    return detail::parse_quota_json(response->body);
}

/**
 * Convenience wrapper preserving the original module entry point.
 *
 * Returns the parsed quota on success, a zeroed-out default when the user is
 * not a subscriber or the endpoint errors (mirrors the TS "null on failure"
 * behaviour at call sites that treat null as "no quota info").
 */
[[nodiscard]] inline UltrareviewQuota get_ultrareview_quota() {
    auto result = fetch_ultrareview_quota();
    if (!result || !*result) return UltrareviewQuota{};
    return **result;
}

[[nodiscard]] inline bool can_use_ultrareview() {
    return get_ultrareview_quota().remaining > 0;
}

} // namespace cc::services::api
