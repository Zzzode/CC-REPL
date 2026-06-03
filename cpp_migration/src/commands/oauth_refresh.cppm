/// @file oauth_refresh.cppm
/// @brief OAuth token refresh command — refreshes expired OAuth tokens
/// for Claude AI / console authentication flows.
module;

#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <format>
#include <cstdlib>
#include <unordered_map>

export module cc.commands.oauth_refresh;

import cc.utils.http;
import cc.utils.json;

export namespace cc::commands::oauth_refresh {

namespace fs = std::filesystem;

/// Token state on disk
struct TokenInfo {
    std::string access_token;
    std::string refresh_token;
    std::string token_type;
    std::chrono::system_clock::time_point expires_at;
    std::string account_id;
};

/// Result of refresh operation
struct RefreshResult {
    bool ok{false};
    std::string message;
    std::optional<std::string> new_access_token;
    std::optional<std::chrono::seconds> expires_in;
};

/// Get the OAuth token file path for an account
[[nodiscard]] inline fs::path get_token_path(std::string_view suffix = "") {
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = fs::path(xdg) / "cc-repl";
    } else if (const char* home = std::getenv("HOME")) {
        base = fs::path(home) / ".config" / "cc-repl";
    } else {
        base = fs::temp_directory_path() / "cc-repl";
    }

    std::string filename = "credentials";
    if (!suffix.empty()) {
        filename += std::string(suffix);
    }
    filename += ".json";
    return base / filename;
}

/// Check if the stored token is expired
[[nodiscard]] inline bool is_token_expired(std::string_view suffix = "") {
    auto path = get_token_path(suffix);
    if (!fs::exists(path)) return true;

    // Read file and look for expires_at field
    std::ifstream file(path);
    if (!file.is_open()) return true;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Simple check: look for "expires_at" timestamp
    auto pos = content.find("\"expires_at\":");
    if (pos == std::string::npos) return true;

    pos += 13; // skip key
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '"')) ++pos;

    // Parse unix timestamp
    std::uint64_t expires_ts = 0;
    while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
        expires_ts = expires_ts * 10 + (content[pos] - '0');
        ++pos;
    }

    if (expires_ts == 0) return true;

    auto expires = std::chrono::system_clock::time_point(
        std::chrono::seconds(expires_ts));
    auto now = std::chrono::system_clock::now();

    // Consider expired if within 5 minutes of expiry
    return now >= (expires - std::chrono::minutes(5));
}

/// Check if a refresh token is available
[[nodiscard]] inline bool has_refresh_token(std::string_view suffix = "") {
    auto path = get_token_path(suffix);
    if (!fs::exists(path)) return false;

    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    return content.find("\"refresh_token\"") != std::string::npos;
}

/// Command name
[[nodiscard]] inline auto name() -> std::string_view { return "oauth_refresh"; }

/// Execute OAuth token refresh
[[nodiscard]] inline auto run(std::string_view account = {}) -> RefreshResult {
    std::string suffix;
    if (!account.empty()) {
        suffix = "-" + std::string(account);
    }

    auto token_path = get_token_path(suffix);

    // Check if token file exists
    if (!fs::exists(token_path)) {
        return {
            .ok = false,
            .message = std::format("No OAuth credentials found at {}. Run /login first.",
                token_path.string()),
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    // Check if token is actually expired
    if (!is_token_expired(suffix)) {
        return {
            .ok = true,
            .message = "Token is still valid, no refresh needed.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    if (!has_refresh_token(suffix)) {
        return {
            .ok = false,
            .message = "No refresh token available. Run /login to re-authenticate.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    const char* endpoint = std::getenv("CC_REMOTE_OAUTH_REFRESH_URL");
    if (!endpoint || !*endpoint) {
        return {
            .ok = false,
            .message = "CC_REMOTE_OAUTH_REFRESH_URL is required to refresh OAuth tokens.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto existing_doc = cc::utils::json::parse_file(token_path);
    if (!existing_doc) {
        return {
            .ok = false,
            .message = std::format("Failed to parse credentials: {}", existing_doc.error().message()),
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto root = existing_doc->root();
    if (!root || !root.is_obj()) {
        return {
            .ok = false,
            .message = "Credentials file must contain a JSON object.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto refresh_value = root.get("refresh_token");
    if (!refresh_value || !refresh_value.is_str() || std::string_view(refresh_value.as_str()).empty()) {
        return {
            .ok = false,
            .message = "No refresh token available. Run /login to re-authenticate.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto request = cc::utils::json::object();
    request.set("grant_type", "refresh_token");
    request.set("refresh_token", refresh_value.as_str());

    cc::utils::HttpClient client;
    auto response = client.post(endpoint, request.serialize(), std::unordered_map<std::string, std::string>{
        {"Content-Type", "application/json"},
    });
    if (!response) {
        return {
            .ok = false,
            .message = response.error().message,
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }
    if (!response->is_ok()) {
        return {
            .ok = false,
            .message = std::format("Token refresh failed with status {}: {}", response->status, response->body),
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto refreshed_doc = cc::utils::json::parse(response->body);
    if (!refreshed_doc) {
        return {
            .ok = false,
            .message = std::format("Token refresh returned invalid JSON: {}", refreshed_doc.error().message()),
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto refreshed = refreshed_doc->root();
    if (!refreshed || !refreshed.is_obj()) {
        return {
            .ok = false,
            .message = "Token refresh response must be a JSON object.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    auto access_value = refreshed.get("access_token");
    if (!access_value || !access_value.is_str() || std::string_view(access_value.as_str()).empty()) {
        return {
            .ok = false,
            .message = "Token refresh response missing access_token.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    const auto now = std::chrono::system_clock::now();
    std::chrono::seconds expires_in{3600};
    if (auto expires = refreshed.get("expires_in"); expires && expires.is_num()) {
        expires_in = std::chrono::seconds(expires.as_int());
    }
    const auto expires_at = now + expires_in;
    const auto expires_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        expires_at.time_since_epoch()).count();

    auto output = cc::utils::json::object();
    output.set("access_token", access_value.as_str());
    if (auto next_refresh = refreshed.get("refresh_token"); next_refresh && next_refresh.is_str()) {
        output.set("refresh_token", next_refresh.as_str());
    } else {
        output.set("refresh_token", refresh_value.as_str());
    }
    if (auto token_type = refreshed.get("token_type"); token_type && token_type.is_str()) {
        output.set("token_type", token_type.as_str());
    } else if (auto existing_type = root.get("token_type"); existing_type && existing_type.is_str()) {
        output.set("token_type", existing_type.as_str());
    } else {
        output.set("token_type", "Bearer");
    }
    output.set("expires_at", static_cast<int64_t>(expires_epoch));
    if (auto account_id = root.get("account_id"); account_id && account_id.is_str()) {
        output.set("account_id", account_id.as_str());
    }

    fs::create_directories(token_path.parent_path());
    std::ofstream out(token_path, std::ios::trunc);
    if (!out) {
        return {
            .ok = false,
            .message = std::format("Failed to write credentials at {}", token_path.string()),
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }
    out << output.serialize();

    return {
        .ok = true,
        .message = std::format("OAuth token refreshed and saved to {}.", token_path.string()),
        .new_access_token = std::string(access_value.as_str()),
        .expires_in = expires_in,
    };
}

} // namespace cc::commands::oauth_refresh
