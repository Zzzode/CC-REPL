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

export module cc.commands.login;

import cc.types.types;
import cc.commands.command;

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
                           .choices = {{"oauth", "apikey", "status"}}},
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
        // In production: launch browser for OAuth flow via libuv
        return CommandResult::success(
            "Opening browser for authentication...\n"
            "Waiting for OAuth callback on http://localhost:9876/callback\n"
            "If the browser doesn't open, visit the URL manually.");
    }

    [[nodiscard]] Result<CommandResult> start_apikey_flow() {
        // In production: prompt for API key securely
        return CommandResult::success(
            "Enter your Anthropic API key (starts with 'sk-ant-'):\n"
            "The key will be stored securely in your system keychain.");
    }
};

} // namespace cc::commands
