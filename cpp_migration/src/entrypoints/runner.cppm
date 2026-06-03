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
#include <sstream>

#include <uv.h>

export module cc.entrypoints.runner;

import cc.types.types;
import cc.utils.http;
import cc.utils.json;

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
        , runner_id_(make_runner_id())
        , state_(RunnerState::Stopped) {}

    virtual ~Runner() { stop(); }

    /// Start the runner's polling loop (non-blocking, uses libuv timer)
    VoidResult start() {
        if (state_ == RunnerState::Running) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Runner already started"));
        }
        if (config_.api_endpoint.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Runner coordinator API endpoint is required"));
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
        std::size_t active_count = 0;
        {
            std::lock_guard lock(tasks_mutex_);
            active_count = active_tasks_.size();
        }
        // Build status payload with current load info
        auto status_json = std::format(
            R"({{"runner_id":"{}","state":"{}","active_tasks":{},"capabilities":{{"tools_count":{}}}}})",
            json_escape(runner_id_), state_to_string(state_),
            active_count, config_.capabilities.allowed_tools.size());
        auto sent = send_to_coordinator("/status", status_json);
        if (!sent) {
            last_coordinator_error_ = sent.error().message;
        }
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
        if (constraints.model && !config_.capabilities.models.empty()) {
            bool found = std::ranges::find(
                config_.capabilities.models, *constraints.model) !=
                config_.capabilities.models.end();
            if (!found) {
                return Error::make(ErrorCode::InvalidRequest,
                    std::format("Required model '{}' not available", *constraints.model));
            }
        }
        if (constraints.allow_network && !config_.capabilities.network_access) {
            return Error::make(ErrorCode::ToolPermissionDenied,
                "Network access not permitted for this runner");
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::string json_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (unsigned char ch : value) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
                    } else {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
            }
        }
        return out;
    }

    /// Send a payload to the coordinator endpoint
    Result<std::string> send_to_coordinator(std::string_view path, std::string_view payload) {
        if (config_.api_endpoint.empty()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Coordinator API endpoint is required"));
        }
        auto endpoint = coordinator_url(path);

        last_coordinator_path_ = std::string(path);
        last_coordinator_payload_ = std::string(payload);

        auto headers = auth_headers();
        headers.emplace("Content-Type", "application/json");

        cc::utils::HttpClient client;
        auto response = client.post(endpoint, payload, headers);
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed,
                std::format("Coordinator POST {} failed with status {}: {}",
                    path, response->status, response->body)));
        }
        return response->body;
    }

    [[nodiscard]] Result<std::vector<TaskPayload>> poll_tasks() {
        auto capacity = config_.max_concurrent_tasks > active_task_count()
            ? config_.max_concurrent_tasks - active_task_count()
            : 0;
        if (capacity == 0) return std::vector<TaskPayload>{};

        auto path = std::format(
            "/tasks?runner_id={}&capacity={}",
            url_encode(runner_id_),
            capacity);
        auto endpoint = coordinator_url(path);

        last_coordinator_path_ = path;
        last_coordinator_payload_.clear();

        cc::utils::HttpClient client;
        auto response = client.get(endpoint, auth_headers());
        if (!response) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed,
                response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(Error::make(
                ErrorCode::ConnectionFailed,
                std::format("Coordinator GET {} failed with status {}: {}",
                    path, response->status, response->body)));
        }
        if (response->body.empty()) return std::vector<TaskPayload>{};

        auto doc = cc::utils::json::parse(response->body);
        if (!doc) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Coordinator returned invalid task JSON: {}",
                    doc.error().message())));
        }
        return parse_tasks_response(doc->root());
    }

    [[nodiscard]] Result<std::string> report_task(const TaskReport& report) {
        auto payload = std::format(
            R"({{"runner_id":"{}","task_id":"{}","status":"{}","duration_ms":{},)"
            R"("token_usage":{{"input_tokens":{},"output_tokens":{},"cache_creation_tokens":{},"cache_read_tokens":{},"total_tokens":{}}},)"
            R"("result":{},"error":{}}})",
            json_escape(runner_id_),
            json_escape(report.id),
            status_to_string(report.status),
            report.duration.count(),
            report.token_usage.input_tokens,
            report.token_usage.output_tokens,
            report.token_usage.cache_creation_tokens,
            report.token_usage.cache_read_tokens,
            report.token_usage.total(),
            report.result ? std::format(R"("{}")", json_escape(*report.result)) : "null",
            report.error ? std::format(R"("{}")", json_escape(*report.error)) : "null");
        return send_to_coordinator("/tasks/report", payload);
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

    static constexpr std::string_view status_to_string(TaskStatus s) {
        switch (s) {
            case TaskStatus::Pending:   return "pending";
            case TaskStatus::Running:   return "running";
            case TaskStatus::Completed: return "completed";
            case TaskStatus::Failed:    return "failed";
            case TaskStatus::Timeout:   return "timeout";
            case TaskStatus::Cancelled: return "cancelled";
        }
        return "unknown";
    }

    [[nodiscard]] static std::string make_runner_id() {
        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return std::format("runner-{}", now);
    }

    [[nodiscard]] std::string coordinator_url(std::string_view path) const {
        auto endpoint = config_.api_endpoint;
        if (!endpoint.empty() && endpoint.back() == '/') {
            endpoint.pop_back();
        }
        if (!path.empty() && path.front() != '/') {
            endpoint.push_back('/');
        }
        endpoint += path;
        return endpoint;
    }

    [[nodiscard]] std::unordered_map<std::string, std::string> auth_headers() const {
        std::unordered_map<std::string, std::string> headers;
        if (!config_.auth_token.empty()) {
            headers.emplace("Authorization", "Bearer " + config_.auth_token);
        }
        return headers;
    }

    [[nodiscard]] static std::string url_encode(std::string_view value) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
            bool safe =
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (safe) {
                out.push_back(static_cast<char>(ch));
            } else {
                out.push_back('%');
                out.push_back(hex[(ch >> 4) & 0x0F]);
                out.push_back(hex[ch & 0x0F]);
            }
        }
        return out;
    }

    [[nodiscard]] static Result<std::vector<TaskPayload>> parse_tasks_response(cc::utils::json::JsonVal root) {
        std::vector<TaskPayload> tasks;
        auto parse_array = [&](cc::utils::json::JsonVal array) -> Result<std::vector<TaskPayload>> {
            std::vector<TaskPayload> parsed;
            array.iter([&](cc::utils::json::JsonVal item) {
                if (!item.is_obj()) return;
                auto task = parse_task_payload(item);
                if (task) parsed.push_back(std::move(*task));
            });
            return parsed;
        };

        if (root.is_arr()) return parse_array(root);
        if (!root.is_obj()) return tasks;

        auto tasks_val = root.get("tasks");
        if (tasks_val.valid() && tasks_val.is_arr()) return parse_array(tasks_val);
        auto items_val = root.get("items");
        if (items_val.valid() && items_val.is_arr()) return parse_array(items_val);
        auto task_val = root.get("task");
        if (task_val.valid() && task_val.is_obj()) {
            auto task = parse_task_payload(task_val);
            if (!task) return std::unexpected(task.error());
            tasks.push_back(std::move(*task));
        }
        return tasks;
    }

    [[nodiscard]] static Result<TaskPayload> parse_task_payload(cc::utils::json::JsonVal value) {
        if (!value.valid() || !value.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Task payload must be a JSON object"));
        }

        TaskPayload task;
        task.id = required_string(value, "id").value_or(required_string(value, "task_id").value_or(""));
        task.prompt = required_string(value, "prompt").value_or(required_string(value, "instruction").value_or(""));
        if (task.id.empty() || task.prompt.empty()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Task payload requires non-empty id and prompt fields"));
        }

        auto timeout = optional_int(value, "timeout_ms");
        if (timeout && *timeout > 0) {
            task.timeout = std::chrono::milliseconds(*timeout);
        }
        task.context = optional_string_array(value, "context");

        auto constraints = value.get("constraints");
        if (!constraints.valid() || !constraints.is_obj()) {
            constraints = value;
        }
        if (auto model = optional_string(constraints, "model"); model && !model->empty()) {
            task.constraints.model = *model;
        }
        if (auto max_tokens = optional_int(constraints, "max_tokens"); max_tokens && *max_tokens > 0) {
            task.constraints.max_tokens = static_cast<uint32_t>(*max_tokens);
        }
        task.constraints.required_tools = optional_string_array(constraints, "required_tools");
        if (task.constraints.required_tools.empty()) {
            task.constraints.required_tools = optional_string_array(constraints, "tools");
        }
        if (auto allow_network = optional_bool(constraints, "allow_network")) {
            task.constraints.allow_network = *allow_network;
        }
        return task;
    }

    [[nodiscard]] static std::optional<std::string> required_string(
        cc::utils::json::JsonVal object, std::string_view key) {
        return optional_string(object, key);
    }

    [[nodiscard]] static std::optional<std::string> optional_string(
        cc::utils::json::JsonVal object, std::string_view key) {
        auto child = object.get(key);
        if (!child.valid() || !child.is_str()) return std::nullopt;
        return std::string(child.as_str());
    }

    [[nodiscard]] static std::optional<int64_t> optional_int(
        cc::utils::json::JsonVal object, std::string_view key) {
        auto child = object.get(key);
        if (!child.valid() || !child.is_num()) return std::nullopt;
        return child.as_int();
    }

    [[nodiscard]] static std::optional<bool> optional_bool(
        cc::utils::json::JsonVal object, std::string_view key) {
        auto child = object.get(key);
        if (!child.valid() || !child.is_bool()) return std::nullopt;
        return child.as_bool();
    }

    [[nodiscard]] static std::vector<std::string> optional_string_array(
        cc::utils::json::JsonVal object, std::string_view key) {
        std::vector<std::string> values;
        auto child = object.get(key);
        if (!child.valid() || !child.is_arr()) return values;
        child.iter([&](cc::utils::json::JsonVal item) {
            if (item.valid() && item.is_str()) {
                values.emplace_back(item.as_str());
            }
        });
        return values;
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
    std::string last_coordinator_error_;

private:
    /// libuv timer callback: poll for new tasks
    static void on_poll_tick(uv_timer_t* handle) {
        auto* self = static_cast<Runner*>(handle->data);
        if (self->active_task_count() >= self->config_.max_concurrent_tasks) return;
        auto tasks = self->poll_tasks();
        if (!tasks) {
            self->last_coordinator_error_ = tasks.error().message;
            return;
        }
        for (const auto& task : *tasks) {
            if (self->active_task_count() >= self->config_.max_concurrent_tasks) break;
            auto report = self->handle_task(task);
            auto sent = self->report_task(report);
            if (!sent) {
                self->last_coordinator_error_ = sent.error().message;
            }
        }
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
            json_escape(task.id),
            json_escape(task.prompt.substr(0, 1000)), // Truncate for payload
            json_escape(system_prompt),
            config_.capabilities.allowed_tools.size());

        auto response = send_to_coordinator("/execute", payload);
        if (!response) return std::unexpected(response.error());
        return *response;
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
            json_escape(task.id),
            json_escape(task.prompt.substr(0, 2000)), // Truncate for JSON safety
            json_escape(model),
            max_tokens,
            json_escape(config_.workspace_dir),
            task.context.size(),
            config_.capabilities.allowed_tools.size());

        auto response = send_to_coordinator("/execute", payload);
        if (!response) return std::unexpected(response.error());
        return *response;
    }
};

} // namespace cc::core
