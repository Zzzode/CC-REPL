/// @file install_github_app_steps.cppm
/// @brief Pure-logic step definitions for the /install-github-app wizard.
///
/// Contains the Step enumeration, the InstallGitHubAppContext state struct,
/// Warning type, and three families of pure functions:
///   * validate(step, ctx)    -> std::vector<Error>
///   * advance(step, ctx, input) -> Step
///   * step_description(step) -> std::string_view
///
/// Side-effect-heavy work (shelling out to `gh`, writing files, starting the
/// OAuth HTTP listener, etc.) is intentionally NOT performed here.  Callers
/// (the FTXUI wizard in Phase 4, or tests) are expected to execute those
/// operations between calls and write the results back into the context
/// before invoking advance() again.  Every transition that requires the
/// caller to perform IO is annotated with `// [IO]` below.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <regex>
#include <algorithm>
#include <format>

export module cc.commands.install_github_app_steps;

import cc.types.types;

export namespace cc::commands::install_github_app {

using namespace cc::core;

// ============================================================
// Step enumeration – 12 wizard steps in execution order.
// ============================================================
enum class Step : std::uint8_t {
    CheckGitHub = 0,         // 1 – auto-run: gh CLI presence + auth + scopes
    Warnings,                // 2 – display warnings surfaced by step 1/3
    ChooseRepo,              // 3 – use current repo or type owner/repo
    InstallApp,              // 4 – open browser to github.com/apps/claude
    CheckExistingWorkflow,   // 5 – .github/workflows/claude*.yml already present
    SelectWorkflows,         // 6 – pick which workflow files to generate
    CheckExistingSecret,     // 7 – repo already has ANTHROPIC_API_KEY (or similar)
    ApiKey,                  // 8 – enter key, reuse existing, or go OAuth
    OAuthFlow,               // 9 – browser OAuth flow with callback listener
    Creating,                // 10 – auto-run: mutate repo (secrets, workflows)
    Success,                 // 11 – terminal success screen
    Error,                   // 12 – terminal error screen
};

// ============================================================
// Helper types
// ============================================================

/// A single advisory warning surfaced by the preflight checks.
struct Warning {
    std::string title;
    std::string message;
    std::vector<std::string> instructions;
};

/// Which authentication method is ultimately stored in the repo secret.
enum class AuthType : std::uint8_t {
    ApiKey,      // plain ANTHROPIC_API_KEY
    OAuthToken,  // CLAUDE_CODE_OAUTH_TOKEN
};

/// Action the user chose for an existing workflow file.
enum class WorkflowAction : std::uint8_t {
    Update,   // overwrite existing .github/workflows/claude*.yml
    Skip,     // leave existing files alone
    Exit,     // cancel wizard entirely
};

/// Radio-button selection inside the ApiKey step.
enum class ApiKeyOption : std::uint8_t {
    ExistingLocal,   // use the key already on disk (config)
    NewManual,       // user typed a new key in the input
    OAuthFlow,       // jump to the OAuth browser flow
};

// ============================================================
// Context – full mutable state carried between steps.
// ============================================================
struct InstallGitHubAppContext {
    // --- CheckGitHub outputs (populated by caller after running gh commands) ---
    bool gh_cli_installed = false;
    bool gh_authenticated = false;
    bool has_repo_scope = false;
    bool has_workflow_scope = false;
    std::vector<Warning> preflight_warnings;

    // --- ChooseRepo ---
    bool use_current_repo = true;
    std::string current_repo;          // "owner/repo" detected from cwd
    std::string selected_repo_name;    // "owner/repo" chosen by user

    // --- InstallApp ---
    // No additional state; browser opening is a caller-side side-effect.

    // --- Workflow detection / selection ---
    bool workflow_exists = false;
    WorkflowAction workflow_action = WorkflowAction::Update;
    std::vector<std::string> selected_workflows{"claude", "claude-review"};

    // --- Secret detection ---
    bool secret_exists = false;
    std::string secret_name = "ANTHROPIC_API_KEY";
    bool use_existing_secret = true;

    // --- ApiKey / OAuth ---
    ApiKeyOption selected_api_key_option = ApiKeyOption::ExistingLocal;
    std::string manual_api_key;          // what the user typed in the input
    std::string local_api_key;           // loaded from disk by caller
    AuthType auth_type = AuthType::ApiKey;

    // OAuth results (written by caller after running the listener)
    bool oauth_completed = false;
    std::string oauth_access_token;
    std::string oauth_error;

    // --- Creating step progress (caller updates after each sub-step) ---
    struct ProgressStep {
        std::string label;
        enum class State { Pending, InProgress, Completed, Failed } state = State::Pending;
        std::optional<std::string> error_detail;
    };
    std::vector<ProgressStep> progress_steps;

    // --- Terminal states ---
    std::string error_reason;            // human-readable headline
    std::string error_instructions;      // what to do next

    // Convenience: resolved repo name honoring use_current_repo.
    [[nodiscard]] const std::string& repo() const noexcept {
        return use_current_repo ? current_repo : selected_repo_name;
    }
};

// ============================================================
// Pure helpers – not exported, used by validate/advance.
// ============================================================
namespace detail {

/// Accept "owner/repo" or a full https/ssh GitHub URL.
[[nodiscard]] bool is_valid_repo_ref(std::string_view s) {
    if (s.empty()) return false;

    // owner/repo form
    static const std::regex owner_repo{R"(^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$)"};
    if (std::regex_match(std::string(s), owner_repo)) return true;

    // https://github.com/owner/repo(.git)?
    static const std::regex https_url{
        R"(^https://github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(\.git)?/?$)"};
    if (std::regex_match(std::string(s), https_url)) return true;

    // git@github.com:owner/repo(.git)?
    static const std::regex ssh_url{
        R"(^git@github\.com:([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(\.git)?$)"};
    if (std::regex_match(std::string(s), ssh_url)) return true;

    return false;
}

/// GitHub secret names: alphanumeric + underscore only.
[[nodiscard]] bool is_valid_secret_name(std::string_view s) {
    static const std::regex pat{R"(^[A-Za-z0-9_]+$)"};
    return !s.empty() && std::regex_match(std::string(s), pat);
}

/// A non-empty Anthropic API key starts with "sk-ant-" and is long enough.
[[nodiscard]] bool is_plausible_api_key(std::string_view s) {
    return s.size() >= 40 && s.substr(0, 6) == "sk-ant-";
}

[[nodiscard]] bool has_any_workflow_selected(const InstallGitHubAppContext& ctx) {
    return std::ranges::any_of(ctx.selected_workflows,
                               [](const std::string& w) { return !w.empty(); });
}

} // namespace

// ============================================================
// validate(step, ctx) -> vector<Error>
//
// Returns the *blocking* issues in the context that would make it unsafe for
// the caller to move forward from `step`.  An empty vector means "proceed".
// Non-fatal advisories (e.g. "you are missing workflow scope but the wizard
// can still partially complete") are reported as Warning entries on the
// context itself – not through this function.
// ============================================================
[[nodiscard]] std::vector<Error> validate(Step step,
                                                 const InstallGitHubAppContext& ctx) {
    std::vector<Error> errs;

    switch (step) {
        case Step::CheckGitHub:
            if (!ctx.gh_cli_installed) {
                errs.push_back(Error::make(
                    ErrorCode::NotFound,
                    "GitHub CLI (`gh`) is not installed or not on PATH."));
            }
            break;

        case Step::Warnings:
            // Warnings are informational – no validation failure possible.
            break;

        case Step::ChooseRepo:
            if (ctx.use_current_repo) {
                if (ctx.current_repo.empty()) {
                    errs.push_back(Error::make(
                        ErrorCode::InvalidInput,
                        "Could not detect the current repo from working directory."));
                } else if (!detail::is_valid_repo_ref(ctx.current_repo)) {
                    errs.push_back(Error::make(
                        ErrorCode::InvalidInput,
                        std::format("Detected repo '{}' is not a valid owner/repo reference.",
                                    ctx.current_repo)));
                }
            } else {
                if (ctx.selected_repo_name.empty()) {
                    errs.push_back(Error::make(
                        ErrorCode::InvalidInput,
                        "Repository name is required."));
                } else if (!detail::is_valid_repo_ref(ctx.selected_repo_name)) {
                    errs.push_back(Error::make(
                        ErrorCode::InvalidInput,
                        std::format("'{}' is not a valid GitHub repo reference "
                                    "(expected owner/repo or a full URL).",
                                    ctx.selected_repo_name)));
                }
            }
            break;

        case Step::InstallApp:
            if (ctx.repo().empty()) {
                errs.push_back(Error::make(ErrorCode::InternalError,
                    "InstallApp reached without a resolved repo."));
            }
            break;

        case Step::CheckExistingWorkflow:
            // Choice is an enum; always valid.
            break;

        case Step::SelectWorkflows:
            if (!detail::has_any_workflow_selected(ctx)) {
                errs.push_back(Error::make(
                    ErrorCode::InvalidInput,
                    "At least one workflow must be selected."));
            }
            break;

        case Step::CheckExistingSecret:
            if (!ctx.use_existing_secret && !detail::is_valid_secret_name(ctx.secret_name)) {
                errs.push_back(Error::make(
                    ErrorCode::InvalidInput,
                    std::format("Secret name '{}' is invalid: must match "
                                "[A-Za-z0-9_]+.", ctx.secret_name)));
            }
            break;

        case Step::ApiKey:
            switch (ctx.selected_api_key_option) {
                case ApiKeyOption::ExistingLocal:
                    if (!detail::is_plausible_api_key(ctx.local_api_key)) {
                        errs.push_back(Error::make(
                            ErrorCode::InvalidInput,
                            "No valid local API key is available. "
                            "Choose another option."));
                    }
                    break;
                case ApiKeyOption::NewManual:
                    if (!detail::is_plausible_api_key(ctx.manual_api_key)) {
                        errs.push_back(Error::make(
                            ErrorCode::InvalidInput,
                            "The API key does not look valid "
                            "(expected sk-ant- prefix, 40+ chars)."));
                    }
                    break;
                case ApiKeyOption::OAuthFlow:
                    // No client-side validation; OAuth handles it.
                    break;
            }
            break;

        case Step::OAuthFlow:
            if (!ctx.oauth_completed) {
                errs.push_back(Error::make(
                    ErrorCode::InvalidInput,
                    "OAuth flow has not completed yet."));
            } else if (!ctx.oauth_error.empty()) {
                errs.push_back(Error::make(
                    ErrorCode::AuthenticationFailed,
                    std::format("OAuth failed: {}", ctx.oauth_error)));
            } else if (ctx.oauth_access_token.empty()) {
                errs.push_back(Error::make(
                    ErrorCode::AuthenticationFailed,
                    "OAuth completed but no access token was received."));
            }
            break;

        case Step::Creating:
            if (ctx.progress_steps.empty()) {
                errs.push_back(Error::make(ErrorCode::InternalError,
                    "Creating step has an empty progress plan."));
            }
            break;

        case Step::Success:
        case Step::Error:
            // Terminal – nothing to validate.
            break;
    }

    return errs;
}

// ============================================================
// advance(step, ctx, user_input) -> Step
//
// Returns the next step.  For auto-run steps (CheckGitHub, Creating) the
// user_input is ignored but the caller must have populated the relevant
// fields on `ctx` from the side-effects it performed.
//
// IO side-effect contract – the CALLER is responsible for:
//   * CheckGitHub: run `gh --version`, `gh auth status`, `gh auth token`,
//                  parse scopes; write into ctx.*gh_* and preflight_warnings.
//   * InstallApp:   open browser to https://github.com/apps/claude/installations/new
//   * CheckExistingWorkflow: `ls .github/workflows/claude*.yml` style check.
//   * CheckExistingSecret:   `gh secret list -R <repo>` (filter names).
//   * ApiKey (ExistingLocal branch): read key from user config on disk.
//   * OAuthFlow:  start AuthCodeListener, build authorize URL, open browser,
//                 call wait_for_callback(), exchange code for token, write
//                 oauth_access_token / oauth_completed / oauth_error.
//   * Creating:   execute each ProgressStep (gh secret set, write workflow
//                 yml files to disk, commit if requested, push).
// ============================================================
[[nodiscard]] Step advance(Step step,
                                  const InstallGitHubAppContext& ctx,
                                  std::string_view user_input) {
    (void)user_input;

    switch (step) {
        case Step::CheckGitHub: {
            // Auto-step.  Blocking problems (gh missing) are in validate().
            if (!ctx.preflight_warnings.empty()) return Step::Warnings;
            // All clear: jump straight to repo choice.
            return Step::ChooseRepo;
        }

        case Step::Warnings: {
            // User has acknowledged warnings (Enter pressed).
            return Step::ChooseRepo;
        }

        case Step::ChooseRepo: {
            // validate() ensures repo() is non-empty.
            // Even on the "chosen repo" path we re-check preflight because
            // gh scope warnings may have been emitted during ChooseRepo
            // secret-list probing too.
            if (!ctx.preflight_warnings.empty()
                && !ctx.gh_authenticated == false /* placeholder for eager checks */) {
                return Step::Warnings;
            }
            return Step::InstallApp;
        }

        case Step::InstallApp: {
            // Browser opened by caller.  Next action depends on whether a
            // workflow file already exists in the target repo.
            if (ctx.workflow_exists) return Step::CheckExistingWorkflow;
            return Step::SelectWorkflows;
        }

        case Step::CheckExistingWorkflow: {
            switch (ctx.workflow_action) {
                case WorkflowAction::Exit:   return Step::Error;  // user bailed
                case WorkflowAction::Skip:
                    // Skip workflow mutation; still need to set a secret.
                    break;
                case WorkflowAction::Update:
                    // Flow continues through CheckExistingSecret below.
                    break;
            }
            // Secret check.
            if (ctx.secret_exists) return Step::CheckExistingSecret;
            if (!ctx.local_api_key.empty()) {
                // Local key is available – ApiKey step with pre-selected reuse.
                return Step::ApiKey;
            }
            return Step::ApiKey;
        }

        case Step::SelectWorkflows: {
            if (ctx.secret_exists) return Step::CheckExistingSecret;
            if (!ctx.local_api_key.empty()) return Step::ApiKey;
            return Step::ApiKey;
        }

        case Step::CheckExistingSecret: {
            // "Reuse" or "new name" – both land in Creating with the
            // chosen auth credentials (validate ensures the name is ok).
            return Step::Creating;
        }

        case Step::ApiKey: {
            switch (ctx.selected_api_key_option) {
                case ApiKeyOption::OAuthFlow:    return Step::OAuthFlow;
                case ApiKeyOption::ExistingLocal:
                case ApiKeyOption::NewManual:
                    // Credentials are on the context; check if we still need
                    // the user to pick a secret name.
                    if (ctx.secret_exists) return Step::CheckExistingSecret;
                    return Step::Creating;
            }
            return Step::ApiKey; // unreachable, silence compiler
        }

        case Step::OAuthFlow: {
            // validate() ensures token is present when we reach here.
            return Step::Creating;
        }

        case Step::Creating: {
            // Caller updated progress_steps.  Any failure -> Error screen.
            bool any_failed = std::ranges::any_of(
                ctx.progress_steps,
                [](const auto& p) { return p.state == InstallGitHubAppContext::ProgressStep::State::Failed; });
            if (any_failed) return Step::Error;
            return Step::Success;
        }

        case Step::Success:
        case Step::Error:
            // Terminal – advance() loops back to itself; caller should exit.
            return step;
    }
    return step; // unreachable
}

// ============================================================
// step_description(step) -> std::string_view
//
// Short human-readable headline shown in the wizard header / breadcrumbs.
// ============================================================
[[nodiscard]] std::string_view step_description(Step step) noexcept {
    switch (step) {
        case Step::CheckGitHub:          return "Checking GitHub CLI";
        case Step::Warnings:             return "Preflight warnings";
        case Step::ChooseRepo:           return "Choose target repository";
        case Step::InstallApp:           return "Install the Claude GitHub App";
        case Step::CheckExistingWorkflow:return "Existing workflow detected";
        case Step::SelectWorkflows:      return "Select workflow files";
        case Step::CheckExistingSecret:  return "Repository secret already exists";
        case Step::ApiKey:               return "Provide an API key";
        case Step::OAuthFlow:            return "Sign in with GitHub OAuth";
        case Step::Creating:             return "Configuring repository…";
        case Step::Success:              return "Installation complete";
        case Step::Error:                return "Installation failed";
    }
    return "Unknown";
}

} // namespace cc::commands::install_github_app
