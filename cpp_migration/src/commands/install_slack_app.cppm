/// @file install_slack_app.cppm
/// @brief InstallSlackAppCommand implementing the /install-slack-app slash command.
///
/// Expanded from the original one-shot "open URL" implementation into a
/// 3-step wizard (pure functions exported for reuse by the Phase 4 FTXUI UI)
/// that mirrors the install-github-app pattern:
///
///   Step 1 – ConfirmEligibility : make sure user has Claude auth and that
///                                 the current plan supports the Slack app.
///   Step 2 – ConfirmInstall     : opt-in checkbox, show what permissions
///                                 the app requests, user presses Enter.
///   Step 3 – OpeningBrowser     : actually open the marketplace URL,
///                                 show fallback if browser fails.
///
/// Side-effects (auth check, browser opening) are the caller's
/// responsibility; this module only defines the step enum, context, and
/// pure validate/advance/description helpers.  The cross-platform open_url
/// helper is retained for convenience.
module;

#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <format>

export module cc.commands.install_slack_app;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands::install_slack_app {

using namespace cc::core;

// ============================================================
// 3-step enumeration
// ============================================================
enum class Step : std::uint8_t {
    ConfirmEligibility = 0,  // 1 – prerequisites check
    ConfirmInstall,          // 2 – user-facing confirmation screen
    OpeningBrowser,          // 3 – perform the open side-effect
};

// ============================================================
// Context
// ============================================================
struct InstallSlackAppContext {
    // --- Written by caller during Step 1 (run `cc auth whoami` / plan check) ---
    bool has_claude_auth = false;
    bool plan_supports_slack_app = true;   // assume yes; caller flips if not
    std::string eligibility_warning;

    // --- Step 2 ---
    bool user_confirmed = false;

    // --- Step 3 outputs (caller fills after browser launch) ---
    bool browser_opened = false;

    /// Static marketplace URL shared by the TS implementation.
    static constexpr std::string_view slack_app_url =
        "https://slack.com/marketplace/A08SF47R6P4-claude";
};

// ============================================================
// Cross-platform browser launcher (kept from original, convenient for tests
// that want to exercise the "real" last step without involving FTXUI).
// ============================================================
namespace detail {

[[nodiscard]] inline bool open_url(std::string_view url) {
#if defined(_WIN32)
    std::string command = "start \"\" \"" + std::string(url) + "\"";
#elif defined(__APPLE__)
    std::string command = "open \"" + std::string(url) + "\"";
#else
    std::string command = "xdg-open \"" + std::string(url) + "\"";
#endif
    return std::system(command.c_str()) == 0;
}

} // namespace detail

// ============================================================
// validate(step, ctx) -> vector<Error>
// ============================================================
[[nodiscard]] std::vector<Error> validate(Step step,
                                                 const InstallSlackAppContext& ctx) {
    std::vector<Error> errs;
    switch (step) {
        case Step::ConfirmEligibility:
            if (!ctx.has_claude_auth) {
                errs.push_back(Error::make(
                    ErrorCode::AuthenticationFailed,
                    "You must be signed in to Claude (`/login`) before installing the Slack app."));
            }
            if (!ctx.plan_supports_slack_app) {
                errs.push_back(Error::make(
                    ErrorCode::PermissionDenied,
                    "The Claude Slack app is not available on your current plan."));
            }
            break;

        case Step::ConfirmInstall:
            if (!ctx.user_confirmed) {
                errs.push_back(Error::make(
                    ErrorCode::InvalidInput,
                    "Press Enter to confirm installation."));
            }
            break;

        case Step::OpeningBrowser:
            // Nothing to validate – if the browser fails we just show a
            // fallback message, it's not a fatal validation error.
            break;
    }
    return errs;
}

// ============================================================
// advance(step, ctx, user_input) -> Step
// ============================================================
[[nodiscard]] Step advance(Step step,
                                  const InstallSlackAppContext& ctx,
                                  std::string_view user_input) {
    (void)user_input;
    switch (step) {
        case Step::ConfirmEligibility:
            // Caller wrote has_claude_auth + plan_supports_slack_app.
            // validate() has already rejected the bad cases before advance()
            // is called, so we can always proceed.
            return Step::ConfirmInstall;

        case Step::ConfirmInstall:
            // validate() ensures user_confirmed == true here.
            return Step::OpeningBrowser;

        case Step::OpeningBrowser:
            // Terminal.  Caller shows "done" + fallback URL if needed.
            return Step::OpeningBrowser;
    }
    return step; // unreachable
}

// ============================================================
// step_description
// ============================================================
[[nodiscard]] std::string_view step_description(Step step) noexcept {
    switch (step) {
        case Step::ConfirmEligibility: return "Checking eligibility";
        case Step::ConfirmInstall:     return "Confirm installation";
        case Step::OpeningBrowser:     return "Opening Slack marketplace";
    }
    return "Unknown";
}

} // namespace cc::commands::install_slack_app

// ============================================================
// Slash command class (thin launcher, same shape as InstallGithubAppCommand)
// ============================================================
export namespace cc::commands {

using namespace cc::core;
namespace steps = install_slack_app;

class InstallSlackAppCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install-slack-app",
            .description = "Install the Claude Slack app",
            .args = {},
            .category = "integrations",
            .aliases = {"install-slack"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext&) { return {}; }

    /// Launcher: returns an injected prompt describing the 3-step wizard
    /// path.  The actual step-by-step interaction is owned by the Phase 4
    /// FTXUI component; the one-liner fallback (open URL directly, no UI)
    /// is preserved as a non-interactive safety net in case the wizard is
    /// ever invoked headlessly.
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Headless / no-wizard path (same behaviour as the old TS command).
        if (!ctx.runtime_state /* || some --no-wizard flag */) {
            if (steps::detail::open_url(steps::InstallSlackAppContext::slack_app_url)) {
                return CommandResult::success(
                    "Opening Slack app installation page in browser...");
            }
            return CommandResult::success(std::format(
                "Could not open browser. Visit: {}",
                steps::InstallSlackAppContext::slack_app_url));
        }

        // Interactive wizard path: return an inject() so the REPL hands off.
        std::string prompt = std::format(
            "[install-slack-app wizard starting]\n"
            "  Slack app URL : {}\n"
            "  Steps         : 3 (eligibility → confirm → open)\n"
            "\nLaunching interactive wizard (FTXUI component, Phase 4).",
            steps::InstallSlackAppContext::slack_app_url);
        return CommandResult::inject(std::move(prompt));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
