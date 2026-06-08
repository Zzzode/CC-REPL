/// @file first_token_date.cppm
/// @brief Track first token usage date for billing.
/// Port of src/services/api/firstTokenDate.ts.
/// Stores the date persistently in global config; fetches once from the API.
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
#include <string_view>

export module cc.services.api.first_token_date;

import cc.utils.json;
import cc.services.api.client;

export namespace cc::services::api {

using Clock = std::chrono::system_clock;

namespace detail {

/// Return the path to the persistent first-token-date marker file.
[[nodiscard]] inline std::filesystem::path state_file_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "cc-repl" / "first_token_date.json";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "cc-repl" / "first_token_date.json";
    }
    return std::filesystem::temp_directory_path() / "cc-repl" / "first_token_date.json";
}

/// Parse an ISO-8601 date string (e.g. "2025-01-15") to a time_point.
/// Manual parse to avoid dependency on std::chrono::parse (not in libc++18).
[[nodiscard]] inline std::optional<Clock::time_point> parse_iso_date(const std::string& iso) {
    if (iso.size() < 10) return std::nullopt;
    // Expect YYYY-MM-DD
    if (iso[4] != '-' || iso[7] != '-') return std::nullopt;
    try {
        int year = std::stoi(iso.substr(0, 4));
        int month = std::stoi(iso.substr(5, 2));
        int day = std::stoi(iso.substr(8, 2));
        if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
        // Convert to days since epoch (simple calculation)
        // Using std::chrono::year_month_day if available, else manual
        int total_days = 0;
        // Days from 1970 to start of given year
        auto y = static_cast<long long>(year);
        auto m = static_cast<long long>(month);
        auto d = static_cast<long long>(day);
        // Days from epoch: using the civil_from_days approach
        // Adjusted so that month <= 2 shifts year back
        if (m <= 2) { y--; m += 12; }
        total_days = static_cast<int>(365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 719469);
        return Clock::time_point(std::chrono::days(total_days));
    } catch (...) {
        return std::nullopt;
    }
}

/// Serialize a time_point to an ISO-8601 date string.
/// Manual conversion to avoid std::chrono::year_month_day limitations.
[[nodiscard]] inline std::string format_iso_date(Clock::time_point tp) {
    auto days_since_epoch = std::chrono::duration_cast<std::chrono::days>(
        tp.time_since_epoch()).count();
    // Convert days since epoch to year/month/day (civil date)
    auto z = days_since_epoch + 719468;
    auto era = (z >= 0 ? z : z - 146096) / 146097;
    auto doe = z - era * 146097;
    auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    auto y = yoe + era * 400;
    auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    auto mp = (5 * doy + 2) / 153;
    auto d = doy - (153 * mp + 2) / 5 + 1;
    auto m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    return std::format("{}-{:02d}-{:02d}", static_cast<int>(y), static_cast<int>(m), static_cast<int>(d));
}

} // namespace detail

/// Get the first token date from persistent storage.
/// Returns nullopt if never recorded.
[[nodiscard]] inline std::optional<Clock::time_point> get_first_token_date() {
    auto path = detail::state_file_path();
    if (!std::filesystem::exists(path)) return std::nullopt;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed) return std::nullopt;

    auto root = parsed->root();
    auto date_str = root.get("first_token_date");
    if (!date_str.is_str()) return std::nullopt;

    return detail::parse_iso_date(std::string(date_str.as_str()));
}

/// Record the first token date. Only writes if not already stored.
inline void set_first_token_date(Clock::time_point tp) {
    // Idempotent: if already stored, skip the write
    if (get_first_token_date().has_value()) return;

    auto path = detail::state_file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("first_token_date", doc.string(detail::format_iso_date(tp)));
    doc.set_root(root);

    std::ofstream file(path, std::ios::trunc);
    if (file.is_open()) {
        file << doc.to_pretty_string();
    }
}

/// Fetch first token date from the API and persist it.
/// This is a one-time operation: if already stored, it returns immediately.
/// Port of TS fetchAndStoreClaudeCodeFirstTokenDate().
[[nodiscard]] inline std::expected<void, std::string> fetch_and_store_first_token_date(
    const std::string& api_key = "",
    const std::string& auth_token = "") {

    // One-time guard
    if (get_first_token_date().has_value()) return {};

    // Determine auth
    std::string effective_key = api_key;
    std::string effective_token = auth_token;
    if (effective_key.empty()) {
        if (auto* env = std::getenv("ANTHROPIC_API_KEY"); env && *env) effective_key = env;
    }
    if (effective_token.empty()) {
        if (auto* env = std::getenv("CLAUDE_AUTH_TOKEN"); env && *env) effective_token = env;
    }

    std::vector<std::string> headers = {"Content-Type: application/json"};
    if (!effective_token.empty()) {
        headers.push_back(std::format("Authorization: Bearer {}", effective_token));
    } else if (!effective_key.empty()) {
        headers.push_back(std::format("x-api-key: {}", effective_key));
    } else {
        return {}; // No auth available
    }

    // Determine base URL
    std::string base_url = "https://api.anthropic.com";
    if (auto* env = std::getenv("ANTHROPIC_BASE_URL"); env && *env) base_url = env;
    std::string url = std::format("{}/api/organization/claude_code_first_token_date", base_url);

    auto result = HttpClient::get(url, headers, std::chrono::milliseconds{10000});
    if (!result) {
        return std::unexpected(std::format("Failed to fetch first token date: network error"));
    }
    if (result->status_code != 200) {
        return std::unexpected(std::format("First token date API returned status {}", result->status_code));
    }

    // Parse response
    auto doc = cc::utils::json::parse(result->body);
    if (!doc) {
        return std::unexpected("Failed to parse first token date response");
    }

    auto date_node = doc->root().get("first_token_date");
    if (!date_node.is_str()) {
        // null/missing — no first token date yet
        return {};
    }

    auto tp = detail::parse_iso_date(std::string(date_node.as_str()));
    if (!tp) {
        return std::unexpected("Invalid first_token_date value in response");
    }

    set_first_token_date(*tp);
    return {};
}

} // namespace cc::services::api
