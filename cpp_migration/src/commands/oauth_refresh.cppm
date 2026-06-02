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

export module cc.commands.oauth_refresh;

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
    const char* home = std::getenv("HOME");
    fs::path base = home
        ? fs::path(home) / ".claude"
        : fs::temp_directory_path() / ".claude";

    std::string filename = ".credentials";
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
/// In the migration module, we validate state and provide guidance.
/// The actual HTTP token exchange requires a network transport layer.
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

    // Check if refresh token is available
    if (!has_refresh_token(suffix)) {
        return {
            .ok = false,
            .message = "No refresh token available. Run /login to re-authenticate.",
            .new_access_token = std::nullopt,
            .expires_in = std::nullopt,
        };
    }

    // In a full implementation, this would:
    // 1. Read the refresh_token from the credentials file
    // 2. POST to the token endpoint with grant_type=refresh_token
    // 3. Save the new access_token and refresh_token
    // 4. Update expires_at
    // The HTTP transport is not wired in this migration module.
    return {
        .ok = false,
        .message = std::format(
            "Token expired. Refresh token available but HTTP transport not configured.\n"
            "Credentials: {}\n"
            "Run /login to re-authenticate interactively.",
            token_path.string()),
        .new_access_token = std::nullopt,
        .expires_in = std::nullopt,
    };
}

} // namespace cc::commands::oauth_refresh
