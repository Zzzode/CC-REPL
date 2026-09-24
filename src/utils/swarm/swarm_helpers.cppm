// C++23 Swarm Helpers Module
// Merges: teamHelpers.ts, teammateInit.ts, teammateLayoutManager.ts,
//         teammateModel.ts, teammatePromptAddendum.ts, spawnUtils.ts,
//         spawnInProcess.ts, inProcessRunner.ts, leaderPermissionBridge.ts,
//         permissionSync.ts, reconnection.ts, constants.ts
module;

#include <cstdlib>
#include <cstddef>
#include <cstdint>

export module cc.utils.swarm_helpers;

import std;

import cc.utils.swarm_backends;
import cc.utils.team_helpers;
import cc.utils.json;

export namespace cc::utils::swarm_helpers {

namespace fs = std::filesystem;
using namespace cc::utils::swarm_backends;

// ============================================================================
// Constants (from constants.ts)
// ============================================================================

/// Default team lead name
inline constexpr std::string_view TEAM_LEAD_NAME = "team-lead";

/// Environment variable to override teammate spawn command
inline constexpr std::string_view TEAMMATE_COMMAND_ENV_VAR = "LOOM_TEAMMATE_COMMAND";

/// Environment variable for teammate color assignment
inline constexpr std::string_view TEAMMATE_COLOR_ENV_VAR = "LOOM_AGENT_COLOR";

/// Environment variable to require plan mode before implementation
inline constexpr std::string_view PLAN_MODE_REQUIRED_ENV_VAR = "LOOM_PLAN_MODE_REQUIRED";

// ============================================================================
// Teammate Prompt Addendum (from teammatePromptAddendum.ts)
// ============================================================================

/// System prompt addendum appended for teammates in a swarm.
/// Explains visibility constraints and communication requirements.
inline constexpr std::string_view TEAMMATE_SYSTEM_PROMPT_ADDENDUM = R"(
# Agent Teammate Communication

IMPORTANT: You are running as an agent in a team. To communicate with anyone on your team:
- Use the SendMessage tool with `to: "<name>"` to send messages to specific teammates
- Use the SendMessage tool with `to: "*"` sparingly for team-wide broadcasts

Just writing a response in text is not visible to others on your team - you MUST use the SendMessage tool.

The user interacts primarily with the team lead. Your work is coordinated through the task system and teammate messaging.
)";

// ============================================================================
// Types (from teamHelpers.ts)
// ============================================================================

/// Permission mode for teammate execution
enum class PermissionMode {
    Default,
    BypassPermissions,
    AcceptEdits,
};

/// Path allowed for all team members without asking
struct TeamAllowedPath {
    std::string path;           ///< Directory path (absolute)
    std::string tool_name;      ///< Tool this applies to (e.g., "Edit", "Write")
    std::string added_by;       ///< Agent name who added this rule
    int64_t added_at = 0;      ///< Timestamp when added
};

/// A member in a team file
struct TeamMember {
    std::string agent_id;
    std::string name;
    std::optional<std::string> agent_type;
    std::optional<std::string> model;
    std::optional<std::string> prompt;
    std::optional<std::string> color;
    bool plan_mode_required = false;
    int64_t joined_at = 0;
    std::string tmux_pane_id;
    std::string cwd;
    std::optional<std::string> worktree_path;
    std::optional<std::string> session_id;
    std::vector<std::string> subscriptions;
    std::optional<BackendType> backend_type;
    bool is_active = true;
    std::optional<PermissionMode> mode;
};

/// Team configuration file structure
struct TeamFile {
    std::string name;
    std::optional<std::string> description;
    int64_t created_at = 0;
    std::string lead_agent_id;
    std::optional<std::string> lead_session_id;
    std::vector<std::string> hidden_pane_ids;
    std::vector<TeamAllowedPath> team_allowed_paths;
    std::vector<TeamMember> members;
};

/// Result of spawning a team
struct SpawnTeamOutput {
    std::string team_name;
    std::string team_file_path;
    std::string lead_agent_id;
};

/// Result of team cleanup
struct CleanupOutput {
    bool success = false;
    std::string message;
    std::optional<std::string> team_name;
};

// ============================================================================
// TeamHelpers — Team file management (from teamHelpers.ts)
// ============================================================================

/// Manages team files and directory lifecycle.
/// Handles CRUD operations on team config.json, member management,
/// permission mode sync, and session cleanup.
class TeamHelpers {
public:
    /// Sanitize a name for use in tmux window names, worktree paths, file paths.
    /// Replaces non-alphanumeric characters with hyphens and lowercases.
    [[nodiscard]] static std::string sanitize_name(std::string_view name);

    /// Sanitize an agent name for use in agent IDs.
    /// Replaces '@' with '-' to prevent ambiguity in agentName@teamName format.
    [[nodiscard]] static std::string sanitize_agent_name(std::string_view name);

    /// Get path to a team's directory
    [[nodiscard]] static fs::path get_team_dir(std::string_view team_name);

    /// Get path to a team's config.json file
    [[nodiscard]] static fs::path get_team_file_path(std::string_view team_name);

    /// Read a team file (synchronous — for sync contexts)
    [[nodiscard]] static std::optional<TeamFile> read_team_file(std::string_view team_name);

    /// Write a team file (synchronous)
    static void write_team_file(std::string_view team_name, const TeamFile& team_file);

    /// Remove a teammate from team file by agent ID or name
    [[nodiscard]] static bool remove_teammate(
        std::string_view team_name,
        std::optional<std::string_view> agent_id = std::nullopt,
        std::optional<std::string_view> name = std::nullopt);

    /// Remove a member from team by pane ID (also removes from hidden pane list)
    [[nodiscard]] static bool remove_member_by_pane(
        std::string_view team_name, std::string_view pane_id);

    /// Remove a member from team by agent ID
    [[nodiscard]] static bool remove_member_by_agent_id(
        std::string_view team_name, std::string_view agent_id);

    /// Add a pane ID to the hidden panes list
    [[nodiscard]] static bool add_hidden_pane(
        std::string_view team_name, std::string_view pane_id);

    /// Remove a pane ID from the hidden panes list
    [[nodiscard]] static bool remove_hidden_pane(
        std::string_view team_name, std::string_view pane_id);

    /// Set a team member's permission mode
    [[nodiscard]] static bool set_member_mode(
        std::string_view team_name, std::string_view member_name,
        PermissionMode mode);

    /// Set multiple members' modes in a single atomic write
    [[nodiscard]] static bool set_multiple_member_modes(
        std::string_view team_name,
        const std::vector<std::pair<std::string, PermissionMode>>& updates);

    /// Set a member's active status (idle/working)
    static void set_member_active(
        std::string_view team_name, std::string_view member_name, bool is_active);

    /// Sync current teammate's mode to config.json so team lead sees it
    static void sync_teammate_mode(
        PermissionMode mode, std::optional<std::string_view> team_name_override = std::nullopt);

    /// Register a team for session cleanup (on exit)
    static void register_for_session_cleanup(std::string_view team_name) {
        std::lock_guard lock(cleanup_mutex_);
        session_created_teams_.insert(std::string(team_name));
    }

    /// Unregister a team from session cleanup
    static void unregister_from_session_cleanup(std::string_view team_name) {
        std::lock_guard lock(cleanup_mutex_);
        session_created_teams_.erase(std::string(team_name));
    }

    /// Clean up all teams created this session that weren't explicitly deleted
    static void cleanup_session_teams();

    /// Clean up team and task directories for a given team name
    static void cleanup_team_directories(std::string_view team_name);

private:
    /// Destroy a git worktree at the given path
    static void destroy_worktree(const fs::path& worktree_path);

    /// Kill orphaned pane-based teammates for a team
    static void kill_orphaned_panes(std::string_view team_name);

    static inline std::mutex cleanup_mutex_;
    static inline std::set<std::string> session_created_teams_;
};

// ============================================================================
// TeammateLayoutManager — Color and pane management (from teammateLayoutManager.ts)
// ============================================================================

/// Manages teammate color assignments and delegates pane operations
/// to the detected backend.
class TeammateLayoutManager {
public:
    /// Assign a unique color to a teammate (round-robin from palette).
    /// Colors are persistent per session.
    [[nodiscard]] static AgentColor assign_color(std::string_view teammate_id) {
        std::lock_guard lock(mutex_);
        auto it = color_assignments_.find(std::string(teammate_id));
        if (it != color_assignments_.end()) {
            return it->second;
        }
        static constexpr AgentColor palette[] = {
            AgentColor::Red, AgentColor::Blue, AgentColor::Green,
            AgentColor::Yellow, AgentColor::Purple, AgentColor::Orange,
            AgentColor::Pink, AgentColor::Cyan,
        };
        auto color = palette[color_index_ % 8];
        color_assignments_[std::string(teammate_id)] = color;
        ++color_index_;
        return color;
    }

    /// Get the assigned color for a teammate, if any
    [[nodiscard]] static std::optional<AgentColor> get_color(std::string_view teammate_id) {
        std::lock_guard lock(mutex_);
        auto it = color_assignments_.find(std::string(teammate_id));
        if (it != color_assignments_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /// Clear all teammate color assignments (called during team cleanup)
    static void clear_colors() {
        std::lock_guard lock(mutex_);
        color_assignments_.clear();
        color_index_ = 0;
    }

    /// Check if currently running inside tmux
    [[nodiscard]] static bool is_inside_tmux() {
        return EnvironmentDetection::is_inside_tmux();
    }

    /// Create a new teammate pane in the swarm view (delegates to detected backend)
    [[nodiscard]] static CreatePaneResult create_teammate_pane(
        std::string_view teammate_name, AgentColor color);

    /// Enable pane border status (delegates to detected backend)
    static void enable_pane_border_status(
        std::optional<std::string_view> window_target = std::nullopt,
        bool use_swarm_socket = false);

    /// Send a command to a specific pane (delegates to detected backend)
    static void send_command_to_pane(
        std::string_view pane_id, std::string_view command,
        bool use_swarm_socket = false);

private:
    static inline std::mutex mutex_;
    static inline std::map<std::string, AgentColor> color_assignments_;
    static inline size_t color_index_ = 0;
};

// ============================================================================
// TeammateModel — Model selection for teammates (from teammateModel.ts)
// ============================================================================

/// Provides model fallback selection for teammates.
class TeammateModel {
public:
    /// Get the hardcoded fallback model for teammates.
    /// When the user has never set teammateDefaultModel, new teammates use this.
    /// Must be provider-aware for Bedrock/Vertex/Foundry customers.
    [[nodiscard]] static std::string get_fallback_model();

    /// Set the provider-specific model ID table (called at startup)
    static void set_provider_config(
        std::string_view default_model_id) {
        std::lock_guard lock(mutex_);
        fallback_model_ = std::string(default_model_id);
    }

private:
    static inline std::mutex mutex_;
    static inline std::string fallback_model_;
};

// ============================================================================
// SpawnUtils — Shared teammate spawn utilities (from spawnUtils.ts)
// ============================================================================

/// Shared utilities for spawning teammates across different backends.
class SpawnUtils {
public:
    /// Get the command to use for spawning teammate processes.
    /// Uses TEAMMATE_COMMAND_ENV_VAR if set, otherwise the current executable.
    [[nodiscard]] static std::string get_teammate_command();

    /// Build CLI flags to propagate from current session to spawned teammates.
    /// Ensures teammates inherit permission mode, model, plugin config.
    struct InheritedFlagsOptions {
        bool plan_mode_required = false;
        std::optional<PermissionMode> permission_mode;
    };
    [[nodiscard]] static std::string build_inherited_cli_flags(
        const InheritedFlagsOptions& options);
    [[nodiscard]] static std::string build_inherited_cli_flags() {
        return build_inherited_cli_flags(InheritedFlagsOptions{});
    }

    /// Environment variables that must be forwarded to tmux-spawned teammates.
    /// Tmux may start a new login shell that doesn't inherit parent's env.
    [[nodiscard]] static std::string build_inherited_env_vars();

    /// List of env vars to forward to teammates
    static constexpr std::string_view TEAMMATE_ENV_VARS[] = {
        "LOOM_USE_BEDROCK",
        "LOOM_USE_VERTEX",
        "LOOM_USE_FOUNDRY",
        "ANTHROPIC_BASE_URL",
        "LOOM_CONFIG_DIR",
        "LOOM_REMOTE",
        "LOOM_REMOTE_MEMORY_DIR",
        "HTTPS_PROXY",
        "https_proxy",
        "HTTP_PROXY",
        "http_proxy",
        "NO_PROXY",
        "no_proxy",
        "SSL_CERT_FILE",
        "NODE_EXTRA_CA_CERTS",
        "REQUESTS_CA_BUNDLE",
        "CURL_CA_BUNDLE",
    };
};

// ============================================================================
// InProcessSpawner — In-process teammate spawning (from spawnInProcess.ts)
// ============================================================================

/// Configuration for spawning an in-process teammate
struct InProcessSpawnConfig {
    std::string name;           ///< Display name (e.g., "researcher")
    std::string team_name;
    std::string prompt;         ///< Initial prompt/task
    std::optional<std::string> color;
    bool plan_mode_required = false;
    std::optional<std::string> model;
};

/// Result from spawning an in-process teammate
struct InProcessSpawnOutput {
    bool success = false;
    std::string agent_id;       ///< Full agent ID (format: "name@team")
    std::optional<std::string> task_id;
    std::optional<std::string> error;
};

/// Handles creation and lifecycle of in-process teammates.
/// Unlike pane-based teammates, these share the same process
/// and use context isolation via thread-local storage.
class InProcessSpawner {
public:
    /// Spawn a new in-process teammate.
    /// Creates TeammateContext, registers task in app state, returns spawn result.
    [[nodiscard]] static InProcessSpawnOutput spawn(const InProcessSpawnConfig& config);

    /// Force-kill an in-process teammate by task ID.
    /// Aborts all async operations and updates task state.
    [[nodiscard]] static bool kill(std::string_view task_id);

private:
    static inline std::mutex mutex_;
};

// ============================================================================
// InProcessRunner — Agent execution loop for in-process teammates
// (from inProcessRunner.ts)
// ============================================================================

/// Identity for an in-process teammate execution
struct RunnerIdentity {
    std::string agent_id;
    std::string agent_name;
    std::string team_name;
    std::optional<AgentColor> color;
    bool plan_mode_required = false;
    std::string parent_session_id;
};

/// Configuration for starting an in-process teammate's agent loop
struct RunnerConfig {
    RunnerIdentity identity;
    std::string task_id;
    std::string prompt;
    std::optional<std::string> model;
    std::optional<std::string> system_prompt;
    std::string system_prompt_mode = "default";
    std::vector<std::string> allowed_tools;
    bool allow_permission_prompts = false;
};

/// Wraps the agent execution loop for in-process teammates.
/// Provides context isolation, progress tracking, idle notification,
/// plan mode approval flow, and cleanup on completion/abort.
class InProcessRunner {
public:
    /// Start the in-process teammate's agent execution loop (fire-and-forget).
    /// Runs asynchronously in a background context.
    static void start(const RunnerConfig& config);

private:
    /// Run the agent main loop with proper context isolation
    static void run_agent_loop(const RunnerConfig& config);
};

// ============================================================================
// LeaderPermissionBridge (from leaderPermissionBridge.ts)
// ============================================================================

/// Callback type for permission confirmation queue updates
using SetToolUseConfirmQueueFn = std::function<void()>;

/// Callback type for permission context updates
using SetToolPermissionContextFn = std::function<void()>;

/// Module-level bridge allowing the REPL to register its permission
/// queue setter for in-process teammates to use.
///
/// When an in-process teammate requests permissions, it uses the
/// standard ToolUseConfirm dialog rather than a worker badge.
class LeaderPermissionBridge {
public:
    /// Register the leader's tool use confirm queue setter
    static void register_confirm_queue(SetToolUseConfirmQueueFn setter) {
        std::lock_guard lock(mutex_);
        confirm_queue_setter_ = std::move(setter);
    }

    /// Get the registered confirm queue setter (may be null)
    [[nodiscard]] static SetToolUseConfirmQueueFn get_confirm_queue() {
        std::lock_guard lock(mutex_);
        return confirm_queue_setter_;
    }

    /// Unregister the confirm queue setter
    static void unregister_confirm_queue() {
        std::lock_guard lock(mutex_);
        confirm_queue_setter_ = nullptr;
    }

    /// Register the leader's permission context setter
    static void register_permission_context(SetToolPermissionContextFn setter) {
        std::lock_guard lock(mutex_);
        permission_context_setter_ = std::move(setter);
    }

    /// Get the registered permission context setter (may be null)
    [[nodiscard]] static SetToolPermissionContextFn get_permission_context() {
        std::lock_guard lock(mutex_);
        return permission_context_setter_;
    }

    /// Unregister the permission context setter
    static void unregister_permission_context() {
        std::lock_guard lock(mutex_);
        permission_context_setter_ = nullptr;
    }

private:
    static inline std::mutex mutex_;
    static inline SetToolUseConfirmQueueFn confirm_queue_setter_;
    static inline SetToolPermissionContextFn permission_context_setter_;
};

// ============================================================================
// PermissionSync — Synchronized permission prompts (from permissionSync.ts)
// ============================================================================
//
// Protocol shape is frozen to match the live TypeScript mailbox path
// (src/utils/teammateMailbox.ts createPermissionRequestMessage/
// createPermissionResponseMessage and src/utils/swarm/permissionSync.ts
// sendPermissionRequestViaMailbox/sendPermissionResponseViaMailbox).
// The older TS pending/resolved DIRECTORY protocol (~/.loom/teams/<t>/
// permissions/{pending,resolved}) is deliberately not ported: request payloads
// live only as the "text" envelope of messages in the existing mailbox tree
// ($LOOM_TEAM_RUNTIME_DIR/<sanitized team>/inboxes/<agent>.json).
//
// TODO(teams): sandbox_permission_request/response (permissionSync.ts:805-928)
// and plan_approval_request/response (teammateMailbox.ts:684-711) variants are
// deferred — no C++ worker-side sandbox network broker/plan gate emits them
// yet; those messages use the same inbox envelope when added.

/// Permission request message envelope (worker -> leader).
/// Field names match the SDK `can_use_tool` snake_case shape.
struct SwarmPermissionRequestMessage {
    std::string type;        ///< Always "permission_request"
    std::string request_id;
    std::string agent_id;    ///< Worker NAME (mailbox routing uses names)
    std::string tool_name;
    std::string tool_use_id;
    std::string description;
    std::string input_json;  ///< Verbatim JSON object text for "input"
};

/// Permission response message envelope (leader -> worker).
struct SwarmPermissionResponseMessage {
    std::string type;  ///< Always "permission_response"
    std::string request_id;
    std::string subtype;  ///< "success" | "error"
    std::optional<std::string> error;
    std::optional<std::string> updated_input_json;
    /// Verbatim JSON array of SDK PermissionUpdate objects. The leader sends
    /// an addRules update when the user chooses "Always allow"; the worker
    /// persists those rules and auto-allows matching future tool calls
    /// without a mailbox round-trip.
    std::optional<std::string> permission_updates_json;
};

/// Static worker/leader entry points for the mailbox permission protocol.
/// All methods are free-of-instance state apart from a process-local consumed
/// response registry; stage C (leader TUI) calls the request parser and
/// send_response_to_worker, while the worker tool-permission hook calls
/// request_and_await on its query thread.
class PermissionSync {
public:
    /// "perm-<unixms>-<7 base36 chars>" (permissionSync.ts generateRequestId).
    [[nodiscard]] static std::string generate_request_id();

    /// Serialize a request to the exact JSON text stored in the mailbox
    /// envelope. The input object is embedded verbatim (brace-checked with a
    /// {} fallback); the engine already produced it as parsed tool_use JSON.
    [[nodiscard]] static std::string build_request_text(
        const SwarmPermissionRequestMessage& request);

    /// Serialize a response: success -> {"response":{}}, error -> {"error":...}.
    [[nodiscard]] static std::string build_response_text(
        const SwarmPermissionResponseMessage& response);

    /// Parse a request envelope; nullopt on type mismatch/missing request_id.
    [[nodiscard]] static std::optional<SwarmPermissionRequestMessage> parse_request(
        std::string_view text);

    /// Parse a response envelope; nullopt on type mismatch/missing request_id.
    [[nodiscard]] static std::optional<SwarmPermissionResponseMessage> parse_response(
        std::string_view text);

    /// Worker -> leader mailbox ("team-lead"). Returns false on write failure.
    static bool send_request_to_leader(
        const SwarmPermissionRequestMessage& request,
        std::string_view team_name);

    /// Leader -> worker mailbox; envelope from is "team-lead".
    static bool send_response_to_worker(
        std::string_view worker_name,
        const SwarmPermissionResponseMessage& response,
        std::string_view team_name);

    /// One non-blocking scan of the worker's own inbox. Consumes (removes) the
    /// matching response exactly once, accepting both read and unread messages
    /// because the general 1.5s inbox worker may mark them read first.
    [[nodiscard]] static std::optional<SwarmPermissionResponseMessage> poll_response(
        std::string_view worker_name,
        std::string_view team_name,
        std::string_view request_id);

    /// Blocking worker entry point (runs on the query/tool thread). Times out
    /// to a DENY (nullopt): TS waits forever, but a headless port must fail
    /// closed instead of wedging a worker on an unattended leader. Tune with
    /// LOOM_PERMISSION_TIMEOUT_MS (default 300000ms).
    [[nodiscard]] static std::optional<SwarmPermissionResponseMessage> request_and_await(
        const SwarmPermissionRequestMessage& request,
        std::string_view team_name,
        std::chrono::milliseconds timeout = default_timeout(),
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250));

    /// Env LOOM_PERMISSION_TIMEOUT_MS, else 300000ms (fail-closed default).
    [[nodiscard]] static std::chrono::milliseconds default_timeout();
};

// ============================================================================
// TeammateInit — Initialization hooks for teammates (from teammateInit.ts)
// ============================================================================

/// Team info required for teammate initialization
struct TeammateInitInfo {
    std::string team_name;
    std::string agent_id;
    std::string agent_name;
};

/// Handles initialization for instances running as teammates in a swarm.
/// Registers a Stop hook to notify the team leader when teammate becomes idle.
class TeammateInit {
public:
    /// Initialize hooks for a teammate running in a swarm.
    /// Registers a Stop hook that sends idle notification to team leader.
    static void initialize_hooks(
        std::string_view session_id, const TeammateInitInfo& info);

    /// Apply team-wide allowed paths from team file
    static void apply_team_allowed_paths(const TeamFile& team_file);
};

// ============================================================================
// SwarmReconnection — Context restoration for resumed sessions
// (from reconnection.ts)
// ============================================================================

/// Team context computed at session startup
struct InitialTeamContext {
    std::string team_name;
    std::string team_file_path;
    std::string lead_agent_id;
    bool is_leader = false;
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_name;
};

/// Handles initialization of swarm context for teammates.
/// - Fresh spawns: Initialize from CLI args
/// - Resumed sessions: Initialize from stored teamName/agentName
class SwarmReconnection {
public:
    /// Compute initial team context for AppState.
    /// Called synchronously at startup before first render.
    [[nodiscard]] static std::optional<InitialTeamContext> compute_initial_context();

    /// Set dynamic team context from CLI args (called during arg parsing)
    static void set_dynamic_context(
        std::string_view team_name, std::string_view agent_id,
        std::string_view agent_name) {
        std::lock_guard lock(mutex_);
        dynamic_team_name_ = std::string(team_name);
        dynamic_agent_id_ = std::string(agent_id);
        dynamic_agent_name_ = std::string(agent_name);
    }

    /// Check if dynamic context has been set
    [[nodiscard]] static bool has_dynamic_context() {
        std::lock_guard lock(mutex_);
        return !dynamic_team_name_.empty() && !dynamic_agent_name_.empty();
    }

private:
    static inline std::mutex mutex_;
    static inline std::string dynamic_team_name_;
    static inline std::string dynamic_agent_id_;
    static inline std::string dynamic_agent_name_;
};

// ── Static-member stub implementations ──────────────────────────────────────
// Defined out-of-line so translation units importing this module can link.
std::string SpawnUtils::get_teammate_command() {
    if (const char* v = std::getenv("TEAMMATE_COMMAND")) return std::string(v);
    return "loom";  // fallback: own executable name
}

std::string SpawnUtils::build_inherited_cli_flags(const InheritedFlagsOptions& opts) {
    std::string out;
    if (opts.plan_mode_required) out += " --plan";
    if (opts.permission_mode) {
        out += " --permission-mode ";
        switch (*opts.permission_mode) {
            case PermissionMode::Default: out += "default"; break;
            case PermissionMode::BypassPermissions: out += "bypass"; break;
            case PermissionMode::AcceptEdits: out += "accept-edits"; break;
        }
    }
    return out;
}

std::string SpawnUtils::build_inherited_env_vars() { return {}; }

// ── PermissionSync mailbox protocol implementation ─────────────────────────

namespace permission_detail {

/// Same escaping switch as team_helpers detail::mailbox_json_escape; kept
/// local so this protocol module does not couple to that detail namespace.
[[nodiscard]] inline std::string json_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    static const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            default:
                // Escape every other C0 control byte so a tool-generated
                // description can't emit invalid JSON that bricks the inbox.
                if (ch < 0x20) {
                    out += R"(\u00)";
                    out.push_back(hex[(ch >> 4) & 0x0F]);
                    out.push_back(hex[ch & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

[[nodiscard]] inline std::string unix_millis_now() {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(millis);
}

/// A usable tool input must be a JSON object. The engine hands us the model's
/// already-parsed tool_use JSON, so a brace check is sufficient; anything
/// suspicious is replaced with an empty object rather than re-serialized.
[[nodiscard]] inline std::string normalize_input_json(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "{}";
    const auto last = input.find_last_not_of(" \t\r\n");
    if (input[first] != '{' || input[last] != '}') return "{}";
    return input;
}

/// permission_updates must be a JSON array.
[[nodiscard]] inline std::string normalize_updates_json(const std::string& json) {
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "[]";
    const auto last = json.find_last_not_of(" \t\r\n");
    if (json[first] != '[' || json[last] != ']') return "[]";
    return json;
}

/// Rewrite an inbox with one exact-text message removed (TS legacy
/// removeWorkerResponse semantics: the blocking waiter deletes the response it
/// consumed so a slow later poll can never redeliver it). Mirrors the
/// read/rewrite pattern of cc::utils::mark_all_read; the envelope serializer
/// is replicated here because team_helpers' detail::write_messages is not
/// exported from that module.
inline bool remove_mailbox_message_by_text(
    std::string_view worker_name,
    std::string_view team_name,
    std::string_view exact_text
) {
    // Share the inbox RMW lock with write_to_mailbox/mark_all_read so the
    // worker's response consumption cannot race the poller's mark-all-read.
    std::lock_guard<std::mutex> lock(cc::utils::teammate_inbox_mutex());
    const auto inbox_path =
        fs::path{cc::utils::get_inbox_path(worker_name, team_name)};
    // Serialize against the leader and other pane PROCESSES as well.
    cc::utils::ScopedInboxLock flock(inbox_path);
    if (!flock.locked()) return false;
    auto messages = cc::utils::read_inbox(worker_name, team_name);
    if (!messages) return false;
    const auto before = messages->size();
    std::erase_if(*messages, [&](const cc::utils::TeammateMessage& message) {
        return message.text == exact_text;
    });
    if (messages->size() == before) return false;

    std::error_code ec;
    fs::create_directories(inbox_path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(inbox_path, std::ios::trunc);
    if (!out) return false;
    out << '[';
    for (std::size_t i = 0; i < messages->size(); ++i) {
        const auto& message = (*messages)[i];
        if (i != 0) out << ',';
        out << R"({"from":")" << json_quote(message.from)
            << R"(","text":")" << json_quote(message.text)
            << R"(","timestamp":")" << json_quote(message.timestamp)
            << R"(","read":)" << (message.read ? "true" : "false");
        if (message.color) {
            out << R"(,"color":")" << json_quote(*message.color) << '"';
        }
        if (message.summary) {
            out << R"(,"summary":")" << json_quote(*message.summary) << '"';
        }
        out << '}';
    }
    out << ']';
    return out.good();
}

} // namespace permission_detail
// ============================================================================
// Worker-side "Always allow" grant persistence
// ============================================================================
//
// When the leader resolves a request with AlwaysAllow, the success envelope
// carries an SDK PermissionUpdate (addRules / behavior "allow"). The worker
// persists those rules and auto-allows matching future calls itself — a
// background pane must not re-prompt the leader for the same tool on every
// turn. The grant store lives in the shared team runtime dir so it survives
// pane process restarts/reconnection.
//
// Conservative enforcement: only whole-tool grants (no ruleContent) auto-
// allow; content-scoped rules like Bash(npm install) are persisted but NOT
// matched here — the worker keeps asking the leader rather than risk a
// half-implemented command matcher granting too much.

/// One flattened allow rule extracted from a permission_updates array.
struct WorkerAllowRule {
    std::string tool_name;
    std::string rule_content;  // empty => whole tool
};

/// Build the permission_updates JSON array for one whole-tool AlwaysAllow
/// decision. Destination is "session": pane workers are ephemeral and the
/// grant store below is the real persistence; we never touch settings files.
[[nodiscard]] inline std::string build_always_allow_updates_json(
    std::string_view tool_name) {
    // json_quote escapes string CONTENT (the wrapping quotes are literal in
    // the fragments, matching every other envelope builder in this file).
    std::string text =
        R"([{"type":"addRules","destination":"session","behavior":"allow","rules":[{"toolName":")";
    text += permission_detail::json_quote(tool_name);
    text += R"("}]}])";
    return text;
}

/// Render a worker's tool input for the leader's approval dialog: Edit-like
/// inputs (old_string/new_string) get a compact -/+ diff view, everything
/// else gets pretty-printed JSON. Capped so a huge edit body cannot flood
/// the overlay. Shared with tests to keep the dialog formatting stable.
[[nodiscard]] inline std::string format_permission_request_input(
    std::string_view /*tool_name*/,
    std::string_view input_json,
    std::size_t max_chars = 2000) {
    auto parsed = cc::utils::json::parse(std::string(input_json));
    std::string rendered;
    if (parsed && parsed->root().is_obj()) {
        const auto root = parsed->root();
        const auto old_s = root.get("old_string");
        const auto new_s = root.get("new_string");
        const auto path_s = root.get("file_path");
        if (old_s.is_str() && new_s.is_str()) {
            if (path_s.is_str()) {
                rendered += std::string(path_s.as_str());
                rendered += '\n';
            }
            auto split_lines = [](std::string_view text) {
                std::vector<std::string_view> lines;
                std::size_t start = 0;
                while (start <= text.size()) {
                    const auto nl = text.find('\n', start);
                    lines.push_back(text.substr(
                        start,
                        nl == std::string_view::npos ? text.size() - start
                                                     : nl - start));
                    if (nl == std::string_view::npos) break;
                    start = nl + 1;
                }
                return lines;
            };
            for (const auto line : split_lines(old_s.as_str())) {
                rendered += "- ";
                rendered += line;
                rendered += '\n';
            }
            for (const auto line : split_lines(new_s.as_str())) {
                rendered += "+ ";
                rendered += line;
                rendered += '\n';
            }
        } else {
            rendered = cc::utils::json::to_pretty_string(*parsed);
        }
    } else {
        rendered = std::string(input_json);
    }
    if (rendered.size() > max_chars) {
        rendered.resize(max_chars);
        rendered += "\n... [truncated]";
    }
    return rendered;
}

/// Process-local cache + on-disk store of grants received from the leader.
/// Instantiated per worker permission check (the store is cheap; reads are
/// file-based and flocked, matching the mailbox protocol's trust model).
class WorkerPermissionGrants {public:
    WorkerPermissionGrants(std::string team_name, std::string agent_name)
        : team_name_(std::move(team_name)),
          agent_name_(std::move(agent_name)) {}

    /// True when a whole-tool allow rule covers this exact tool name.
    [[nodiscard]] bool allows(std::string_view tool_name) {
        for (const auto& rule : load_rules()) {
            if (rule.rule_content.empty() && rule.tool_name == tool_name) {
                return true;
            }
        }
        return false;
    }

    /// Persist every addRules/allow update in a verbatim permission_updates
    /// JSON array. Unknown/non-allow update shapes are ignored (the worker
    /// has no settings destinations other than its own grant file).
    void apply_updates(std::string_view updates_json) {
        auto parsed = cc::utils::json::parse(std::string(updates_json));
        if (!parsed) return;
        const auto root = parsed->root();
        if (!root.is_arr()) return;

        std::vector<WorkerAllowRule> rules = load_rules();
        std::size_t added = 0;
        root.iter([&](cc::utils::json::JsonVal update) {
            if (!update.is_obj()) return;
            if (update.get_string("type") != "addRules") return;
            if (update.get_string("behavior") != "allow") return;
            const auto rules_node = update.get("rules");
            if (!rules_node.is_arr()) return;
            rules_node.iter([&](cc::utils::json::JsonVal rule) {
                if (!rule.is_obj()) return;
                const auto name = rule.get("toolName");
                if (!name.is_str() || name.as_str().empty()) return;
                WorkerAllowRule candidate{
                    .tool_name = std::string(name.as_str()),
                    .rule_content = {},
                };
                if (const auto content = rule.get("ruleContent");
                    content.is_str()) {
                    candidate.rule_content = std::string(content.as_str());
                }
                const bool duplicate = std::ranges::any_of(
                    rules, [&](const WorkerAllowRule& existing) {
                        return existing.tool_name == candidate.tool_name &&
                               existing.rule_content == candidate.rule_content;
                    });
                if (!duplicate) {
                    rules.push_back(std::move(candidate));
                    ++added;
                }
            });
        });
        if (added > 0) save_rules(rules);
    }

private:
    [[nodiscard]] fs::path grants_path() const {
        const auto team = cc::utils::team_dir(team_name_);
        return fs::path{team} / "permissions" /
               ("worker-allow-" +
                cc::utils::detail::sanitize_path_component(agent_name_, "agent") +
                ".json");
    }

    [[nodiscard]] std::vector<WorkerAllowRule> load_rules() {
        std::vector<WorkerAllowRule> rules;
        const auto path = grants_path();
        std::error_code ec;
        if (!fs::exists(path, ec)) return rules;
        auto parsed = cc::utils::json::parse_file(path);
        if (!parsed) return rules;
        const auto list = parsed->root().get("rules");
        if (!list.is_arr()) return rules;
        list.iter([&](cc::utils::json::JsonVal rule) {
            if (!rule.is_obj()) return;
            const auto name = rule.get("tool_name");
            if (!name.is_str()) return;
            WorkerAllowRule entry{};
            entry.tool_name = std::string(name.as_str());
            if (const auto content = rule.get("rule_content");
                content.is_str()) {
                entry.rule_content = std::string(content.as_str());
            }
            rules.push_back(std::move(entry));
        });
        return rules;
    }

    void save_rules(const std::vector<WorkerAllowRule>& rules) {
        const auto path = grants_path();
        cc::utils::ScopedFileLock lock(path);
        if (!lock.locked()) return;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out) return;
        out << "{\"rules\":[";
        for (std::size_t i = 0; i < rules.size(); ++i) {
            if (i != 0) out << ',';
            out << R"({"tool_name":")"
                << permission_detail::json_quote(rules[i].tool_name)
                << R"(","rule_content":")"
                << permission_detail::json_quote(rules[i].rule_content)
                << "\"}";
        }
        out << "]}";
    }

    std::string team_name_;
    std::string agent_name_;
};

inline std::string PermissionSync::generate_request_id() {
    static constexpr std::string_view alphabet =
        "abcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937 rng(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string suffix(7, 'a');
    for (char& ch : suffix) {
        ch = alphabet[static_cast<std::size_t>(rng()) % alphabet.size()];
    }
    return "perm-" + permission_detail::unix_millis_now() + "-" + suffix;
}

inline std::string PermissionSync::build_request_text(
    const SwarmPermissionRequestMessage& request
) {
    std::string text;
    text += "{\"type\":\"permission_request\",\"request_id\":\"";
    text += permission_detail::json_quote(request.request_id);
    text += "\",\"agent_id\":\"";
    text += permission_detail::json_quote(request.agent_id);
    text += "\",\"tool_name\":\"";
    text += permission_detail::json_quote(request.tool_name);
    text += "\",\"tool_use_id\":\"";
    text += permission_detail::json_quote(request.tool_use_id);
    text += "\",\"description\":\"";
    text += permission_detail::json_quote(request.description);
    text += "\",\"input\":";
    text += permission_detail::normalize_input_json(request.input_json);
    text += ",\"permission_suggestions\":[]}";
    return text;
}

inline std::string PermissionSync::build_response_text(
    const SwarmPermissionResponseMessage& response
) {
    std::string text = "{\"type\":\"permission_response\",\"request_id\":\"";
    text += permission_detail::json_quote(response.request_id);
    text += "\",\"subtype\":\"";
    text += permission_detail::json_quote(response.subtype);
    text += '"';
    if (response.subtype == "error") {
        text += ",\"error\":\"";
        text += permission_detail::json_quote(
            response.error.value_or("Permission denied"));
        text += '"';
    } else {
        text += ",\"response\":{";
        bool need_comma = false;
        // The generic C++ dialog never edits input, so this is normally
        // omitted (JSON.stringify drops undefined in TS); serialize it only
        // when a caller explicitly supplies a replacement object.
        if (response.updated_input_json) {
            text += "\"updated_input\":";
            text += permission_detail::normalize_input_json(*response.updated_input_json);
            need_comma = true;
        }
        // Carries "Always allow" addRules updates so the worker persists the
        // grant and skips the mailbox round-trip on matching later calls.
        if (response.permission_updates_json) {
            if (need_comma) text += ',';
            text += "\"permission_updates\":";
            text += permission_detail::normalize_updates_json(
                *response.permission_updates_json);
        }
        text += '}';
    }
    text += '}';
    return text;
}

inline std::optional<SwarmPermissionRequestMessage> PermissionSync::parse_request(
    std::string_view text
) {
    auto parsed = cc::utils::json::parse(text);
    if (!parsed.has_value()) return std::nullopt;
    const auto root = parsed->root();
    if (!root.is_obj() || root.get_string("type") != "permission_request") {
        return std::nullopt;
    }
    const std::string request_id = root.get_string("request_id");
    if (request_id.empty()) return std::nullopt;

    SwarmPermissionRequestMessage request;
    request.type = "permission_request";
    request.request_id = request_id;
    request.agent_id = root.get_string("agent_id");
    request.tool_name = root.get_string("tool_name");
    request.tool_use_id = root.get_string("tool_use_id");
    request.description = root.get_string("description");
    const auto input = root.get("input");
    request.input_json = input.is_obj() ? input.to_string() : std::string{"{}"};
    return request;
}

inline std::optional<SwarmPermissionResponseMessage> PermissionSync::parse_response(
    std::string_view text
) {
    auto parsed = cc::utils::json::parse(text);
    if (!parsed.has_value()) return std::nullopt;
    const auto root = parsed->root();
    if (!root.is_obj() || root.get_string("type") != "permission_response") {
        return std::nullopt;
    }
    const std::string request_id = root.get_string("request_id");
    if (request_id.empty()) return std::nullopt;
    const std::string subtype = root.get_string("subtype");
    if (subtype != "success" && subtype != "error") return std::nullopt;

    SwarmPermissionResponseMessage response;
    response.type = "permission_response";
    response.request_id = request_id;
    response.subtype = subtype;
    if (subtype == "error") {
        const auto error = root.get("error");
        if (error.is_str()) response.error = std::string(error.as_str());
    } else {
        if (const auto body = root.get_object("response")) {
            const auto updated_input = body->get("updated_input");
            if (updated_input.is_obj()) {
                response.updated_input_json = updated_input.to_string();
            }
            const auto updates = body->get("permission_updates");
            if (updates.is_arr() && updates.size() > 0) {
                response.permission_updates_json = updates.to_string();
            }
        }
    }
    return response;
}

inline bool PermissionSync::send_request_to_leader(
    const SwarmPermissionRequestMessage& request,
    std::string_view team_name
) {
    cc::utils::TeammateMessage message;
    message.from = request.agent_id;
    message.text = build_request_text(request);
    message.timestamp = permission_detail::unix_millis_now();
    return cc::utils::write_to_mailbox(
        TEAM_LEAD_NAME, std::move(message), team_name).has_value();
}

inline bool PermissionSync::send_response_to_worker(
    std::string_view worker_name,
    const SwarmPermissionResponseMessage& response,
    std::string_view team_name
) {
    cc::utils::TeammateMessage message;
    message.from = std::string(TEAM_LEAD_NAME);
    message.text = build_response_text(response);
    message.timestamp = permission_detail::unix_millis_now();
    return cc::utils::write_to_mailbox(
        worker_name, std::move(message), team_name).has_value();
}

inline std::optional<SwarmPermissionResponseMessage> PermissionSync::poll_response(
    std::string_view worker_name,
    std::string_view team_name,
    std::string_view request_id
) {
    auto messages = cc::utils::read_inbox(worker_name, team_name);
    if (!messages) return std::nullopt;

    // Process-local consumed registry; keyed by team so request ids never
    // collide across teams. Accepts read OR unread messages because the
    // general 1.5s inbox poller may have marked the response read first.
    static std::mutex consumed_mutex;
    static std::unordered_set<std::string> consumed;
    const std::string key =
        std::string(team_name) + "/" + std::string(request_id);

    std::optional<SwarmPermissionResponseMessage> found;
    std::string found_text;
    {
        std::lock_guard lock(consumed_mutex);
        if (consumed.find(key) != consumed.end()) return std::nullopt;
        for (const auto& message : *messages) {
            auto parsed = parse_response(message.text);
            if (!parsed || parsed->request_id != request_id) continue;
            consumed.insert(key);
            found = std::move(parsed);
            found_text = message.text;
            break;
        }
    }
    if (!found) return std::nullopt;

    // Delete the consumed envelope independently of the general inbox worker.
    permission_detail::remove_mailbox_message_by_text(
        worker_name, team_name, found_text);
    return found;
}

inline std::optional<SwarmPermissionResponseMessage> PermissionSync::request_and_await(
    const SwarmPermissionRequestMessage& request,
    std::string_view team_name,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval
) {
    // Deliver the request envelope before entering the deadline loop; a
    // mailbox write failure denies immediately (fail closed).
    if (!send_request_to_leader(request, team_name)) return std::nullopt;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto response = poll_response(
                request.agent_id, team_name, request.request_id)) {
            return response;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return std::nullopt;
}

inline std::chrono::milliseconds PermissionSync::default_timeout() {
    if (const char* value = std::getenv("LOOM_PERMISSION_TIMEOUT_MS");
        value && *value) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — single-threaded env at start
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        if (end != value && parsed > 0) {
            return std::chrono::milliseconds(parsed);
        }
    }
    return std::chrono::milliseconds(300000);
}

} // namespace cc::utils::swarm_helpers
