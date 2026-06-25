/// @file local_agent_task.cppm
/// @brief Local agent task implementation with progress tracking and lifecycle management.
/// Migrated from src/tasks/LocalAgentTask/LocalAgentTask.tsx
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <utility>
#include <chrono>
#include <format>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <future>

export module cc.tasks.local_agent_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Constants
// ============================================================

/// Maximum number of recent activities to keep in progress tracker
inline constexpr std::size_t MAX_RECENT_ACTIVITIES = 5;

/// Grace period before panel eviction (milliseconds)
inline constexpr auto PANEL_GRACE_MS = std::chrono::milliseconds(5000);

// ============================================================
// Progress Tracking
// ============================================================

/// Update progress tracker from an assistant message's usage data
inline void update_progress_from_usage(
    ProgressTracker& tracker,
    int input_tokens,
    int cache_creation_tokens,
    int cache_read_tokens,
    int output_tokens
) {
    // Keep latest input (it's cumulative in the API), sum outputs
    tracker.latest_input_tokens = input_tokens + cache_creation_tokens + cache_read_tokens;
    tracker.cumulative_output_tokens += output_tokens;
}

/// Record a tool use in the progress tracker
inline void record_tool_use(
    ProgressTracker& tracker,
    const std::string& tool_name,
    const std::string& input_json,
    std::optional<std::string> activity_description = std::nullopt,
    bool is_search = false,
    bool is_read = false
) {
    tracker.tool_use_count++;
    tracker.recent_activities.push_back(ToolActivity{
        .tool_name = tool_name,
        .input_json = input_json,
        .activity_description = std::move(activity_description),
        .is_search = is_search,
        .is_read = is_read,
    });
    // Cap recent activities
    while (tracker.recent_activities.size() > MAX_RECENT_ACTIVITIES) {
        tracker.recent_activities.erase(tracker.recent_activities.begin());
    }
}

/// Get a snapshot of current progress from tracker
[[nodiscard]] inline AgentProgress get_progress_update(const ProgressTracker& tracker) {
    return AgentProgress{
        .tool_use_count = tracker.tool_use_count,
        .token_count = get_token_count_from_tracker(tracker),
        .last_activity = tracker.recent_activities.empty() 
            ? std::nullopt 
            : std::optional{tracker.recent_activities.back()},
        .recent_activities = tracker.recent_activities,
        .summary = std::nullopt
    };
}

// ============================================================
// Background Signal Management
// ============================================================

/// Manages background signal promises for foreground->background transitions
class BackgroundSignalRegistry {
    std::mutex mutex_;
    std::unordered_map<std::string, std::promise<void>> resolvers_;
    std::unordered_map<std::string, std::shared_future<void>> futures_;

public:
    /// Register a background signal for a task, returns a future to await
    [[nodiscard]] std::shared_future<void> register_signal(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        std::promise<void> promise;
        auto future = promise.get_future().share();
        futures_[task_id] = future;
        resolvers_[task_id] = std::move(promise);
        return future;
    }
    
    /// Resolve the background signal (task has been backgrounded)
    void resolve(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        if (auto it = resolvers_.find(task_id); it != resolvers_.end()) {
            it->second.set_value();
            resolvers_.erase(it);
        }
    }
    
    /// Remove a signal (task completed without backgrounding)
    void remove(const std::string& task_id) {
        std::lock_guard lock(mutex_);
        resolvers_.erase(task_id);
        futures_.erase(task_id);
    }
};

/// Global background signal registry
inline BackgroundSignalRegistry& background_signals() {
    static BackgroundSignalRegistry instance;
    return instance;
}

// ============================================================
// Agent Task State Mutation Callbacks
// ============================================================

/// Callback type for updating a specific agent task's state
using UpdateAgentTaskFn = std::function<void(const std::string&, std::function<void(LocalAgentTaskState&)>)>;

/// Callback type for registering a task in AppState
using RegisterTaskFn = std::function<void(LocalAgentTaskState)>;

/// Callback type for removing a task from AppState
using RemoveTaskFn = std::function<void(const std::string&)>;

// ============================================================
// Agent Task Lifecycle
// ============================================================

/// Kill an agent task. No-op if already killed/completed.
inline void kill_async_agent(
    const std::string& task_id,
    UpdateAgentTaskFn update_task,
    std::function<void()> abort_fn
) {
    bool killed = false;
    update_task(task_id, [&](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        killed = true;
        abort_fn();  // Abort the agent's execution
        
        task.status = cc::core::TaskStatus::Killed;
        task.end_time = std::chrono::system_clock::now();
        task.evict_after = task.retain 
            ? std::nullopt 
            : std::optional{std::chrono::system_clock::now() + PANEL_GRACE_MS};
        task.selected_agent = std::nullopt;
    });
}

/// Kill all running agent tasks (used by ESC cancellation)
template <typename GetTasksFn>
inline void kill_all_running_agent_tasks(
    GetTasksFn get_tasks,
    UpdateAgentTaskFn update_task,
    std::function<void(const std::string&)> abort_fn
) {
    auto tasks = get_tasks();
    for (const auto& [task_id, task] : tasks) {
        if (task.status == cc::core::TaskStatus::Running) {
            kill_async_agent(task_id, update_task, [&]() { abort_fn(task_id); });
        }
    }
}

/// Mark agents as notified without sending notification
inline void mark_agents_notified(
    const std::string& task_id,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [](LocalAgentTaskState& task) {
        task.notified = true;
    });
}

/// Update progress for a running agent task
inline void update_agent_progress(
    const std::string& task_id,
    const AgentProgress& progress,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [&](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        // Preserve existing summary
        auto existing_summary = task.progress ? task.progress->summary : std::nullopt;
        task.progress = progress;
        if (existing_summary) {
            task.progress->summary = existing_summary;
        }
    });
}

/// Update the background summary for an agent task
inline void update_agent_summary(
    const std::string& task_id,
    const std::string& summary,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [&](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        if (!task.progress) {
            task.progress = AgentProgress{};
        }
        task.progress->summary = summary;
    });
}

/// Complete an agent task with result
inline void complete_agent_task(
    const AgentToolResult& result,
    UpdateAgentTaskFn update_task
) {
    update_task(result.agent_id, [&](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        task.status = cc::core::TaskStatus::Completed;
        task.result = result;
        task.end_time = std::chrono::system_clock::now();
        task.evict_after = task.retain
            ? std::nullopt
            : std::optional{std::chrono::system_clock::now() + PANEL_GRACE_MS};
        task.selected_agent = std::nullopt;
    });
}

/// Fail an agent task with error
inline void fail_agent_task(
    const std::string& task_id,
    const std::string& error,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [&](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        task.status = cc::core::TaskStatus::Failed;
        task.error = error;
        task.end_time = std::chrono::system_clock::now();
        task.evict_after = task.retain
            ? std::nullopt
            : std::optional{std::chrono::system_clock::now() + PANEL_GRACE_MS};
        task.selected_agent = std::nullopt;
    });
}

/// Register an async (immediately backgrounded) agent task
[[nodiscard]] inline LocalAgentTaskState create_async_agent_state(
    const std::string& agent_id,
    const std::string& description,
    const std::string& prompt,
    const AgentDefinition& selected_agent,
    std::optional<std::string> tool_use_id = std::nullopt
) {
    LocalAgentTaskState state{};
    state.id = cc::core::TaskId{agent_id};
    state.type = cc::core::TaskType::LocalAgent;
    state.status = cc::core::TaskStatus::Running;
    state.description = description;
    state.tool_use_id = tool_use_id;
    state.start_time = std::chrono::system_clock::now();
    state.notified = false;
    state.agent_id = agent_id;
    state.prompt = prompt;
    state.selected_agent = selected_agent;
    state.agent_type = selected_agent.agent_type.empty() ? "general-purpose" : selected_agent.agent_type;
    state.retrieved = false;
    state.last_reported_tool_count = 0;
    state.last_reported_token_count = 0;
    state.is_backgrounded = true;  // Immediately backgrounded
    state.retain = false;
    state.disk_loaded = false;
    return state;
}

/// Register a foreground agent task that could be backgrounded later
struct ForegroundAgentRegistration {
    std::string task_id;
    std::shared_future<void> background_signal;
};

[[nodiscard]] inline ForegroundAgentRegistration create_foreground_agent_state(
    const std::string& agent_id,
    const std::string& description,
    const std::string& prompt,
    const AgentDefinition& selected_agent,
    std::optional<std::string> tool_use_id = std::nullopt
) {
    auto state = create_async_agent_state(agent_id, description, prompt, selected_agent, tool_use_id);
    state.is_backgrounded = false;  // Not yet backgrounded
    
    // Register background signal
    auto signal = background_signals().register_signal(agent_id);
    
    return ForegroundAgentRegistration{
        .task_id = agent_id,
        .background_signal = std::move(signal),
    };
}

/// Background a specific foreground agent task
[[nodiscard]] inline bool background_agent_task(
    const std::string& task_id,
    UpdateAgentTaskFn update_task
) {
    bool backgrounded = false;
    update_task(task_id, [&](LocalAgentTaskState& task) {
        if (task.is_backgrounded) return;
        task.is_backgrounded = true;
        backgrounded = true;
    });
    
    if (backgrounded) {
        // Resolve the background signal
        background_signals().resolve(task_id);
    }
    return backgrounded;
}

/// Unregister a foreground agent task (completed without backgrounding)
inline void unregister_agent_foreground(
    const std::string& task_id,
    RemoveTaskFn remove_task
) {
    background_signals().remove(task_id);
    remove_task(task_id);
}

/// Queue a pending message for an agent task
inline void queue_pending_message(
    const std::string& task_id,
    const std::string& msg,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [&](LocalAgentTaskState& task) {
        task.pending_messages.push_back(msg);
    });
}

/// Drain all pending messages from an agent task
[[nodiscard]] inline std::vector<std::string> drain_pending_messages(
    const std::string& task_id,
    std::function<std::optional<LocalAgentTaskState>(const std::string&)> get_task,
    UpdateAgentTaskFn update_task
) {
    auto task_opt = get_task(task_id);
    if (!task_opt || task_opt->pending_messages.empty()) {
        return {};
    }
    
    auto drained = task_opt->pending_messages;
    update_task(task_id, [](LocalAgentTaskState& task) {
        task.pending_messages.clear();
    });
    return drained;
}

// ============================================================
// Agent Notification
// ============================================================

[[nodiscard]] inline std::string escape_xml_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result.push_back(ch); break;
        }
    }
    return result;
}

/// Generate XML notification for an agent task completion
[[nodiscard]] inline std::string generate_agent_notification(
    const std::string& task_id,
    const std::string& description,
    const std::string& status,  // "completed" | "failed" | "stopped"
    std::optional<std::string> error = std::nullopt,
    std::optional<std::string> final_message = std::nullopt,
    std::optional<std::string> tool_use_id = std::nullopt,
    std::optional<std::string> worktree_path = std::nullopt,
    std::optional<std::string> worktree_branch = std::nullopt,
    std::optional<std::string> output_file = std::nullopt
) {
    const auto escaped_task_id = escape_xml_text(task_id);
    const auto escaped_description = escape_xml_text(description);
    const auto escaped_status = escape_xml_text(status);
    std::string summary;
    if (status == "completed") {
        summary = std::format("Agent \"{}\" completed", escaped_description);
    } else if (status == "failed") {
        summary = std::format("Agent \"{}\" failed: {}", escaped_description, escape_xml_text(error.value_or("Unknown error")));
    } else {
        summary = std::format("Agent \"{}\" was stopped", escaped_description);
    }
    
    std::string tool_use_line = tool_use_id 
        ? std::format("\n<tool_use_id>{}</tool_use_id>", escape_xml_text(*tool_use_id)) 
        : "";
    std::string result_section = final_message 
        ? std::format("\n<result>{}</result>", escape_xml_text(*final_message)) 
        : "";
    std::string worktree_section = worktree_path
        ? std::format("\n<worktree><worktree_path>{}</worktree_path>{}</worktree>",
            escape_xml_text(*worktree_path),
            worktree_branch ? std::format("<worktree_branch>{}</worktree_branch>", escape_xml_text(*worktree_branch)) : "")
        : "";
    
    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>{}\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}{}\n"
        "</task_notification>",
        escaped_task_id, tool_use_line,
        escape_xml_text(output_file.value_or("")),
        escaped_status, summary, result_section, worktree_section
    );
}

} // namespace cc::tasks
