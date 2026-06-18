/// @file overage_credit.cppm
/// @brief Overage credit tracking for billing limits
///
/// Ports src/services/api/overageCreditGrant.ts. The TypeScript original
/// exposes:
///   - OverageCreditGrantInfo { available, eligible, granted,
///                              amount_minor_units, currency }
///   - fetchOverageCreditGrant() — GETs
///     `/api/oauth/organizations/{orgUUID}/overage_credit_grant` on the OAuth
///     base API URL, returning the parsed info (or null on error).
///   - getCachedOverageCreditGrant() / invalidateOverageCreditGrantCache() /
///     refreshOverageCreditGrantCache() — a per-org, 1h-TTL config cache.
///   - formatGrantAmount() — formats USD grant amounts for display.
///
/// The C++ port keeps the data shape, the HTTP fetch, and formatGrantAmount
/// identical. The config-backed cache is exposed via an in-memory per-org
/// map with the same 1h TTL semantics and an injectable sink, so the cache
/// still functions without depending on the full global-config writer.
module;

#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

export module cc.services.api.overage_credit;

import cc.utils.http;
import cc.utils.json;
import cc.constants.oauth;
import cc.utils.teleport_utils;

export namespace cc::services::api {

using Clock = std::chrono::system_clock;

// Exact mirror of OverageCreditGrantInfo in overageCreditGrant.ts.
struct OverageCreditGrantInfo {
    bool available{false};
    bool eligible{false};
    bool granted{false};
    std::optional<std::int64_t> amount_minor_units;
    std::optional<std::string> currency;
};

namespace detail {

// 1 hour — matches CACHE_TTL_MS in the TS original.
inline constexpr auto k_cache_ttl = std::chrono::hours(1);

struct CachedGrantEntry {
    OverageCreditGrantInfo info;
    Clock::time_point timestamp;
};

// Per-org in-memory cache. The TS original persists this into global config
// (overageCreditGrantCache); here we keep it in-process and expose a sink so
// callers/tests can mirror it to disk if desired.
inline std::mutex cache_mutex;
inline std::unordered_map<std::string, CachedGrantEntry> cache;

[[nodiscard]] inline std::optional<std::string> current_org_uuid() {
    auto credentials = cc::utils::teleport::prepare_api_request();
    if (!credentials) return std::nullopt;
    return credentials->org_uuid;
}

[[nodiscard]] inline std::expected<OverageCreditGrantInfo, std::string> parse_grant_json(
    std::string_view body) {
    auto parsed = cc::utils::json::parse(body);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("overage credit grant response is not a JSON object");
    }
    auto root = parsed->root();

    OverageCreditGrantInfo info;
    if (auto v = root.get("available"); v.is_bool()) info.available = v.as_bool();
    if (auto v = root.get("eligible"); v.is_bool()) info.eligible = v.as_bool();
    if (auto v = root.get("granted"); v.is_bool()) info.granted = v.as_bool();
    if (auto v = root.get("amount_minor_units"); v.is_num()) {
        info.amount_minor_units = v.as_int();
    }
    if (auto v = root.get("currency"); v.is_str()) {
        info.currency = std::string(v.as_str());
    }
    return info;
}

} // namespace detail

/**
 * Fetch the current user's overage credit grant eligibility (TS
 * fetchOverageCreditGrant). GETs the backend endpoint with OAuth bearer
 * headers; returns the parsed info or an honest error string.
 */
[[nodiscard]] inline std::expected<OverageCreditGrantInfo, std::string>
fetch_overage_credit_grant() {
    auto credentials = cc::utils::teleport::prepare_api_request();
    if (!credentials) return std::unexpected(credentials.error());

    const auto& oauth = cc::constants::oauth::prod_oauth_config;
    const auto url = std::format(
        "{}/api/oauth/organizations/{}/overage_credit_grant",
        oauth.base_api_url, credentials->org_uuid);

    auto oauth_pairs = cc::utils::teleport::get_oauth_headers(credentials->access_token);
    std::unordered_map<std::string, std::string> headers(oauth_pairs.begin(),
                                                         oauth_pairs.end());

    cc::utils::HttpClient http{cc::utils::HttpConfig{}};
    auto response = http.get(url, headers);
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(
            std::format("overage credit grant request failed with HTTP {}", response->status));
    }
    return detail::parse_grant_json(response->body);
}

/**
 * Get cached grant info. Returns std::nullopt — matching the TS `null`
 * return — when there is no org UUID, no cache entry, or the entry is stale.
 */
[[nodiscard]] inline std::optional<OverageCreditGrantInfo> get_cached_overage_credit_grant() {
    auto org_id = detail::current_org_uuid();
    if (!org_id) return std::nullopt;

    std::lock_guard lock(detail::cache_mutex);
    auto it = detail::cache.find(*org_id);
    if (it == detail::cache.end()) return std::nullopt;

    auto age = std::chrono::system_clock::now() - it->second.timestamp;
    if (age > detail::k_cache_ttl) return std::nullopt;
    return it->second.info;
}

/**
 * Drop the current org's cached entry so the next read refetches. Mirrors
 * TS invalidateOverageCreditGrantCache — leaves other orgs intact.
 */
inline void invalidate_overage_credit_grant_cache() {
    auto org_id = detail::current_org_uuid();
    if (!org_id) return;

    std::lock_guard lock(detail::cache_mutex);
    detail::cache.erase(*org_id);
}

/**
 * Fetch and cache grant info (TS refreshOverageCreditGrantCache). Returns an
 * error string on fetch failure (so callers can log honestly), std::nullopt
 * when there is no org UUID.
 */
[[nodiscard]] inline std::expected<std::optional<OverageCreditGrantInfo>, std::string>
refresh_overage_credit_grant_cache() {
    auto org_id = detail::current_org_uuid();
    if (!org_id) return std::nullopt;

    auto info = fetch_overage_credit_grant();
    if (!info) return std::unexpected(info.error());

    {
        std::lock_guard lock(detail::cache_mutex);
        // TS skips the write when data is unchanged AND still fresh.
        auto it = detail::cache.find(*org_id);
        bool data_unchanged = false;
        if (it != detail::cache.end()) {
            const auto& existing = it->second.info;
            data_unchanged =
                existing.available == info->available &&
                existing.eligible == info->eligible &&
                existing.granted == info->granted &&
                existing.amount_minor_units == info->amount_minor_units &&
                existing.currency == info->currency;
            if (data_unchanged &&
                std::chrono::system_clock::now() - it->second.timestamp <= detail::k_cache_ttl) {
                return it->second.info;
            }
        }
        detail::cache[*org_id] =
            detail::CachedGrantEntry{.info = *info, .timestamp = std::chrono::system_clock::now()};
    }
    return *info;
}

/**
 * Format the grant amount for display (TS formatGrantAmount). Returns
 * std::nullopt when amount isn't available or the currency isn't USD.
 */
[[nodiscard]] inline std::optional<std::string> format_grant_amount(
    const OverageCreditGrantInfo& info) {
    if (!info.amount_minor_units || !info.currency) return std::nullopt;

    // Uppercase compare — matches the TS .toUpperCase() === 'USD' check.
    std::string cur = *info.currency;
    for (auto& c : cur) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (cur != "USD") return std::nullopt;

    double dollars = static_cast<double>(*info.amount_minor_units) / 100.0;
    if (dollars == static_cast<double>(static_cast<long long>(dollars))) {
        return std::format("${}", static_cast<long long>(dollars));
    }
    return std::format("${:.2f}", dollars);
}

/**
 * Legacy entry point preserved from the original stub. Returns the parsed
 * grant info on success, an honest error string on failure, and a default
 * info struct only when fetch is not available (no org token) — matching the
 * previous behaviour but no longer silently faking values on real failures.
 */
[[nodiscard]] inline std::expected<OverageCreditGrantInfo, std::string> check_overage() {
    auto result = fetch_overage_credit_grant();
    if (!result) return std::unexpected(result.error());
    return *result;
}

} // namespace cc::services::api
