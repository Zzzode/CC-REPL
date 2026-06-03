/// @file remote_auth.cppm
/// @brief Remote authentication with OAuth token management.
/// Handles token storage (file-based), refresh via HTTP POST,
/// expiry tracking, and credential retrieval for remote connections.
module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <format>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

export module cc.remote.remote_auth;

export import cc.utils.json;
import cc.utils.http;

export namespace cc::remote {

// ============================================================
// Token storage
// ============================================================

struct StoredToken {
    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    std::chrono::system_clock::time_point expires_at;
    std::string scope;
};

/// Credential source type
enum class AuthSource {
    None,
    OAuthFile,      // ~/.claude/credentials.json
    EnvVar,         // CC_API_KEY environment variable
    SessionToken,   // Short-lived session token
};

// ============================================================
// RemoteAuth — Token management for remote connections
// ============================================================

class RemoteAuth {
public:
    RemoteAuth() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            credentials_path_ = std::filesystem::path(xdg) / "cc-repl" / "credentials.json";
        } else if (const char* home = std::getenv("HOME")) {
            credentials_path_ = std::filesystem::path(home) / ".config" / "cc-repl" / "credentials.json";
        }
    }

    /// Authenticate with a remote host using stored credentials.
    /// Returns an access token ready for use in Authorization header.
    [[nodiscard]] auto get_token() -> std::expected<std::string, std::string> {
        std::lock_guard lock(mutex_);

        // Check environment variables first.
        if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key) {
            source_ = AuthSource::EnvVar;
            return std::string(key);
        }
        if (const char* key = std::getenv("CC_API_KEY"); key && *key) {
            source_ = AuthSource::EnvVar;
            return std::string(key);
        }

        // Load from file if not already cached
        if (!token_.has_value()) {
            auto load_result = load_credentials();
            if (!load_result) {
                return std::unexpected(load_result.error());
            }
        }

        // Check if token is expired
        if (is_expired()) {
            auto refresh_result = refresh();
            if (!refresh_result) {
                // Token expired and refresh failed — clear and fail
                token_.reset();
                return std::unexpected(refresh_result.error());
            }
        }

        source_ = AuthSource::OAuthFile;
        return token_->access_token;
    }

    /// Store credentials from OAuth flow
    auto store_credentials(StoredToken token) -> std::expected<void, std::string> {
        std::lock_guard lock(mutex_);
        token_ = std::move(token);
        return save_credentials();
    }

    /// Refresh the access token using the stored refresh token
    [[nodiscard]] auto refresh() -> std::expected<void, std::string> {
        if (!token_.has_value() || token_->refresh_token.empty()) {
            return std::unexpected("No refresh token available");
        }

        const char* endpoint = std::getenv("CC_REMOTE_OAUTH_REFRESH_URL");
        if (!endpoint || !*endpoint) {
            return std::unexpected("CC_REMOTE_OAUTH_REFRESH_URL is required to refresh remote OAuth tokens");
        }

        auto request = cc::utils::json::object();
        request.set("grant_type", "refresh_token");
        request.set("refresh_token", token_->refresh_token);

        cc::utils::HttpClient client;
        auto response = client.post(endpoint, request.serialize(), std::unordered_map<std::string, std::string>{
            {"Content-Type", "application/json"},
        });
        if (!response) {
            return std::unexpected(response.error().message);
        }
        if (!response->is_ok()) {
            return std::unexpected(std::format(
                "Token refresh failed with status {}: {}",
                response->status, response->body));
        }

        auto doc = cc::utils::json::parse(response->body);
        if (!doc) {
            return std::unexpected(std::format("Token refresh returned invalid JSON: {}", doc.error().message()));
        }

        auto root = doc->root();
        if (!root || !root.is_obj()) {
            return std::unexpected("Token refresh response must be a JSON object");
        }
        if (auto access = root.get("access_token"); access && access.is_str()) {
            token_->access_token = std::string(access.as_str());
        } else {
            return std::unexpected("Token refresh response missing access_token");
        }
        if (auto refresh_token = root.get("refresh_token"); refresh_token && refresh_token.is_str()) {
            token_->refresh_token = std::string(refresh_token.as_str());
        }
        if (auto token_type = root.get("token_type"); token_type && token_type.is_str()) {
            token_->token_type = std::string(token_type.as_str());
        }
        if (auto scope = root.get("scope"); scope && scope.is_str()) {
            token_->scope = std::string(scope.as_str());
        }
        if (auto expires_in = root.get("expires_in"); expires_in && expires_in.is_num()) {
            token_->expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in.as_int());
        } else if (auto expires_at = root.get("expires_at"); expires_at && expires_at.is_num()) {
            token_->expires_at = std::chrono::system_clock::time_point(std::chrono::seconds(expires_at.as_int()));
        } else {
            token_->expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
        }

        return save_credentials();
    }

    /// Check if we have valid authentication
    [[nodiscard]] bool is_authenticated() const {
        std::lock_guard lock(mutex_);
        
        if (std::getenv("ANTHROPIC_API_KEY") || std::getenv("CC_API_KEY")) return true;
        if (!token_.has_value()) return false;
        return !is_expired();
    }

    /// Clear all stored authentication state
    void clear() {
        std::lock_guard lock(mutex_);
        token_.reset();
        source_ = AuthSource::None;

        // Remove credentials file
        std::error_code ec;
        std::filesystem::remove(credentials_path_, ec);
    }

    /// Get the authentication source type
    [[nodiscard]] AuthSource source() const { return source_; }

    /// Get token expiry time (if available)
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> expires_at() const {
        std::lock_guard lock(mutex_);
        if (token_.has_value()) return token_->expires_at;
        return std::nullopt;
    }

    /// Build Authorization header value
    [[nodiscard]] auto authorization_header() -> std::expected<std::string, std::string> {
        auto token = get_token();
        if (!token) return std::unexpected(token.error());
        return std::format("Bearer {}", *token);
    }

private:
    /// Check if the current token is expired
    [[nodiscard]] bool is_expired() const {
        if (!token_.has_value()) return true;
        auto now = std::chrono::system_clock::now();
        // Consider expired 60 seconds before actual expiry for safety margin
        return now >= (token_->expires_at - std::chrono::seconds{60});
    }

    /// Load credentials from file
    auto load_credentials() -> std::expected<void, std::string> {
        if (credentials_path_.empty() || !std::filesystem::exists(credentials_path_)) {
            return std::unexpected("No credentials file found");
        }

        auto doc_result = cc::utils::json::parse_file(credentials_path_);
        if (!doc_result) {
            return std::unexpected(std::format(
                "Failed to parse credentials: {}", doc_result.error().message()));
        }

        auto root = doc_result->root();
        if (!root || !root.is_obj()) {
            return std::unexpected("Invalid credentials format");
        }

        StoredToken tok;
        if (auto v = root.get("access_token"); v && v.is_str()) {
            tok.access_token = std::string(v.as_str());
        } else {
            return std::unexpected("Missing access_token in credentials");
        }

        if (auto v = root.get("refresh_token"); v && v.is_str()) {
            tok.refresh_token = std::string(v.as_str());
        }
        if (auto v = root.get("token_type"); v && v.is_str()) {
            tok.token_type = std::string(v.as_str());
        }
        if (auto v = root.get("scope"); v && v.is_str()) {
            tok.scope = std::string(v.as_str());
        }
        if (auto v = root.get("expires_at"); v && v.is_num()) {
            auto epoch_secs = v.as_int();
            tok.expires_at = std::chrono::system_clock::time_point(
                std::chrono::seconds(epoch_secs));
        } else {
            // Default: assume 1 hour from now
            tok.expires_at = std::chrono::system_clock::now() + std::chrono::hours(1);
        }

        token_ = std::move(tok);
        return {};
    }

    /// Save credentials to file
    auto save_credentials() -> std::expected<void, std::string> {
        if (credentials_path_.empty()) {
            return std::unexpected("No credentials path configured");
        }
        if (!token_.has_value()) {
            return std::unexpected("No token to save");
        }

        // Ensure directory exists
        std::error_code ec;
        std::filesystem::create_directories(credentials_path_.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format("Failed to create credentials dir: {}", ec.message()));
        }

        // Serialize token as JSON
        auto expires_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            token_->expires_at.time_since_epoch()).count();

        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();
        root.add("access_token", doc.string(token_->access_token));
        root.add("refresh_token", doc.string(token_->refresh_token));
        root.add("token_type", doc.string(token_->token_type));
        root.add("scope", doc.string(token_->scope));
        root.add("expires_at", doc.number(static_cast<int64_t>(expires_epoch)));
        doc.set_root(root);

        std::ofstream file(credentials_path_);
        if (!file.is_open()) {
            return std::unexpected("Failed to open credentials file for writing");
        }
        file << doc.to_pretty_string();
        if (!file.good()) {
            return std::unexpected("Failed to write credentials file");
        }

        // Set file permissions to owner-only (0600)
        std::filesystem::permissions(credentials_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);

        return {};
    }

    // State
    std::optional<StoredToken> token_;
    AuthSource source_ = AuthSource::None;
    std::filesystem::path credentials_path_;
    mutable std::mutex mutex_;
};

// ============================================================
// Convenience free functions (backward-compatible API)
// ============================================================

/// Get the global RemoteAuth instance
inline RemoteAuth& global_remote_auth() {
    static RemoteAuth instance;
    return instance;
}

/// Check if we have valid remote authentication
inline bool is_remote_authenticated() {
    return global_remote_auth().is_authenticated();
}

/// Clear all remote authentication state
inline void clear_remote_auth() {
    global_remote_auth().clear();
}

} // namespace cc::remote
