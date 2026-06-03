/// @file login.cppm
/// @brief LoginCommand implementing the /login slash command.
/// OAuth flow trigger, API key entry, show auth status.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>

export module cc.commands.login;

import cc.types.types;
import cc.commands.command;
import cc.services.oauth.client;
import cc.constants.oauth;

export namespace cc::commands {

using namespace cc::core;

/// Authentication method used for login
enum class AuthMethod : std::uint8_t {
    OAuth,      // Browser-based OAuth flow
    ApiKey,     // Direct API key entry
};

/// Current authentication state
struct AuthState {
    bool authenticated = false;
    std::optional<std::string> account_email;
    std::optional<AuthMethod> method;
    std::optional<std::string> organization;
};

/// LoginCommand implements the /login slash command.
/// Supports OAuth flow and API key authentication.
class LoginCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "login",
            .description = "Authenticate with Anthropic API",
            .aliases = {"auth"},
            .args = {
                CommandArg{.name = "method", .description = "oauth | apikey | status",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"oauth", "apikey", "status"}},
            },
            .hidden = false,
            .category = "auth",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto method = ctx.args[0];
        if (method != "oauth" && method != "apikey" && method != "status") {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid method: '{}'. Use: oauth|apikey|status", method)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) {
            // Default: show status if already logged in, otherwise start OAuth
            if (state_.authenticated) {
                return CommandResult::success(format_status());
            }
            return start_oauth();
        }

        auto method = std::string(ctx.args[0]);

        if (method == "status") return CommandResult::success(format_status());
        if (method == "oauth") return start_oauth();
        if (method == "apikey") return start_apikey_flow();

        return CommandResult::success(format_status());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"oauth", "apikey", "status"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Get current authentication state
    [[nodiscard]] const AuthState& auth_state() const noexcept { return state_; }

private:
    AuthState state_;

    [[nodiscard]] static std::filesystem::path credentials_path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
            return std::filesystem::path(xdg) / "cc-repl" / "credentials.json";
        }
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".config" / "cc-repl" / "credentials.json";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "credentials.json";
    }

    [[nodiscard]] static std::string json_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (unsigned char ch : value) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
                    } else {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
            }
        }
        return out;
    }

    [[nodiscard]] static Result<CommandResult> write_credential_file(std::string_view body) {
        auto path = credentials_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to create credential directory: {}", ec.message())));
        }

        std::ofstream output(path, std::ios::trunc);
        if (!output.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to write credentials to '{}'.", path.string())));
        }
        output << body;
        output.close();
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to restrict credential permissions: {}", ec.message())));
        }
        return CommandResult::success(std::format("Credentials saved to '{}'.", path.string()));
    }

    [[nodiscard]] static Result<CommandResult> write_credentials(std::string_view type, std::string_view value) {
        auto body = std::format(
            R"({{"type":"{}","value":"{}","api_key":"{}"}})",
            json_escape(type), json_escape(value),
            type == "api_key" ? json_escape(value) : "");
        return write_credential_file(body);
    }

    [[nodiscard]] static Result<CommandResult> write_oauth_credentials(
        const cc::services::oauth::TokenPair& token) {
        auto expires_at = token.issued_at + std::chrono::seconds(token.expires_in);
        auto expires_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            expires_at.time_since_epoch()).count();
        auto body = std::format(
            R"({{"type":"oauth","access_token":"{}","refresh_token":"{}","token_type":"{}","scope":"{}","expires_at":{}}})",
            json_escape(token.access_token),
            json_escape(token.refresh_token),
            json_escape(token.token_type.empty() ? "Bearer" : token.token_type),
            json_escape(token.scope),
            expires_epoch);
        return write_credential_file(body);
    }

    [[nodiscard]] static std::string oauth_error_message(cc::services::oauth::OAuthError error) {
        using cc::services::oauth::OAuthError;
        switch (error) {
            case OAuthError::AuthorizationFailed: return "OAuth authorization failed.";
            case OAuthError::TokenExchangeFailed: return "OAuth token exchange failed.";
            case OAuthError::TokenRefreshFailed: return "OAuth token refresh failed.";
            case OAuthError::TokenExpired: return "OAuth token expired.";
            case OAuthError::TokenInvalid: return "OAuth token is invalid.";
            case OAuthError::NetworkError: return "OAuth network request failed.";
            case OAuthError::KeychainError: return "OAuth credential storage failed.";
            case OAuthError::CallbackServerError: return "OAuth callback server failed to start.";
            case OAuthError::PkceError: return "OAuth PKCE challenge failed.";
            case OAuthError::InvalidState: return "OAuth callback state did not match.";
            case OAuthError::InvalidGrant: return "OAuth authorization grant was invalid.";
        }
        return "OAuth failed.";
    }

    [[nodiscard]] std::string format_status() const {
        if (!state_.authenticated) {
            return "Not authenticated.\nUse /login oauth or /login apikey to authenticate.";
        }
        std::string out = "Authenticated:\n";
        out += std::format("  Account: {}\n", state_.account_email.value_or("unknown"));
        out += std::format("  Method:  {}\n", state_.method == AuthMethod::OAuth ? "OAuth" : "API Key");
        if (state_.organization) {
            out += std::format("  Org:     {}", *state_.organization);
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> start_oauth() {
        if (const char* token = std::getenv("ANTHROPIC_AUTH_TOKEN")) {
            if (*token == '\0') {
                return std::unexpected(Error::make(ErrorCode::AuthenticationFailed,
                    "ANTHROPIC_AUTH_TOKEN is set but empty."));
            }
            cc::services::oauth::TokenPair pair{
                .access_token = token,
                .refresh_token = "",
                .token_type = "Bearer",
                .expires_in = 365 * 24 * 60 * 60,
                .issued_at = std::chrono::system_clock::now(),
                .scope = "",
            };
            auto saved = write_oauth_credentials(pair);
            if (!saved) return saved;
            state_.authenticated = true;
            state_.method = AuthMethod::OAuth;
            return CommandResult::success("Authenticated with ANTHROPIC_AUTH_TOKEN.");
        }
        cc::services::oauth::OAuthConfig config{
            .client_id = std::string(cc::constants::oauth::prod_oauth_config.client_id),
            .authorization_endpoint = std::string(cc::constants::oauth::prod_oauth_config.claude_ai_authorize_url),
            .token_endpoint = std::string(cc::constants::oauth::prod_oauth_config.token_url),
            .redirect_uri = "http://localhost:19485/callback",
            .scopes = {},
            .keychain_service = "cc-repl-oauth",
            .callback_port = 19485,
            .auth_timeout = std::chrono::seconds{300},
        };
        for (auto scope : cc::constants::oauth::claude_ai_oauth_scopes) {
            config.scopes.emplace_back(scope);
        }

        cc::services::oauth::OAuthClient client(std::move(config));
        auto token = client.authorize_interactive();
        if (!token) {
            return std::unexpected(Error::make(
                ErrorCode::AuthenticationFailed,
                oauth_error_message(token.error())));
        }
        auto saved = write_oauth_credentials(*token);
        if (!saved) return saved;
        state_.authenticated = true;
        state_.method = AuthMethod::OAuth;
        return CommandResult::success("Authenticated with OAuth.");
    }

    [[nodiscard]] Result<CommandResult> start_apikey_flow() {
        if (const char* key = std::getenv("ANTHROPIC_API_KEY")) {
            std::string_view key_view(key);
            if (!key_view.starts_with("sk-")) {
                return std::unexpected(Error::make(ErrorCode::AuthenticationFailed,
                    "ANTHROPIC_API_KEY does not look like an Anthropic API key."));
            }
            auto saved = write_credentials("api_key", key_view);
            if (!saved) return saved;
            state_.authenticated = true;
            state_.method = AuthMethod::ApiKey;
            return CommandResult::success("Authenticated with ANTHROPIC_API_KEY.");
        }
        return std::unexpected(Error::make(
            ErrorCode::AuthenticationFailed,
            "Secure interactive API key entry is not available in this native command path. Set ANTHROPIC_API_KEY and rerun /login apikey."));
    }
};

} // namespace cc::commands
