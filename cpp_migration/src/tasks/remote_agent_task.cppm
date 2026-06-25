/// @file remote_agent_task.cppm
/// @brief Remote agent task implementation with polling and ultraplan support.
/// Migrated from src/tasks/RemoteAgentTask/RemoteAgentTask.tsx
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <format>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>

export module cc.tasks.remote_agent_task;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Constants
// ============================================================

/// Polling interval for remote session updates (1 second)
inline constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1000);

/// Remote review timeout (30 minutes)
inline constexpr auto REMOTE_REVIEW_TIMEOUT = std::chrono::minutes(30);

/// Number of consecutive idle polls needed before considering session done
inline constexpr int STABLE_IDLE_POLLS = 5;

// ============================================================
// Precondition Checking
// ============================================================

/// Precondition error types for remote session eligibility
enum class PreconditionErrorType : std::uint8_t {
    NotLoggedIn,
    NoRemoteEnvironment,
    NotInGitRepo,
    NoGitRemote,
    GithubAppNotInstalled,
    PolicyBlocked,
};

/// A single precondition error
struct PreconditionError {
    PreconditionErrorType type;
};

/// Result of eligibility check
struct RemoteAgentPreconditionResult {
    bool eligible = false;
    std::vector<PreconditionError> errors;
};

/// Format a precondition error for display
[[nodiscard]] inline std::string format_precondition_error(PreconditionErrorType type) {
    switch (type) {
        case PreconditionErrorType::NotLoggedIn:
            return "Please run /login and sign in with your Claude.ai account (not Console).";
        case PreconditionErrorType::NoRemoteEnvironment:
            return "No cloud environment available. Set one up at https://claude.ai/code/onboarding?magic=env-setup";
        case PreconditionErrorType::NotInGitRepo:
            return "Background tasks require a git repository. Initialize git or run from a git repository.";
        case PreconditionErrorType::NoGitRemote:
            return "Background tasks require a GitHub remote. Add one with `git remote add origin REPO_URL`.";
        case PreconditionErrorType::GithubAppNotInstalled:
            return "The Claude GitHub app must be installed on this repository first.\nhttps://github.com/apps/claude/installations/new";
        case PreconditionErrorType::PolicyBlocked:
            return "Remote sessions are disabled by your organization's policy. Contact your organization admin.";
    }
    return "Unknown error";
}

// ============================================================
// Completion Checker Registry
// ============================================================

/// Completion checker callback - returns non-null string to complete the task
using RemoteTaskCompletionChecker = std::function<std::optional<std::string>(
    const std::optional<AutofixPrMetadata>&)>;

/// Registry of completion checkers per task type
class CompletionCheckerRegistry {
    std::mutex mutex_;
    std::unordered_map<RemoteTaskType, RemoteTaskCompletionChecker> checkers_;

public:
    void register_checker(RemoteTaskType type, RemoteTaskCompletionChecker checker) {
        std::lock_guard lock(mutex_);
        checkers_[type] = std::move(checker);
    }
    
    [[nodiscard]] std::optional<RemoteTaskCompletionChecker> get(RemoteTaskType type) {
        std::lock_guard lock(mutex_);
        if (auto it = checkers_.find(type); it != checkers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

/// Global completion checker registry
inline CompletionCheckerRegistry& completion_checkers() {
    static CompletionCheckerRegistry instance;
    return instance;
}

/// Register a completion checker
inline void register_completion_checker(RemoteTaskType type, RemoteTaskCompletionChecker checker) {
    completion_checkers().register_checker(type, std::move(checker));
}

// ============================================================
// Remote Task State Creation
// ============================================================

/// Create a remote agent task state
[[nodiscard]] inline RemoteAgentTaskState create_remote_agent_state(
    const std::string& task_id,
    RemoteTaskType remote_task_type,
    const std::string& session_id,
    const std::string& command,
    const std::string& title,
    std::optional<std::string> tool_use_id = std::nullopt,
    bool is_remote_review = false,
    bool is_ultraplan = false,
    bool is_long_running = false,
    std::optional<AutofixPrMetadata> metadata = std::nullopt
) {
    RemoteAgentTaskState state{};
    state.id = cc::core::TaskId{task_id};
    state.type = cc::core::TaskType::RemoteAgent;
    state.status = cc::core::TaskStatus::Running;
    state.description = title;
    state.tool_use_id = tool_use_id;
    state.start_time = std::chrono::system_clock::now();
    state.notified = false;
    state.remote_task_type = remote_task_type;
    state.remote_task_metadata = std::move(metadata);
    state.session_id = session_id;
    state.command = command;
    state.title = title;
    state.is_long_running = is_long_running;
    state.poll_started_at = std::chrono::system_clock::now();
    state.is_remote_review = is_remote_review;
    state.is_ultraplan = is_ultraplan;
    return state;
}

// ============================================================
// Polling
// ============================================================

/// Session poll response
struct PollResponse {
    std::string last_event_id;
    std::string session_status;  // "idle" | "archived" | "running"
    std::vector<std::string> new_events;  // Serialized SDK messages
};

/// Callback type for fetching session events
using PollSessionFn = std::function<std::optional<PollResponse>(
    const std::string& session_id, const std::optional<std::string>& last_event_id)>;

/// Remote session poller - manages periodic polling for a single task
class RemoteSessionPoller {
    std::atomic<bool> running_{true};
    std::jthread thread_;

public:
    RemoteSessionPoller() = default;
    
    /// Start polling a remote session
    void start(
        std::string task_id,
        PollSessionFn poll_fn,
        std::function<void(const std::string&, const PollResponse&)> on_update,
        std::function<void(const std::string&, const std::string&)> on_complete,
        std::function<void(const std::string&, const std::string&)> on_fail
    ) {
        thread_ = std::jthread([this, task_id = std::move(task_id),
                                 poll_fn = std::move(poll_fn),
                                 on_update = std::move(on_update),
                                 on_complete = std::move(on_complete),
                                 on_fail = std::move(on_fail)](std::stop_token stop) {
            std::optional<std::string> last_event_id;
            int consecutive_idle_polls = 0;
            
            while (!stop.stop_requested() && running_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(POLL_INTERVAL);
                if (!running_.load(std::memory_order_relaxed)) break;
                
                auto response = poll_fn(task_id, last_event_id);
                if (!response) continue;
                
                last_event_id = response->last_event_id;
                bool log_grew = !response->new_events.empty();
                
                // Notify of update
                on_update(task_id, *response);
                
                // Check for archived status
                if (response->session_status == "archived") {
                    on_complete(task_id, "completed");
                    return;
                }
                
                // Track idle polls for stable idle detection
                if (response->session_status == "idle" && !log_grew) {
                    consecutive_idle_polls++;
                } else {
                    consecutive_idle_polls = 0;
                }
                
                if (consecutive_idle_polls >= STABLE_IDLE_POLLS) {
                    on_complete(task_id, "completed");
                    return;
                }
            }
        });
    }
    
    /// Stop polling
    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.request_stop();
        }
    }
    
    ~RemoteSessionPoller() { stop(); }
    
    RemoteSessionPoller(const RemoteSessionPoller&) = delete;
    RemoteSessionPoller& operator=(const RemoteSessionPoller&) = delete;
    RemoteSessionPoller(RemoteSessionPoller&&) = delete;
    RemoteSessionPoller& operator=(RemoteSessionPoller&&) = delete;
};

// ============================================================
// Remote Task Kill
// ============================================================

/// Kill a remote agent task and archive the session
inline void kill_remote_agent_task(
    const std::string& task_id,
    std::function<void(const std::string&, std::function<void(RemoteAgentTaskState&)>)> update_task,
    std::function<void(const std::string&)> archive_session
) {
    std::optional<std::string> session_id;
    
    update_task(task_id, [&](RemoteAgentTaskState& task) {
        if (task.status != cc::core::TaskStatus::Running) return;
        
        session_id = task.session_id;
        task.status = cc::core::TaskStatus::Killed;
        task.notified = true;
        task.end_time = std::chrono::system_clock::now();
    });
    
    // Archive the remote session to free cloud resources
    if (session_id) {
        archive_session(*session_id);
    }
}

// ============================================================
// Plan/Review Extraction
// ============================================================

/// Extract ultraplan content from log (searches for <ultraplan> tags)
[[nodiscard]] inline std::optional<std::string> extract_plan_from_log(
    const std::vector<std::string>& log_messages
) {
    // Walk backwards through messages looking for ultraplan tags
    for (auto it = log_messages.rbegin(); it != log_messages.rend(); ++it) {
        auto open_pos = it->find("<ultraplan>");
        auto close_pos = it->find("</ultraplan>");
        if (open_pos != std::string::npos && close_pos != std::string::npos && close_pos > open_pos) {
            auto content = it->substr(open_pos + 11, close_pos - open_pos - 11);
            if (!content.empty()) {
                return content;
            }
        }
    }
    return std::nullopt;
}

/// Get the session URL for a remote task
[[nodiscard]] inline std::string get_remote_task_session_url(
    const std::string& session_id,
    std::optional<std::string> ingress_url = std::nullopt
) {
    if (ingress_url) {
        return std::format("{}/chat/{}", *ingress_url, session_id);
    }
    return std::format("https://claude.ai/chat/{}", session_id);
}

} // namespace cc::tasks
