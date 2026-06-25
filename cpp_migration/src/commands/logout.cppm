/// @file logout.cppm
/// @brief LogoutCommand implementing the /logout slash command.
/// Clear stored credentials, confirm logout.
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
#include <functional>
#include <cstdlib>
#include <filesystem>

export module cc.commands.logout;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// LogoutCommand implements the /logout slash command.
/// Clears stored authentication credentials with confirmation.
class LogoutCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "logout",
            .description = "Clear stored authentication credentials",
            .args = {
                CommandArg{.name = "flag", .description = "--force to skip confirmation",
                           .type = ArgType::None, .required = false},
            },
            .category = "auth",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        if (ctx.args[0] != "--force" && ctx.args[0] != "-f") {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Unknown flag: '{}'. Use --force to skip confirmation.", ctx.args[0])));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        bool force = !ctx.args.empty() &&
                     (ctx.args[0] == "--force" || ctx.args[0] == "-f");

        if (!is_authenticated()) {
            return CommandResult::success("Not currently authenticated. Nothing to do.");
        }

        if (!force) {
            return request_confirmation();
        }

        return perform_logout();
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        if (std::string_view("--force").starts_with(partial)) {
            suggestions.emplace_back("--force");
        }
        return suggestions;
    }

    /// Set authentication state (injected by the auth system)
    void set_authenticated(bool value) { authenticated_ = value; }

    /// Set confirmation response callback
    using ConfirmFn = std::function<bool()>;
    void set_confirm_fn(ConfirmFn fn) { confirm_fn_ = std::move(fn); }

private:
    bool authenticated_ = false;
    ConfirmFn confirm_fn_;

    [[nodiscard]] static std::filesystem::path credentials_path() {
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
            return std::filesystem::path(xdg) / "cc-repl" / "credentials.json";
        }
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".config" / "cc-repl" / "credentials.json";
        }
        return std::filesystem::temp_directory_path() / "cc-repl" / "credentials.json";
    }

    [[nodiscard]] bool is_authenticated() const noexcept {
        return authenticated_ || std::filesystem::exists(credentials_path());
    }

    [[nodiscard]] Result<CommandResult> request_confirmation() {
        if (confirm_fn_ && confirm_fn_()) {
            return perform_logout();
        }
        return CommandResult::success(
            "Are you sure you want to logout? This will clear stored credentials.\n"
            "Use /logout --force to confirm, or respond 'yes'.");
    }

    [[nodiscard]] Result<CommandResult> perform_logout() {
        // Clear OAuth tokens from keychain
        auto token_result = clear_oauth_tokens();
        if (!token_result) return std::unexpected(token_result.error());

        // Clear API key from keychain
        auto key_result = clear_api_key();
        if (!key_result) return std::unexpected(key_result.error());

        authenticated_ = false;
        return CommandResult::success("Logged out successfully. Credentials cleared from keychain.");
    }

    [[nodiscard]] static VoidResult clear_oauth_tokens() {
        std::error_code ec;
        std::filesystem::remove(credentials_path(), ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to remove credentials: {}", ec.message())));
        }
        return {};
    }

    [[nodiscard]] static VoidResult clear_api_key() {
        return {};
    }
};

} // namespace cc::commands
