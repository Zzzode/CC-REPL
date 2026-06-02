// C++23 Swarm Helpers Module
// Merges: teamHelpers.ts, teammateInit.ts, teammateLayoutManager.ts,
//         teammateModel.ts, teammatePromptAddendum.ts, spawnUtils.ts,
//         spawnInProcess.ts, inProcessRunner.ts, leaderPermissionBridge.ts,
//         permissionSync.ts, reconnection.ts, constants.ts
module;

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.swarm_helpers;

import cc.utils.swarm_backends;

export namespace cc::utils::swarm_helpers {

namespace fs = std::filesystem;
using namespace cc::utils::swarm_backends;

// ============================================================================
// Constants (from constants.ts)
// ============================================================================

/// Default team lead name
inline constexpr std::string_view TEAM_LEAD_NAME = "team-lead";

/// Environment variable to override teammate spawn command
inline constexpr std::string_view TEAMMATE_COMMAND_ENV_VAR = "CLAUDE_CODE_TEAMMATE_COMMAND";

/// Environment variable for teammate color assignment
inline constexpr std::string_view TEAMMATE_COLOR_ENV_VAR = "CLAUDE_CODE_AGENT_COLOR";

/// Environment variable to require plan mode before implementation
inline constexpr std::string_view PLAN_MODE_REQUIRED_ENV_VAR = "CLAUDE_CODE_PLAN_MODE_REQUIRED";

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
        "CLAUDE_CODE_USE_BEDROCK",
        "CLAUDE_CODE_USE_VERTEX",
        "CLAUDE_CODE_USE_FOUNDRY",
        "ANTHROPIC_BASE_URL",
        "CLAUDE_CONFIG_DIR",
        "CLAUDE_CODE_REMOTE",
        "CLAUDE_CODE_REMOTE_MEMORY_DIR",
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

/// Permission request from a worker to the leader
struct SwarmPermissionRequest {
    std::string id;                 ///< Unique request identifier
    std::string worker_id;          ///< Worker's agent ID
    std::string worker_name;        ///< Worker's display name
    std::optional<std::string> worker_color;
    std::string team_name;
    std::string tool_name;          ///< Tool requesting permission
    std::optional<std::string> tool_input; ///< JSON serialized tool input
    std::optional<std::string> rule_content; ///< Matched rule content
    int64_t timestamp = 0;
};

/// Permission response from leader to worker
struct SwarmPermissionResponse {
    std::string request_id;         ///< Matches the request ID
    bool approved = false;
    std::optional<std::string> reason;
    int64_t timestamp = 0;
};

/// Manages synchronized permission prompts across agents in a swarm.
/// Workers send requests to leader's mailbox; leader responds via worker's mailbox.
class PermissionSync {
public:
    /// Send a permission request to the team leader
    static void send_request(const SwarmPermissionRequest& request);

    /// Send a permission response to a worker
    static void send_response(const SwarmPermissionResponse& response,
                              std::string_view worker_name,
                              std::string_view team_name);

    /// Check for pending permission responses in worker's mailbox
    [[nodiscard]] static std::optional<SwarmPermissionResponse> poll_response(
        std::string_view request_id, std::string_view agent_name,
        std::string_view team_name);

    /// Get the permission sync directory for a team
    [[nodiscard]] static fs::path get_sync_dir(std::string_view team_name);

private:
    static inline std::mutex mutex_;
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

} // namespace cc::utils::swarm_helpers
