/// @file overage_credit.cppm
/// @brief Overage credit grant tracking for billing limits.
/// Port of src/services/api/overageCreditGrant.ts.
/// Checks eligibility for overage credit grants and caches the result.
module;
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

export module cc.services.api.overage_credit;

import cc.utils.json;
import cc.services.api.client;

export namespace cc::services::api {

/// Overage credit grant information returned from the API.
struct OverageCreditGrantInfo {
    bool available{false};
    bool eligible{false};
    bool granted{false};
    std::optional<int64_t> amount_minor_units;
    std::optional<std::string> currency;
};

/// Convenience struct used by the rest of the codebase.
struct OverageInfo {
    double credit_remaining{0};
    double overage_amount{0};
    bool is_over_limit{false};
};

namespace detail {

/// Cache entry with timestamp for TTL-based expiration.
struct CachedGrantEntry {
    OverageCreditGrantInfo info;
    std::chrono::steady_clock::time_point cached_at;
};

inline constexpr auto CACHE_TTL_MS = std::chrono::milliseconds{3600000}; // 1 hour

/// Path to the overage credit cache file.
[[nodiscard]] inline std::filesystem::path cache_file_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "cc-repl" / "overage_credit_cache.json";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "cc-repl" / "overage_credit_cache.json";
    }
    return std::filesystem::temp_directory_path() / "cc-repl" / "overage_credit_cache.json";
}

/// Parse a grant info from a JSON value.
[[nodiscard]] inline std::optional<OverageCreditGrantInfo> parse_grant_info(cc::utils::json::JsonVal node) {
    if (!node.is_obj()) return std::nullopt;
    OverageCreditGrantInfo info;
    auto avail = node.get("available");
    if (avail.is_bool()) info.available = avail.as_bool();
    auto elig = node.get("eligible");
    if (elig.is_bool()) info.eligible = elig.as_bool();
    auto grant = node.get("granted");
    if (grant.is_bool()) info.granted = grant.as_bool();
    auto amount = node.get("amount_minor_units");
    if (amount.is_num()) info.amount_minor_units = amount.as_int();
    auto curr = node.get("currency");
    if (curr.is_str()) info.currency = std::string(curr.as_str());
    return info;
}

/// Serialize grant info to a JSON string.
[[nodiscard]] inline std::string serialize_grant_info(const OverageCreditGrantInfo& info) {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("available", doc.boolean(info.available));
    root.add("eligible", doc.boolean(info.eligible));
    root.add("granted", doc.boolean(info.granted));
    if (info.amount_minor_units) {
        root.add("amount_minor_units", doc.number(*info.amount_minor_units));
    }
    if (info.currency) {
        root.add("currency", doc.string(*info.currency));
    }
    root.add("cached_at_epoch_ms", doc.number(
        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count())));
    doc.set_root(root);
    return doc.to_string();
}

/// Write cache to disk.
inline void write_cache(const std::string& org_id, const OverageCreditGrantInfo& info) {
    auto path = cache_file_path();

    // Read existing cache (may contain entries for other orgs)
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();

    if (std::filesystem::exists(path)) {
        auto existing = cc::utils::json::parse_file(path);
        if (existing && existing->root().is_obj()) {
            // Preserve other org entries — re-export existing keys
            existing->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal val) {
                auto key_str = key.as_str();
                if (key_str != org_id) {
                    root.add(std::string(key_str), doc.raw_json(val.to_string()));
                }
            });
        }
    }

    // Add/update this org's entry
    auto grant_obj = doc.raw_json(serialize_grant_info(info));
    root.add(org_id, grant_obj);
    doc.set_root(root);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;

    std::ofstream file(path, std::ios::trunc);
    if (file.is_open()) {
        file << doc.to_string();
    }
}

} // namespace detail

/// Check cached overage credit grant status for the given organization.
/// Returns nullopt if no cache or cache is stale (>1 hour old).
[[nodiscard]] inline std::optional<OverageCreditGrantInfo> get_cached_overage_credit(
    const std::string& org_id) {
    if (org_id.empty()) return std::nullopt;

    auto path = detail::cache_file_path();
    if (!std::filesystem::exists(path)) return std::nullopt;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;

    auto entry = parsed->root().get(org_id);
    if (!entry.is_obj()) return std::nullopt;

    // Check TTL
    auto cached_ms = entry.get("cached_at_epoch_ms");
    if (cached_ms.is_num()) {
        auto cached_epoch = std::chrono::milliseconds{cached_ms.as_int()};
        auto cached_at = std::chrono::steady_clock::time_point{cached_epoch};
        auto age = std::chrono::steady_clock::now() - cached_at;
        if (age > detail::CACHE_TTL_MS) return std::nullopt;
    } else {
        return std::nullopt; // No timestamp — treat as stale
    }

    return detail::parse_grant_info(entry);
}

/// Fetch overage credit grant from the API and update cache.
/// Returns the grant info, or an error string on failure.
[[nodiscard]] inline std::expected<OverageCreditGrantInfo, std::string> fetch_overage_credit(
    const std::string& org_id,
    const std::string& access_token) {
    if (org_id.empty() || access_token.empty()) {
        return std::unexpected("Missing org_id or access_token");
    }

    std::string base_url = "https://api.anthropic.com";
    if (auto* env = std::getenv("ANTHROPIC_BASE_URL"); env && *env) base_url = env;

    auto url = std::format("{}/api/oauth/organizations/{}/overage_credit_grant", base_url, org_id);

    std::vector<std::string> headers = {
        std::format("Authorization: Bearer {}", access_token),
        "Content-Type: application/json"
    };

    auto result = HttpClient::get(url, headers, std::chrono::milliseconds{10000});
    if (!result) {
        return std::unexpected("Network error fetching overage credit grant");
    }
    if (result->status_code != 200) {
        return std::unexpected(std::format("Overage credit API returned status {}", result->status_code));
    }

    auto doc = cc::utils::json::parse(result->body);
    if (!doc) {
        return std::unexpected("Failed to parse overage credit response");
    }

    auto info = detail::parse_grant_info(doc->root());
    if (!info) {
        return std::unexpected("Invalid overage credit grant response shape");
    }

    // Persist to cache
    detail::write_cache(org_id, *info);

    return *info;
}

/// Invalidate cached overage credit for the given org.
inline void invalidate_overage_credit_cache(const std::string& org_id) {
    if (org_id.empty()) return;
    auto path = detail::cache_file_path();
    if (!std::filesystem::exists(path)) return;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return;

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal val) {
        auto key_str = key.as_str();
        if (key_str != org_id) {
            root.add(std::string(key_str), doc.raw_json(val.to_string()));
        }
    });
    doc.set_root(root);

    std::ofstream file(path, std::ios::trunc);
    if (file.is_open()) file << doc.to_string();
}

/// Format grant amount from minor units to human-readable string.
/// Returns nullopt if amount or currency is not set.
[[nodiscard]] inline std::optional<std::string> format_grant_amount(const OverageCreditGrantInfo& info) {
    if (!info.amount_minor_units || !info.currency) return std::nullopt;
    const auto& currency = *info.currency;
    const auto amount = *info.amount_minor_units;

    if (currency == "USD") {
        double dollars = static_cast<double>(amount) / 100.0;
        if (dollars == static_cast<int64_t>(dollars)) {
            return std::format("${}", static_cast<int64_t>(dollars));
        }
        return std::format("${:.2f}", dollars);
    }
    // Generic fallback
    return std::format("{} {}", amount, currency);
}

/// Legacy convenience: check overage status.
/// Returns a simplified OverageInfo derived from the grant info.
[[nodiscard]] inline std::expected<OverageInfo, std::string> check_overage() {
    // Try to get org_id from environment
    std::string org_id;
    if (auto* env = std::getenv("CLAUDE_ORGANIZATION_UUID"); env && *env) org_id = env;
    if (org_id.empty()) return OverageInfo{};

    auto cached = get_cached_overage_credit(org_id);
    if (!cached) {
        return OverageInfo{};
    }

    OverageInfo info;
    if (cached->eligible && cached->granted && cached->amount_minor_units) {
        info.credit_remaining = static_cast<double>(*cached->amount_minor_units) / 100.0;
    }
    info.is_over_limit = cached->eligible && !cached->granted;
    return info;
}

} // namespace cc::services::api
