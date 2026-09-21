/// @file types.cppm
/// @brief Union of all concrete task state types and background task detection.
/// Migrated from src/tasks/types.ts
module;

#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <memory>
#include <set>

export module cc.tasks.types;

import cc.tasks.task;

export namespace cc::tasks {

// Forward declarations of all task state types (defined in their respective modules)
// We use variant-based dispatch for type-safe task state handling.

// ============================================================
// Shell Task State (from LocalShellTask/guards.ts)
// ============================================================

/// Shell task variant: "bash" for regular commands, "monitor" for streaming monitors
enum class BashTaskKind : std::uint8_t {
    Bash,
    Monitor,
};

/// State for a local shell (bash) task
struct LocalShellTaskState : cc::core::TaskStateBase {
    std::string command;
    struct Result {
        int code = 0;
        bool interrupted = false;
    };
    std::optional<Result> result;
    bool completion_status_sent_in_attachment = false;
    // shellCommand is a runtime handle, not serialized
    std::size_t last_reported_total_lines = 0;
    bool is_backgrounded = false;
    std::optional<std::string> agent_id;
    BashTaskKind kind = BashTaskKind::Bash;
};

/// Type guard for LocalShellTaskState
[[nodiscard]] inline bool is_local_shell_task(const cc::core::TaskStateBase& task) noexcept {
    return task.type == cc::core::TaskType::LocalBash;
}

// ============================================================
// Agent Task State (from LocalAgentTask/LocalAgentTask.tsx)
// ============================================================

/// Activity description for a tool use
struct ToolActivity {
    std::string tool_name;
    std::string input_json;  // Serialized input as JSON string
    std::optional<std::string> activity_description;
    bool is_search = false;
    bool is_read = false;
};

/// Progress information for an agent task
struct AgentProgress {
    int tool_use_count = 0;
    int token_count = 0;
    std::optional<ToolActivity> last_activity;
    std::vector<ToolActivity> recent_activities;
    std::optional<std::string> summary;
};

/// Progress tracker for computing incremental updates
struct ProgressTracker {
    int tool_use_count = 0;
    int latest_input_tokens = 0;
    int cumulative_output_tokens = 0;
    std::vector<ToolActivity> recent_activities;
};

/// Create a fresh progress tracker
[[nodiscard]] inline ProgressTracker create_progress_tracker() {
    return ProgressTracker{};
}

/// Get total token count from tracker
[[nodiscard]] inline int get_token_count_from_tracker(const ProgressTracker& tracker) {
    return tracker.latest_input_tokens + tracker.cumulative_output_tokens;
}

/// Agent definition (simplified from AgentDefinition)
struct AgentDefinition {
    std::string agent_type = "general-purpose";
    std::string when_to_use;
    std::string source;
    // System prompt getter omitted (runtime callback)
};

/// Result from an agent tool execution
struct AgentToolResult {
    std::string agent_id;
    std::string output;
    bool success = true;
    std::optional<std::string> error;
};

/// State for a local agent task
struct LocalAgentTaskState : cc::core::TaskStateBase {
    std::string agent_id;
    std::string prompt;
    std::optional<AgentDefinition> selected_agent;
    std::string agent_type = "general-purpose";
    std::optional<std::string> model;
    // abortController is runtime-only
    std::optional<std::string> error;
    std::optional<AgentToolResult> result;
    std::optional<AgentProgress> progress;
    bool retrieved = false;
    // messages stored separately for memory efficiency
    int last_reported_tool_count = 0;
    int last_reported_token_count = 0;
    bool is_backgrounded = false;
    std::vector<std::string> pending_messages;
    bool retain = false;
    bool disk_loaded = false;
    std::optional<std::chrono::system_clock::time_point> evict_after;
};

/// Type guard for LocalAgentTaskState
[[nodiscard]] inline bool is_local_agent_task(const cc::core::TaskStateBase& task) noexcept {
    return task.type == cc::core::TaskType::LocalAgent;
}

/// Check if an agent task is a panel agent (not main-session)
[[nodiscard]] inline bool is_panel_agent_task(const LocalAgentTaskState& task) noexcept {
    return task.agent_type != "main-session";
}

// ============================================================
// Remote Agent Task State (from RemoteAgentTask/RemoteAgentTask.tsx)
// ============================================================

/// Remote task type variants
enum class RemoteTaskType : std::uint8_t {
    RemoteAgent,
    Ultraplan,
    Ultrareview,
    AutofixPr,
    BackgroundPr,
};

/// Convert RemoteTaskType to string
[[nodiscard]] constexpr std::string_view remote_task_type_to_string(RemoteTaskType t) noexcept {
    switch (t) {
        case RemoteTaskType::RemoteAgent: return "remote-agent";
        case RemoteTaskType::Ultraplan: return "ultraplan";
        case RemoteTaskType::Ultrareview: return "ultrareview";
        case RemoteTaskType::AutofixPr: return "autofix-pr";
        case RemoteTaskType::BackgroundPr: return "background-pr";
    }
    return "unknown";
}

/// Ultraplan phase states
enum class UltraplanPhase : std::uint8_t {
    NeedsInput,
    PlanReady,
};

/// Review progress tracking
struct ReviewProgress {
    std::optional<std::string> stage;  // "finding" | "verifying" | "synthesizing"
    int bugs_found = 0;
    int bugs_verified = 0;
    int bugs_refuted = 0;
};

/// Metadata for autofix-pr tasks
struct AutofixPrMetadata {
    std::string owner;
    std::string repo;
    int pr_number = 0;
};

/// State for a remote agent task
struct RemoteAgentTaskState : cc::core::TaskStateBase {
    RemoteTaskType remote_task_type = RemoteTaskType::RemoteAgent;
    std::optional<AutofixPrMetadata> remote_task_metadata;
    std::string session_id;
    std::string command;
    std::string title;
    // todoList and log stored separately for memory efficiency
    bool is_long_running = false;
    std::chrono::system_clock::time_point poll_started_at;
    bool is_remote_review = false;
    std::optional<ReviewProgress> review_progress;
    bool is_ultraplan = false;
    std::optional<UltraplanPhase> ultraplan_phase;
};

/// Type guard for RemoteAgentTaskState
[[nodiscard]] inline bool is_remote_agent_task(const cc::core::TaskStateBase& task) noexcept {
    return task.type == cc::core::TaskType::RemoteAgent;
}

// ============================================================
// In-Process Teammate Task State (from InProcessTeammateTask/types.ts)
// ============================================================

/// Teammate identity stored in task state
struct TeammateIdentity {
    std::string agent_id;       // e.g., "researcher@my-team"
    std::string agent_name;     // e.g., "researcher"
    std::string team_name;
    std::optional<std::string> color;
    bool plan_mode_required = false;
    std::string parent_session_id;
};

/// Permission mode for teammates
enum class PermissionMode : std::uint8_t {
    Default,
    AutoApprove,
    Suggest,
};

/// State for an in-process teammate task
struct InProcessTeammateTaskState : cc::core::TaskStateBase {
    TeammateIdentity identity;
    std::string prompt;
    std::optional<std::string> model;
    std::optional<AgentDefinition> selected_agent;
    // abortController, currentWorkAbortController are runtime-only
    bool awaiting_plan_approval = false;
    PermissionMode permission_mode = PermissionMode::Default;
    std::optional<std::string> error;
    std::optional<AgentToolResult> result;
    std::optional<AgentProgress> progress;
    // messages stored separately
    std::vector<std::string> pending_user_messages;
    std::optional<std::string> spinner_verb;
    std::optional<std::string> past_tense_verb;
    bool is_idle = false;
    bool shutdown_requested = false;
    int last_reported_tool_count = 0;
    int last_reported_token_count = 0;
};

/// Type guard
[[nodiscard]] inline bool is_in_process_teammate_task(const cc::core::TaskStateBase& task) noexcept {
    return task.type == cc::core::TaskType::InProcessTeammate;
}

/// Cap on messages kept in task state for UI display
inline constexpr std::size_t TEAMMATE_MESSAGES_UI_CAP = 50;

/// Append an item with cap enforcement
template<typename T>
[[nodiscard]] std::vector<T> append_capped_message(const std::vector<T>& prev, const T& item) {
    if (prev.empty()) {
        return {item};
    }
    if (prev.size() >= TEAMMATE_MESSAGES_UI_CAP) {
        std::vector<T> next(prev.begin() + 1, prev.end());
        next.push_back(item);
        return next;
    }
    auto result = prev;
    result.push_back(item);
    return result;
}

// ============================================================
// Dream Task State (from DreamTask/DreamTask.ts)
// ============================================================

/// Dream phase
enum class DreamPhase : std::uint8_t {
    Starting,
    Updating,
};

/// A single assistant turn from the dream agent
struct DreamTurn {
    std::string text;
    int tool_use_count = 0;
};

/// State for a dream (memory consolidation) task
struct DreamTaskState : cc::core::TaskStateBase {
    DreamPhase phase = DreamPhase::Starting;
    int sessions_reviewing = 0;
    std::vector<std::string> files_touched;
    std::vector<DreamTurn> turns;
    // abortController is runtime-only
    /// Stashed mtime so kill can rewind the consolidation lock
    std::chrono::system_clock::time_point prior_mtime;
};

/// Type guard
[[nodiscard]] inline bool is_dream_task(const cc::core::TaskStateBase& task) noexcept {
    return task.type == cc::core::TaskType::Dream;
}

/// Max turns kept for live display
inline constexpr std::size_t MAX_DREAM_TURNS = 30;

// ============================================================
// Workflow and Monitor Task States (stubs for completeness)
// ============================================================

/// State for a local workflow task
struct LocalWorkflowTaskState : cc::core::TaskStateBase {
    std::string workflow_name;
    std::string workflow_id;
};

/// State for a monitor MCP task
struct MonitorMcpTaskState : cc::core::TaskStateBase {
    std::string server_name;
    std::string tool_name;
};

// ============================================================
// Task State Variant (union of all types)
// ============================================================

/// Union of all concrete task state types
using TaskState = std::variant<
    LocalShellTaskState,
    LocalAgentTaskState,
    RemoteAgentTaskState,
    InProcessTeammateTaskState,
    LocalWorkflowTaskState,
    MonitorMcpTaskState,
    DreamTaskState
>;

/// Same as TaskState - all types can appear in background indicator
using BackgroundTaskState = TaskState;

/// Check if a task should be shown in the background tasks indicator.
/// A task is considered a background task if:
/// 1. It is running or pending
/// 2. It has been explicitly backgrounded (not a foreground task)
[[nodiscard]] inline bool is_background_task(const cc::core::TaskStateBase& task) noexcept {
    // Must be running or pending
    if (task.status != cc::core::TaskStatus::Running &&
        task.status != cc::core::TaskStatus::Pending) {
        return false;
    }
    return true;
}

/// Overload that also checks isBackgrounded field for shell/agent tasks
[[nodiscard]] inline bool is_background_task(const LocalShellTaskState& task) noexcept {
    if (task.status != cc::core::TaskStatus::Running &&
        task.status != cc::core::TaskStatus::Pending) {
        return false;
    }
    // Foreground tasks are not "background tasks"
    if (!task.is_backgrounded) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool is_background_task(const LocalAgentTaskState& task) noexcept {
    if (task.status != cc::core::TaskStatus::Running &&
        task.status != cc::core::TaskStatus::Pending) {
        return false;
    }
    if (!task.is_backgrounded) {
        return false;
    }
    return true;
}

// ============================================================
// Main Session Task Type (from LocalMainSessionTask.ts)
// ============================================================

/// Main session task is a LocalAgentTaskState with agentType = "main-session"
/// No separate struct needed - just use is_main_session_task() to discriminate
[[nodiscard]] inline bool is_main_session_task(const LocalAgentTaskState& task) noexcept {
    return task.agent_type == "main-session";
}

} // namespace cc::tasks
