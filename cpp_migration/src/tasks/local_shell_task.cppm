/// @file local_shell_task.cppm
/// @brief Local shell task implementation with stall detection and background management.
/// Migrated from src/tasks/LocalShellTask/ (guards.ts, killShellTasks.ts, LocalShellTask.tsx)
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <regex>
#include <atomic>
#include <mutex>
#include <thread>
#include <format>
#include <memory>
#include <utility>

export module cc.tasks.local_shell_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Constants
// ============================================================

/// Prefix identifying a LocalShellTask summary for UI collapse transform
inline constexpr std::string_view BACKGROUND_BASH_SUMMARY_PREFIX = "Background command ";

/// Interval between stall checks (5 seconds)
inline constexpr auto STALL_CHECK_INTERVAL = std::chrono::milliseconds(5000);

/// Duration without output growth before considering a command stalled (45 seconds)
inline constexpr auto STALL_THRESHOLD = std::chrono::milliseconds(45000);

/// Number of bytes to read from tail for stall detection
inline constexpr std::size_t STALL_TAIL_BYTES = 1024;

// ============================================================
// Prompt Detection (from looksLikePrompt)
// ============================================================

/// Patterns that suggest a command is blocked waiting for keyboard input.
/// Used to gate the stall notification - we stay silent on commands that
/// are merely slow and only notify when the tail looks like an interactive prompt.
[[nodiscard]] inline bool looks_like_prompt(std::string_view tail) {
    // Get the last non-empty line
    auto trimmed = tail;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' ')) {
        trimmed.remove_suffix(1);
    }
    
    auto last_newline = trimmed.rfind('\n');
    std::string last_line;
    if (last_newline == std::string_view::npos) {
        last_line = std::string(trimmed);
    } else {
        last_line = std::string(trimmed.substr(last_newline + 1));
    }
    
    // Pattern matching for interactive prompts
    static const std::vector<std::regex> patterns = {
        std::regex(R"(\(y/n\))", std::regex::icase),
        std::regex(R"(\[y/n\])", std::regex::icase),
        std::regex(R"(\(yes/no\))", std::regex::icase),
        std::regex(R"(\b(?:Do you|Would you|Shall I|Are you sure|Ready to)\b.*\?\s*$)", std::regex::icase),
        std::regex(R"(Press (any key|Enter))", std::regex::icase),
        std::regex(R"(Continue\?)", std::regex::icase),
        std::regex(R"(Overwrite\?)", std::regex::icase),
    };
    
    for (const auto& pattern : patterns) {
        if (std::regex_search(last_line, pattern)) {
            return true;
        }
    }
    return false;
}

// ============================================================
// Stall Watchdog
// ============================================================

/// Handle for cancelling a stall watchdog
class StallWatchdog {
    std::atomic<bool> cancelled_{false};
    std::jthread thread_;

public:
    StallWatchdog() = default;
    
    /// Start monitoring output file for stalls
    /// @param task_id Task identifier
    /// @param description Human-readable task description
    /// @param kind Task kind (bash/monitor)
    /// @param on_stall Callback invoked when stall is detected with prompt-like output
    void start(
        std::string task_id,
        std::string description,
        BashTaskKind kind,
        std::function<void(std::string_view last_output)> on_stall
    ) {
        // Don't watch monitors - they're expected to run indefinitely
        if (kind == BashTaskKind::Monitor) return;
        
        thread_ = std::jthread([this, task_id = std::move(task_id), 
                                 description = std::move(description),
                                 on_stall = std::move(on_stall)](std::stop_token stop) {
            std::size_t last_size = 0;
            auto last_growth = std::chrono::steady_clock::now();
            
            while (!stop.stop_requested() && !cancelled_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(STALL_CHECK_INTERVAL);
                
                if (cancelled_.load(std::memory_order_relaxed)) break;
                
                auto now = std::chrono::steady_clock::now();
                auto elapsed = now - last_growth;
                
                if (elapsed < STALL_THRESHOLD) continue;
                
                // Would read tail of output file here
                // If looks_like_prompt(tail), fire notification
                // Reset last_growth after notifying or if not a prompt
                last_growth = now;
            }
        });
    }
    
    /// Cancel the watchdog
    void cancel() {
        cancelled_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.request_stop();
        }
    }
    
    ~StallWatchdog() { cancel(); }
    
    // Non-copyable, movable
    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;
    StallWatchdog(StallWatchdog&&) = default;
    StallWatchdog& operator=(StallWatchdog&&) = default;
};

// ============================================================
// Shell Task Kill Logic (from killShellTasks.ts)
// ============================================================

/// Callback type for state mutation
using SetAppStateFn = std::function<void(std::function<void(cc::core::TaskStateBase&)>)>;

/// Kill a specific shell task by ID
/// @param task_id The task to kill
/// @param update_task Callback to mutate task state
/// @param kill_process Callback to kill the actual OS process
inline void kill_shell_task(
    const std::string& task_id,
    std::function<void(const std::string&, std::function<void(LocalShellTaskState&)>)> update_task,
    std::function<void(const std::string&)> kill_process
) {
    update_task(task_id, [&](LocalShellTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        // Kill the process
        kill_process(task_id);
        
        // Update state
        task.status = cc::core::TaskStatus::Killed;
        task.notified = true;
        task.end_time = std::chrono::system_clock::now();
    });
}

/// Kill all running bash tasks spawned by a given agent.
/// Called from runAgent finally block so background processes don't outlive
/// the agent that started them.
template <typename GetShellTasksFn>
inline void kill_shell_tasks_for_agent(
    const std::string& agent_id,
    GetShellTasksFn get_shell_tasks,
    std::function<void(const std::string&, std::function<void(LocalShellTaskState&)>)> update_task,
    std::function<void(const std::string&)> kill_process
) {
    auto tasks = get_shell_tasks();
    for (const auto& [task_id, task] : tasks) {
        if (task.agent_id == agent_id && task.status == cc::core::TaskStatus::Running) {
            kill_shell_task(task_id, update_task, kill_process);
        }
    }
}

// ============================================================
// Shell Task Spawn & Lifecycle
// ============================================================

/// Input for spawning a shell task
struct ShellSpawnInput {
    std::string command;
    std::string description;
    std::optional<std::string> tool_use_id;
    std::optional<std::string> agent_id;
    BashTaskKind kind = BashTaskKind::Bash;
};

/// Handle returned after spawning a shell task
struct ShellTaskHandle {
    std::string task_id;
    std::function<void()> cleanup;
};

/// Result of a shell command execution
struct ShellExitResult {
    int code = 0;
    bool interrupted = false;
};

/// Notification status for completed shell tasks
enum class ShellNotificationStatus : std::uint8_t {
    Completed,
    Failed,
    Killed,
};

/// Generate notification summary for a shell task
[[nodiscard]] inline std::string generate_shell_notification_summary(
    std::string_view description,
    ShellNotificationStatus status,
    std::optional<int> exit_code,
    BashTaskKind kind
) {
    if (kind == BashTaskKind::Monitor) {
        switch (status) {
            case ShellNotificationStatus::Completed:
                return std::format("Monitor \"{}\" stream ended", description);
            case ShellNotificationStatus::Failed:
                return exit_code 
                    ? std::format("Monitor \"{}\" script failed (exit {})", description, *exit_code)
                    : std::format("Monitor \"{}\" script failed", description);
            case ShellNotificationStatus::Killed:
                return std::format("Monitor \"{}\" stopped", description);
        }
    }
    
    switch (status) {
        case ShellNotificationStatus::Completed:
            return exit_code
                ? std::format("{}\"{}\" completed (exit code {})", BACKGROUND_BASH_SUMMARY_PREFIX, description, *exit_code)
                : std::format("{}\"{}\" completed", BACKGROUND_BASH_SUMMARY_PREFIX, description);
        case ShellNotificationStatus::Failed:
            return exit_code
                ? std::format("{}\"{}\" failed with exit code {}", BACKGROUND_BASH_SUMMARY_PREFIX, description, *exit_code)
                : std::format("{}\"{}\" failed", BACKGROUND_BASH_SUMMARY_PREFIX, description);
        case ShellNotificationStatus::Killed:
            return std::format("{}\"{}\" was stopped", BACKGROUND_BASH_SUMMARY_PREFIX, description);
    }
    return "";
}

/// Check if there are any foreground tasks that can be backgrounded
[[nodiscard]] inline bool has_foreground_tasks(
    const std::vector<std::pair<std::string, LocalShellTaskState>>& shell_tasks,
    const std::vector<std::pair<std::string, LocalAgentTaskState>>& agent_tasks
) {
    for (const auto& [_, task] : shell_tasks) {
        if (!task.is_backgrounded && task.status == cc::core::TaskStatus::Running) {
            return true;
        }
    }
    for (const auto& [_, task] : agent_tasks) {
        if (!task.is_backgrounded && !is_main_session_task(task) && 
            task.status == cc::core::TaskStatus::Running) {
            return true;
        }
    }
    return false;
}

/// Mark a task as notified to suppress pending notification
inline void mark_shell_task_notified(
    const std::string& task_id,
    std::function<void(const std::string&, std::function<void(LocalShellTaskState&)>)> update_task
) {
    update_task(task_id, [](LocalShellTaskState& task) {
        task.notified = true;
    });
}

} // namespace cc::tasks
