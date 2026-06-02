module;

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.background_remote;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// Status of a background remote session.
export enum class RemoteSessionStatus {
    Starting,
    Running,
    Completed,
    Failed,
    Killed,
};

/// Lightweight SDK message representation (subset of the TS SDKMessage).
export struct SdkMessage {
    std::string role;
    std::string content;
    std::int64_t timestamp{0};
};

/// A single todo item for the remote session's checklist.
export struct TodoItem {
    std::string id;
    std::string content;
    bool completed{false};
};

/// In-memory representation of a background remote session.
export struct BackgroundRemoteSession {
    std::string id;
    std::string command;
    std::int64_t start_time{0};
    RemoteSessionStatus status{RemoteSessionStatus::Starting};
    std::vector<TodoItem> todo_list;
    std::string title;
    std::vector<SdkMessage> log;
};

/// Discriminated precondition failure types.
export enum class PreconditionFailureType {
    NotLoggedIn,
    NoRemoteEnvironment,
    NotInGitRepo,
    NoGitRemote,
    GithubAppNotInstalled,
    PolicyBlocked,
};

/// A single precondition failure with its type tag.
export struct PreconditionFailure {
    PreconditionFailureType type;
};

/// Method by which a repository is accessed for remote operations.
export enum class RepoAccessMethod {
    GithubApp,
    TokenSync,
    None,
};

/// Result of checking repo access for remote operations.
export struct RepoAccessResult {
    bool has_access{false};
    RepoAccessMethod method{RepoAccessMethod::None};
};

/// Options controlling the eligibility check.
export struct EligibilityCheckOptions {
    bool skip_bundle{false};
};

// ---------------------------------------------------------------------------
// Precondition checks
// ---------------------------------------------------------------------------

/// Checks if user needs to log in with Claude.ai.
/// Returns true if login is required.
export auto check_needs_claude_ai_login()
    -> std::expected<bool, std::string>;

/// Checks if git working directory is clean (ignores untracked files).
export auto check_is_git_clean()
    -> std::expected<bool, std::string>;

/// Checks if user has access to at least one remote environment.
export auto check_has_remote_environment()
    -> std::expected<bool, std::string>;

/// Checks if the current directory is inside a git repository.
export auto check_is_in_git_repo() -> bool;

/// Checks if the current repository has a GitHub remote configured.
export auto check_has_git_remote()
    -> std::expected<bool, std::string>;

/// Checks if the GitHub app is installed on a specific repository.
export auto check_github_app_installed(
    std::string_view owner,
    std::string_view repo)
    -> std::expected<bool, std::string>;

/// Checks if the user has synced their GitHub credentials via /web-setup.
export auto check_github_token_synced()
    -> std::expected<bool, std::string>;

/// Tiered check for whether a GitHub repo is accessible for remote ops.
/// 1. GitHub App installed  2. Token synced  3. Neither.
export auto check_repo_for_remote_access(
    std::string_view owner,
    std::string_view repo)
    -> std::expected<RepoAccessResult, std::string>;

// ---------------------------------------------------------------------------
// Session eligibility
// ---------------------------------------------------------------------------

/// Checks eligibility for creating a background remote session.
/// Returns a (possibly empty) list of failed preconditions.
export auto check_background_remote_session_eligibility(
    EligibilityCheckOptions const& opts)
    -> std::expected<std::vector<PreconditionFailure>, std::string>;

/// Overload without options (uses defaults).
export auto check_background_remote_session_eligibility()
    -> std::expected<std::vector<PreconditionFailure>, std::string>;
