// Teleport API client, environment selection, git bundle transfer.
// Migrated from src/utils/teleport/*.ts
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.teleport_utils;

export namespace cc::utils::teleport {

using namespace std::chrono;

// =========================================================================
// api.ts — Types & API Client
// =========================================================================

/// Retry delays for teleport API requests (exponential backoff).
inline constexpr std::array kRetryDelays = {
    milliseconds{2000}, milliseconds{4000},
    milliseconds{8000}, milliseconds{16000}
};
inline constexpr std::size_t kMaxRetries = kRetryDelays.size();

/// CCR BYOC beta header value.
inline constexpr std::string_view kCcrByocBeta = "ccr-byoc-2025-07-29";

/// Session status from the Sessions API.
enum class SessionStatus {
    requires_action,
    running,
    idle,
    archived,
};

/// Git source in a session context.
struct GitSource {
    std::string url;
    std::optional<std::string> revision;
    bool allow_unrestricted_git_push{false};
};

/// Knowledge base source in a session context.
struct KnowledgeBaseSource {
    std::string knowledge_base_id;
};

/// A session context source (tagged union).
using SessionContextSource = std::variant<GitSource, KnowledgeBaseSource>;

/// Outcome git info.
struct OutcomeGitInfo {
    std::string repo;
    std::vector<std::string> branches;
};

/// Git repository outcome.
struct GitRepositoryOutcome {
    OutcomeGitInfo git_info;
};

/// GitHub PR reference.
struct GithubPr {
    std::string owner;
    std::string repo;
    int number;
};

/// Session context payload.
struct SessionContext {
    std::vector<SessionContextSource> sources;
    std::string cwd;
    std::vector<GitRepositoryOutcome> outcomes;
    std::optional<std::string> custom_system_prompt;
    std::optional<std::string> append_system_prompt;
    std::optional<std::string> model;
    std::optional<std::string> seed_bundle_file_id;
    std::optional<GithubPr> github_pr;
    bool reuse_outcome_branches{false};
};

/// Full session resource from the API.
struct SessionResource {
    std::string id;
    std::optional<std::string> title;
    SessionStatus session_status;
    std::string environment_id;
    std::string created_at;
    std::string updated_at;
    SessionContext session_context;
};

/// List sessions response.
struct ListSessionsResponse {
    std::vector<SessionResource> data;
    bool has_more;
    std::optional<std::string> first_id;
    std::optional<std::string> last_id;
};

/// Code session status.
enum class CodeSessionStatus {
    idle,
    working,
    waiting,
    completed,
    archived,
    cancelled,
    rejected,
};

/// Repository info within a code session.
struct CodeSessionRepo {
    std::string name;
    std::string owner_login;
    std::optional<std::string> default_branch;
};

/// Lightweight code session (transformed from SessionResource).
struct CodeSession {
    std::string id;
    std::string title;
    std::string description;
    CodeSessionStatus status;
    std::optional<CodeSessionRepo> repo;
    std::vector<std::string> turns;
    std::string created_at;
    std::string updated_at;
};

/// Content for a remote session message (plain string or structured blocks).
using RemoteMessageContent = std::variant<
    std::string,
    std::vector<std::string>  // simplified content blocks
>;

/// API credentials prepared for requests.
struct ApiCredentials {
    std::string access_token;
    std::string org_uuid;
};

/// Check if an error is a transient network error that should be retried.
[[nodiscard]] bool is_transient_network_error(int http_status, bool has_response);

/// Prepare API request credentials (access token + org UUID).
[[nodiscard]] std::expected<ApiCredentials, std::string> prepare_api_request();

/// Get OAuth headers for API requests.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
get_oauth_headers(std::string_view access_token);

/// Fetch code sessions from the Sessions API (/v1/sessions).
[[nodiscard]] std::expected<std::vector<CodeSession>, std::string>
fetch_code_sessions_from_sessions_api();

/// Fetch a single session by ID.
[[nodiscard]] std::expected<SessionResource, std::string>
fetch_session(std::string_view session_id);

/// Extract the first branch name from a session's git repository outcomes.
[[nodiscard]] std::optional<std::string>
get_branch_from_session(const SessionResource& session);

/// Send a user message event to an existing remote session.
[[nodiscard]] std::expected<bool, std::string>
send_event_to_remote_session(
    std::string_view session_id,
    const RemoteMessageContent& content);

/// Send a user message event with explicit UUID for dedup.
[[nodiscard]] std::expected<bool, std::string>
send_event_to_remote_session(
    std::string_view session_id,
    const RemoteMessageContent& content,
    std::string_view uuid);

/// Update the title of an existing remote session.
[[nodiscard]] std::expected<bool, std::string>
update_session_title(std::string_view session_id, std::string_view title);

// =========================================================================
// environments.ts — Environment Management
// =========================================================================

/// Environment kind.
enum class EnvironmentKind {
    anthropic_cloud,
    byoc,
    bridge,
};

/// Environment state.
enum class EnvironmentState {
    active,
};

/// Environment resource from the API.
struct EnvironmentResource {
    EnvironmentKind kind;
    std::string environment_id;
    std::string name;
    std::string created_at;
    EnvironmentState state;
};

/// Fetch available environments from the Environment API.
[[nodiscard]] std::expected<std::vector<EnvironmentResource>, std::string>
fetch_environments();

/// Create a default anthropic_cloud environment.
[[nodiscard]] std::expected<EnvironmentResource, std::string>
create_default_cloud_environment(std::string_view name);

// =========================================================================
// environmentSelection.ts — Environment Selection
// =========================================================================

/// Setting source identifier for environment selection tracking.
enum class SettingSource {
    user_settings,
    project_settings,
    enterprise_settings,
    flag_settings,
};

/// Information about environment selection state.
struct EnvironmentSelectionInfo {
    std::vector<EnvironmentResource> available_environments;
    std::optional<EnvironmentResource> selected_environment;
    std::optional<SettingSource> selected_environment_source;
};

/// Get info about available environments and the currently selected one.
[[nodiscard]] std::expected<EnvironmentSelectionInfo, std::string>
get_environment_selection_info();

// =========================================================================
// gitBundle.ts — Git Bundle Creation & Upload
// =========================================================================

/// Default maximum bundle size (100MB).
inline constexpr std::size_t kDefaultBundleMaxBytes = 100 * 1024 * 1024;

/// Bundle scope: how much of the repo was packed.
enum class BundleScope {
    all,       // --all (full refs)
    head,      // HEAD only (current branch history)
    squashed,  // single parentless commit (snapshot only)
};

/// Reason a bundle operation failed.
enum class BundleFailReason {
    git_error,
    too_large,
    empty_repo,
};

/// Successful bundle upload result.
struct BundleUploadSuccess {
    std::string file_id;
    std::size_t bundle_size_bytes;
    BundleScope scope;
    bool has_wip;
};

/// Failed bundle upload result.
struct BundleUploadFailure {
    std::string error;
    std::optional<BundleFailReason> fail_reason;
};

/// Result of createAndUploadGitBundle.
using BundleUploadResult = std::expected<BundleUploadSuccess, BundleUploadFailure>;

/// Configuration for the Files API upload.
struct FilesApiConfig {
    std::string access_token;
    std::string org_uuid;
    std::string base_url;
};

/// Create a git bundle of the repository and upload to the Files API.
/// Fallback chain: --all -> HEAD -> squashed-root.
/// Captures tracked WIP via stash create.
[[nodiscard]] BundleUploadResult create_and_upload_git_bundle(
    const FilesApiConfig& config);

/// Create a git bundle with explicit working directory and abort signal.
[[nodiscard]] BundleUploadResult create_and_upload_git_bundle(
    const FilesApiConfig& config,
    std::string_view cwd,
    std::function<bool()> should_abort);

} // namespace cc::utils::teleport
