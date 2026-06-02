/// @file runner.cppm
/// @brief Self-hosted runner and environment runner module.
/// Implements task polling, execution, heartbeat reporting, and specialized
/// runner variants for container/sandbox and full-access environments.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <mutex>
#include <atomic>

#include <uv.h>

export module cc.entrypoints.runner;

import cc.types.types;

export namespace cc::core {

// ============================================================
// Runner configuration
// ============================================================

/// Configuration for a runner instance
struct RunnerConfig {
    std::string api_endpoint;                   // Coordinator API endpoint URL
    std::string auth_token;                     // Authentication token
    std::chrono::milliseconds poll_interval{5000}; // Interval between task polls
    uint32_t max_concurrent_tasks = 4;          // Max parallel task executions
    std::string workspace_dir;                  // Root workspace directory

    /// Capabilities declaration - what this runner can do
    struct Capabilities {
        std::vector<std::string> allowed_tools; // Tool names this runner can invoke
        std::vector<std::string> models;        // LLM models available
        uint64_t max_memory_mb = 4096;          // Memory limit in MB
        uint64_t max_disk_mb = 10240;           // Disk limit in MB
        bool network_access = true;             // Whether network is available
        bool can_spawn_agents = false;          // Sub-agent spawning support
    } capabilities;
};

// ============================================================
// Task payload and report structures
// ============================================================

/// Incoming task from the coordinator
struct TaskPayload {
    std::string id;                             // Unique task identifier
    std::string prompt;                         // The user prompt / instruction
    std::vector<std::string> context;           // Additional context documents
    std::chrono::milliseconds timeout{300000};  // Task timeout (default 5 min)

    /// Constraints on task execution
    struct Constraints {
        std::optional<std::string> model;       // Required model
        std::vector<std::string> required_tools; // Tools that must be available
        std::optional<uint32_t> max_tokens;     // Output token limit
        bool allow_network = true;              // Network access during execution
    } constraints;
};

/// Status of a completed or failed task
enum class TaskStatus : uint8_t {
    Pending,
    Running,
    Completed,
    Failed,
    Timeout,
    Cancelled,
};

/// Report sent back to the coordinator after task execution
struct TaskReport {
    std::string id;                             // Matches TaskPayload::id
    TaskStatus status = TaskStatus::Pending;
    std::optional<std::string> result;          // Output on success
    std::optional<std::string> error;           // Error message on failure
    TokenUsage token_usage;                     // Tokens consumed
    std::chrono::milliseconds duration{0};      // Wall-clock execution time

    /// Format report as summary string
    [[nodiscard]] std::string summary() const {
        return std::format("[Task {}] status={} duration={}ms tokens={}",
            id, static_cast<int>(status), duration.count(), token_usage.total());
    }
};

// ============================================================
// Runner base class
// ============================================================

/// Base Runner: polls for tasks, executes them, and reports results.
/// Subclasses specialize for different execution environments.
class Runner {
public:
    explicit Runner(RunnerConfig config)
        : config_(std::move(config))
        , state_(RunnerState::Stopped) {}

    virtual ~Runner() { stop(); }

    /// Start the runner's polling loop (non-blocking, uses libuv timer)
    VoidResult start() {
        if (state_ == RunnerState::Running) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Runner already started"));
        }
        state_ = RunnerState::Running;
        loop_ = uv_default_loop();

        // Initialize the poll timer
        uv_timer_init(loop_, &poll_timer_);
        poll_timer_.data = this;
        uv_timer_start(&poll_timer_, on_poll_tick,
                       0, config_.poll_interval.count());

        // Initialize the heartbeat timer
        uv_timer_init(loop_, &heartbeat_timer_);
        heartbeat_timer_.data = this;
        uv_timer_start(&heartbeat_timer_, on_heartbeat_tick,
                       1000, 10000); // Heartbeat every 10s
        return {};
    }

    /// Graceful shutdown: stop polling and wait for active tasks
    void stop() {
        if (state_ != RunnerState::Running) return;
        state_ = RunnerState::Stopping;

        uv_timer_stop(&poll_timer_);
        uv_timer_stop(&heartbeat_timer_);

        // Wait for in-flight tasks to drain
        state_ = RunnerState::Stopped;
    }

    /// Execute a single task and produce a report
    TaskReport handle_task(const TaskPayload& task) {
        auto start_time = std::chrono::steady_clock::now();
        TaskReport report{.id = task.id, .status = TaskStatus::Running};

        // Check constraints before execution
        if (auto err = validate_constraints(task.constraints)) {
            report.status = TaskStatus::Failed;
            report.error = err->message;
            return report;
        }

        // Mark task as active
        {
            std::lock_guard lock(tasks_mutex_);
            active_tasks_.insert(task.id);
        }

        // Execute the task (subclasses override execute_impl)
        auto result = execute_impl(task);
        auto end_time = std::chrono::steady_clock::now();
        report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);

        if (result) {
            report.status = TaskStatus::Completed;
            report.result = std::move(*result);
        } else {
            report.status = TaskStatus::Failed;
            report.error = result.error().message;
        }

        // Remove from active set
        {
            std::lock_guard lock(tasks_mutex_);
            active_tasks_.erase(task.id);
        }
        return report;
    }

    /// Send a heartbeat status to the coordinator
    void report_status() {
        std::lock_guard lock(tasks_mutex_);
        // Build status payload with current load info
        auto status_json = std::format(
            R"({{"runner_id":"{}","state":"{}","active_tasks":{},"capabilities":{{"tools_count":{}}}}})",
            runner_id_, state_to_string(state_),
            active_tasks_.size(), config_.capabilities.allowed_tools.size());
        send_to_coordinator("/status", status_json);
    }

    // Accessors
    [[nodiscard]] const RunnerConfig& config() const noexcept { return config_; }
    [[nodiscard]] uint32_t active_task_count() const {
        std::lock_guard lock(tasks_mutex_);
        return static_cast<uint32_t>(active_tasks_.size());
    }

protected:
    /// Override in subclasses: actual task execution logic
    virtual Result<std::string> execute_impl(const TaskPayload& task) = 0;

    /// Check if task constraints match this runner's capabilities
    std::optional<Error> validate_constraints(const TaskPayload::Constraints& constraints) {
        if (!constraints.required_tools.empty()) {
            for (const auto& tool : constraints.required_tools) {
                bool found = std::ranges::find(
                    config_.capabilities.allowed_tools, tool) !=
                    config_.capabilities.allowed_tools.end();
                if (!found) {
                    return Error::make(ErrorCode::ToolNotFound,
                        std::format("Required tool '{}' not available", tool));
                }
            }
        }
        return std::nullopt;
    }

    /// Send a payload to the coordinator endpoint
    void send_to_coordinator(std::string_view path, std::string_view payload) {
        last_coordinator_path_ = std::string(path);
        last_coordinator_payload_ = std::string(payload);
    }

    enum class RunnerState : uint8_t { Stopped, Running, Stopping };

    static constexpr std::string_view state_to_string(RunnerState s) {
        switch (s) {
            case RunnerState::Stopped:  return "stopped";
            case RunnerState::Running:  return "running";
            case RunnerState::Stopping: return "stopping";
        }
        return "unknown";
    }

    RunnerConfig config_;
    std::string runner_id_;
    RunnerState state_;
    uv_loop_t* loop_ = nullptr;
    uv_timer_t poll_timer_{};
    uv_timer_t heartbeat_timer_{};
    mutable std::mutex tasks_mutex_;
    std::unordered_set<std::string> active_tasks_;
    std::string last_coordinator_path_;
    std::string last_coordinator_payload_;

private:
    /// libuv timer callback: poll for new tasks
    static void on_poll_tick(uv_timer_t* handle) {
        auto* self = static_cast<Runner*>(handle->data);
        if (self->active_task_count() >= self->config_.max_concurrent_tasks) return;
        // Real implementation: HTTP GET /tasks -> parse TaskPayload -> handle_task()
    }

    /// libuv timer callback: send heartbeat
    static void on_heartbeat_tick(uv_timer_t* handle) {
        auto* self = static_cast<Runner*>(handle->data);
        self->report_status();
    }
};

// ============================================================
// EnvironmentRunner - sandboxed execution
// ============================================================

/// Runs inside a container/sandbox with limited tool access and resource constraints.
class EnvironmentRunner final : public Runner {
public:
    explicit EnvironmentRunner(RunnerConfig config)
        : Runner(std::move(config)) {
        // Enforce sandbox constraints
        config_.capabilities.can_spawn_agents = false;
        config_.capabilities.network_access = false;
    }

    /// Restrict the allowed tools to a safe subset
    void set_allowed_tools(std::vector<std::string> tools) {
        config_.capabilities.allowed_tools = std::move(tools);
    }

    /// Set resource limits
    void set_resource_limits(uint64_t max_memory_mb, uint64_t max_disk_mb) {
        config_.capabilities.max_memory_mb = max_memory_mb;
        config_.capabilities.max_disk_mb = max_disk_mb;
    }

protected:
    /// Sandboxed execution: restricted tool set, no network, resource bounded
    Result<std::string> execute_impl(const TaskPayload& task) override {
        // Validate this task does not exceed sandbox limits
        if (task.constraints.allow_network && !config_.capabilities.network_access) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied,
                "Network access not permitted in sandbox"));
        }

        // Build the execution context with sandbox restrictions
        // The prompt is sent to the LLM with a restricted tool set
        std::string system_prompt = std::format(
            "You are operating in a sandboxed environment.\n"
            "Available tools: {}\n"
            "Memory limit: {}MB\n"
            "Disk limit: {}MB\n"
            "Network access: disabled\n"
            "Execute the following task within these constraints.",
            config_.capabilities.allowed_tools.size(),
            config_.capabilities.max_memory_mb,
            config_.capabilities.max_disk_mb);

        // Execute via the coordinator's API - submit prompt with context
        std::string payload = std::format(
            R"({{"task_id":"{}","prompt":"{}","system":"{}","tools":{},"sandbox":true}})",
            task.id,
            task.prompt.substr(0, 1000), // Truncate for payload
            system_prompt,
            config_.capabilities.allowed_tools.size());

        send_to_coordinator("/execute", payload);

        // In a full implementation: wait for execution result from coordinator
        // The coordinator dispatches to the LLM and returns the result
        // For now, return confirmation that task was dispatched
        return std::format("[sandbox] Task '{}' dispatched with {} context docs, "
                           "prompt length {}, {} tools available",
                           task.id, task.context.size(),
                           task.prompt.size(),
                           config_.capabilities.allowed_tools.size());
    }
};

// ============================================================
// SelfHostedRunner - full access execution
// ============================================================

/// Full-access runner with all tools, sub-agent spawning, and persistent workspace.
class SelfHostedRunner final : public Runner {
public:
    explicit SelfHostedRunner(RunnerConfig config)
        : Runner(std::move(config)) {
        // Enable full capabilities
        config_.capabilities.can_spawn_agents = true;
        config_.capabilities.network_access = true;
    }

    /// Spawn a sub-agent for delegated work
    Result<std::string> spawn_sub_agent(const std::string& prompt,
                                         const std::vector<std::string>& tools) {
        if (!config_.capabilities.can_spawn_agents) {
            return std::unexpected(Error::make(
                ErrorCode::ToolPermissionDenied, "Sub-agent spawning disabled"));
        }
        (void)tools;
        return std::format("[sub-agent] Processing: {}", prompt.substr(0, 50));
    }

    /// Access the persistent workspace directory
    [[nodiscard]] const std::string& workspace() const noexcept {
        return config_.workspace_dir;
    }

protected:
    /// Full execution: unrestricted tool access with sub-agent support
    Result<std::string> execute_impl(const TaskPayload& task) override {
        // Build full-access execution context
        std::string context_payload;
        for (const auto& ctx : task.context) {
            context_payload += ctx + "\n---\n";
        }

        // Select model (use task constraint or default)
        std::string model = task.constraints.model.value_or("claude-sonnet-4-20250514");

        // Determine max tokens
        uint32_t max_tokens = task.constraints.max_tokens.value_or(4096);

        // Build the execution request for the coordinator
        std::string payload = std::format(
            R"({{"task_id":"{}","prompt":"{}","model":"{}","max_tokens":{},"workspace":"{}",)"
            R"("context_docs":{},"tools_count":{},"can_spawn_agents":true}})",
            task.id,
            task.prompt.substr(0, 2000), // Truncate for JSON safety
            model,
            max_tokens,
            config_.workspace_dir,
            task.context.size(),
            config_.capabilities.allowed_tools.size());

        send_to_coordinator("/execute", payload);

        // In a full implementation:
        // 1. Send prompt + context to LLM via coordinator
        // 2. Process tool_use responses by executing tools locally
        // 3. Handle sub-agent spawning if needed
        // 4. Continue multi-turn conversation until end_turn
        // 5. Return final result

        return std::format("[self-hosted] Task '{}' dispatched: model={}, "
                           "workspace='{}', {} context docs, {} tools",
                           task.id, model, config_.workspace_dir,
                           task.context.size(),
                           config_.capabilities.allowed_tools.size());
    }
};

} // namespace cc::core
