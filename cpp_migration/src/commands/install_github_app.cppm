/// @file install_github_app.cppm
/// @brief InstallGithubAppCommand implementing the /install-github-app slash command.
///
/// The command itself is intentionally thin: its execute() returns an
/// `Injected` CommandResult carrying a short textual summary of the wizard
/// path.  The interactive multi-step FTXUI wizard that actually walks the
/// user through the 12 steps defined in install_github_app_steps.cppm is
/// deferred to Phase 4 (UI Agent).  Keeping the business pure functions in
/// the sibling `install_github_app_steps` module means the wizard UI and
/// unit tests can both drive the state machine without duplicating logic.
module;

#include <expected>
#include <string>
#include <vector>
#include <format>

export module cc.commands.install_github_app;

import cc.types.types;
import cc.commands.command;
import cc.commands.install_github_app_steps;

export namespace cc::commands {

using namespace cc::core;
namespace steps = install_github_app;

/// InstallGithubAppCommand implements the /install-github-app slash command.
///
/// Design note: the class does NOT own wizard runtime state (that lives on
/// the FTXUI component in Phase 4).  It is a stateless launcher that:
///   1. Validates the context is sane (no conflicting args, etc.).
///   2. Builds an initial InstallGitHubAppContext with the TS-equivalent
///      INITIAL_STATE defaults.
///   3. Returns an injected prompt describing the wizard so the REPL can
///      render / hand off to the FTXUI wizard.
class InstallGithubAppCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "install-github-app",
            .description = "Install and configure the Claude GitHub App for Actions",
            .args = {
                CommandArg{
                    .name = "repo",
                    .description = "Optional target repo as owner/repo (skips the choose-repo step)",
                    .type = ArgType::Text,
                    .required = false,
                    .choices = {},
                    .default_value = std::nullopt,
                },
            },
            .category = "integrations",
            .aliases = {"install-gh-app"},
            .hidden = false,
        };
    }

    /// Lightweight argument validation.  Deep per-step validation lives in
    /// steps::validate() and is called by the wizard driver.
    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.size() > 1) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidInput,
                std::format("Expected at most 1 argument (repo), got {}", ctx.args.size())));
        }
        return {};
    }

    /// Launch the wizard.  Returns CommandResult::inject() so that the REPL
    /// loop treats the returned string as a prompt for the FTXUI wizard
    /// (Phase 4) rather than as terminal output.
    ///
    /// If `ctx.args[0]` is present it is treated as the target repo and
    /// pre-populated into the initial context (use_current_repo = false).
    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        steps::InstallGitHubAppContext initial = make_initial_context(ctx);

        std::string prompt = std::format(
            "[install-github-app wizard starting]\n"
            "  Target repo : {}\n"
            "  First step  : {} (Step {})\n"
            "  Workflows   : {}\n"
            "  Secret name : {}\n"
            "  Auth mode   : {}\n"
            "\nLaunching interactive wizard (FTXUI component, Phase 4).",
            initial.repo().empty() ? std::string{"<auto-detect>"} : initial.repo(),
            steps::step_description(steps::Step::CheckGitHub),
            static_cast<int>(steps::Step::CheckGitHub) + 1,
            join_workflows(initial.selected_workflows),
            initial.secret_name,
            initial.auth_type == steps::AuthType::ApiKey ? "api_key"
                                                         : "oauth_token");

        return CommandResult::inject(std::move(prompt));
    }

    /// Bash-style completion: for the first positional we could suggest
    /// repos from `gh repo list`, but that requires IO.  For now return
    /// the empty list – the wizard UI owns richer completion.
    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        (void)partial;
        return {};
    }

private:
    /// Mirrors the TS INITIAL_STATE record.
    [[nodiscard]] static steps::InstallGitHubAppContext make_initial_context(
        const CommandContext& ctx) {
        steps::InstallGitHubAppContext s;
        s.use_current_repo = ctx.args.empty();
        if (!ctx.args.empty()) {
            s.selected_repo_name = ctx.args[0];
        }
        s.secret_name = "ANTHROPIC_API_KEY";
        s.use_existing_secret = true;
        s.selected_workflows = {"claude", "claude-review"};
        s.auth_type = steps::AuthType::ApiKey;
        s.selected_api_key_option = steps::ApiKeyOption::ExistingLocal;
        s.workflow_action = steps::WorkflowAction::Update;
        return s;
    }

    [[nodiscard]] static std::string join_workflows(
        const std::vector<std::string>& ws) {
        if (ws.empty()) return "<none>";
        std::string out = ws[0];
        for (std::size_t i = 1; i < ws.size(); ++i) {
            out += ", " + ws[i];
        }
        return out;
    }
};

} // namespace cc::commands
