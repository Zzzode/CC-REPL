/// @file local_main_session_task.cppm
/// @brief Main session backgrounding task implementation.
/// Migrated from src/tasks/LocalMainSessionTask.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <format>
#include <random>
#include <memory>
#include <atomic>

export module cc.tasks.local_main_session_task;

import cc.tasks.task;
import cc.tasks.types;
import cc.tasks.local_agent_task;

export namespace cc::tasks {

// ============================================================
// Main Session Task ID Generation
// ============================================================

/// Generate a unique task ID for main session tasks.
/// Uses 's' prefix to distinguish from agent tasks ('a' prefix).
[[nodiscard]] inline std::string generate_main_session_task_id() {
    static constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, sizeof(alphabet) - 2);
    
    std::string id = "s";
    for (int i = 0; i < 8; ++i) {
        id += alphabet[dist(rng)];
    }
    return id;
}

// ============================================================
// Default Agent Definition
// ============================================================

/// Default agent definition for main session tasks when no agent is specified
inline const AgentDefinition DEFAULT_MAIN_SESSION_AGENT{
    .agent_type = "main-session",
    .when_to_use = "Main session query",
    .source = "userSettings",
};

// ============================================================
// Registration
// ============================================================

/// Result of registering a main session task
struct MainSessionRegistration {
    std::string task_id;
    std::shared_ptr<std::atomic_bool> abort_requested;
};

/// Register a backgrounded main session task.
/// Called when the user backgrounds the current session query (Ctrl+B twice).
[[nodiscard]] inline MainSessionRegistration register_main_session_task(
    const std::string& description,
    std::optional<AgentDefinition> agent_definition = std::nullopt
) {
    auto task_id = generate_main_session_task_id();
    
    // Create state as a LocalAgentTaskState with agentType = "main-session"
    LocalAgentTaskState state{};
    state.id = cc::core::TaskId{task_id};
    state.type = cc::core::TaskType::LocalAgent;
    state.status = cc::core::TaskStatus::Running;
    state.description = description;
    state.start_time = std::chrono::system_clock::now();
    state.notified = false;
    state.agent_id = task_id;
    state.prompt = description;
    state.selected_agent = agent_definition.value_or(DEFAULT_MAIN_SESSION_AGENT);
    state.agent_type = "main-session";
    state.retrieved = false;
    state.last_reported_tool_count = 0;
    state.last_reported_token_count = 0;
    state.is_backgrounded = true;  // Already backgrounded
    state.retain = false;
    state.disk_loaded = false;
    
    return MainSessionRegistration{
        .task_id = task_id,
        .abort_requested = std::make_shared<std::atomic_bool>(false),
    };
}

// ============================================================
// Completion
// ============================================================

/// Complete the main session task and send notification
inline void complete_main_session_task(
    const std::string& task_id,
    bool success,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [success](LocalAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        task.status = success ? cc::core::TaskStatus::Completed : cc::core::TaskStatus::Failed;
        task.end_time = std::chrono::system_clock::now();
    });
}

/// Generate notification message for main session completion
[[nodiscard]] inline std::string generate_main_session_notification(
    const std::string& task_id,
    const std::string& description,
    const std::string& status,  // "completed" | "failed"
    std::optional<std::string> tool_use_id = std::nullopt
) {
    std::string summary = (status == "completed")
        ? std::format("Background session \"{}\" completed", description)
        : std::format("Background session \"{}\" failed", description);
    
    std::string tool_use_line = tool_use_id
        ? std::format("\n<tool_use_id>{}</tool_use_id>", *tool_use_id)
        : "";
    
    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>{}\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>\n"
        "</task_notification>",
        task_id, tool_use_line, task_id, status, summary
    );
}

// ============================================================
// Foreground
// ============================================================

/// Foreground a main session task - mark it as foregrounded so output
/// appears in the main view. The background query keeps running.
inline void foreground_main_session_task(
    const std::string& task_id,
    UpdateAgentTaskFn update_task
) {
    update_task(task_id, [](LocalAgentTaskState& task) {
        task.is_backgrounded = false;
    });
}

} // namespace cc::tasks
