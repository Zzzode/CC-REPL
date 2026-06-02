/// @file coordinator_mode.cppm
/// @brief Coordinator mode - manages multiple runners, distributes tasks,
/// monitors health, and handles load balancing across runner instances.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <queue>
#include <mutex>
#include <atomic>

#include <uv.h>

export module cc.coordinator.coordinator_mode;

import cc.types.types;
import cc.entrypoints.runner;

export namespace cc::core {

// ============================================================
// Coordinator configuration
// ============================================================

/// Configuration for the coordinator mode
struct CoordinatorConfig {
    uint32_t max_runners = 16;                          // Maximum allowed runners
    std::chrono::milliseconds task_timeout{300000};     // Default 5 min per task
    std::chrono::milliseconds rebalance_interval{10000}; // Rebalance every 10s
    std::chrono::milliseconds health_check_interval{5000}; // Heartbeat check interval
    std::chrono::milliseconds dead_runner_threshold{30000}; // Mark dead after 30s silence
    uint16_t listen_port = 9090;                        // API listen port
};

// ============================================================
// RunnerInfo - metadata about a connected runner
// ============================================================

/// Information tracked for each connected runner
struct RunnerInfo {
    std::string id;                                     // Unique runner identifier
    std::string endpoint;                               // Runner's callback URL

    /// Runner capabilities (mirrors RunnerConfig::Capabilities)
    struct Capabilities {
        std::vector<std::string> allowed_tools;
        std::vector<std::string> models;
        uint64_t max_memory_mb = 4096;
        bool can_spawn_agents = false;
        bool network_access = true;
    } capabilities;

    uint32_t current_load = 0;                          // Active tasks count
    uint32_t max_load = 4;                              // Max concurrent tasks
    std::chrono::system_clock::time_point last_heartbeat; // Last health signal
    std::chrono::system_clock::time_point registered_at;

    /// Compute load factor (0.0 = idle, 1.0 = full)
    [[nodiscard]] double load_factor() const noexcept {
        if (max_load == 0) return 1.0;
        return static_cast<double>(current_load) / static_cast<double>(max_load);
    }

    /// Check if the runner has capacity for more work
    [[nodiscard]] bool has_capacity() const noexcept {
        return current_load < max_load;
    }

    /// Check if this runner supports the given tool
    [[nodiscard]] bool supports_tool(std::string_view tool) const {
        return std::ranges::find(capabilities.allowed_tools, tool) !=
               capabilities.allowed_tools.end();
    }
};

// ============================================================
// TaskId and task queue entry
// ============================================================

/// Strong task identifier type
struct TaskIdTag {};
using TaskId = StrongId<TaskIdTag>;

/// Internal task entry tracked by the coordinator
struct TaskEntry {
    TaskId id;
    TaskPayload payload;
    TaskStatus status = TaskStatus::Pending;
    std::optional<std::string> assigned_runner;         // Runner handling this task
    std::chrono::system_clock::time_point submitted_at;
    std::optional<std::chrono::system_clock::time_point> started_at;
    std::optional<TaskReport> report;                   // Result once completed
};

// ============================================================
// LoadBalancer - distributes tasks across runners
// ============================================================

/// Distributes incoming tasks to runners based on load and capabilities.
class LoadBalancer {
public:
    /// Select the best runner for a given task based on load and capabilities
    [[nodiscard]] std::optional<std::string> select_runner(
        const TaskPayload& task,
        const std::unordered_map<std::string, RunnerInfo>& runners) const {

        std::string best_id;
        double best_score = std::numeric_limits<double>::max();

        for (const auto& [id, runner] : runners) {
            // Skip runners that are at capacity
            if (!runner.has_capacity()) continue;

            // Check required tool support
            bool tools_ok = std::ranges::all_of(
                task.constraints.required_tools,
                [&](const auto& tool) { return runner.supports_tool(tool); });
            if (!tools_ok) continue;

            // Check network requirement
            if (task.constraints.allow_network && !runner.capabilities.network_access) {
                continue;
            }

            // Score by load factor (lower is better)
            double score = runner.load_factor();
            if (score < best_score) {
                best_score = score;
                best_id = id;
            }
        }

        if (best_id.empty()) return std::nullopt;
        return best_id;
    }

    /// Strategy for handling unassignable tasks
    enum class OverflowStrategy : uint8_t {
        Queue,   // Hold in queue until a runner frees up
        Reject,  // Reject immediately
        Retry,   // Retry after a delay
    };

    void set_overflow_strategy(OverflowStrategy strategy) { overflow_ = strategy; }
    [[nodiscard]] OverflowStrategy overflow_strategy() const noexcept { return overflow_; }

private:
    OverflowStrategy overflow_ = OverflowStrategy::Queue;
};

// ============================================================
// HealthChecker - monitors runner heartbeats
// ============================================================

/// Monitors runner health via heartbeat tracking. Removes dead runners
/// and triggers rebalancing of their assigned tasks.
class HealthChecker {
public:
    explicit HealthChecker(std::chrono::milliseconds threshold)
        : dead_threshold_(threshold) {}

    /// Check all runners and return IDs of dead ones
    [[nodiscard]] std::vector<std::string> check_health(
        const std::unordered_map<std::string, RunnerInfo>& runners) const {

        std::vector<std::string> dead;
        auto now = std::chrono::system_clock::now();

        for (const auto& [id, runner] : runners) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - runner.last_heartbeat);
            if (elapsed > dead_threshold_) {
                dead.push_back(id);
            }
        }
        return dead;
    }

    /// Update heartbeat timestamp for a runner
    static void record_heartbeat(RunnerInfo& runner) {
        runner.last_heartbeat = std::chrono::system_clock::now();
    }

    void set_threshold(std::chrono::milliseconds threshold) {
        dead_threshold_ = threshold;
    }

private:
    std::chrono::milliseconds dead_threshold_;
};

// ============================================================
// CoordinatorMode - main coordinator orchestrator
// ============================================================

/// Coordinator mode: manages a fleet of runners, accepts task submissions,
/// performs load balancing, and monitors runner health.
class CoordinatorMode {
public:
    explicit CoordinatorMode(CoordinatorConfig config)
        : config_(std::move(config))
        , health_checker_(config_.dead_runner_threshold)
        , next_task_id_(1) {}

    /// Start the coordinator (sets up libuv timers for health/rebalance)
    VoidResult start() {
        if (running_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Coordinator already running"));
        }
        loop_ = uv_default_loop();

        // Setup health check timer
        uv_timer_init(loop_, &health_timer_);
        health_timer_.data = this;
        uv_timer_start(&health_timer_, on_health_check,
                       config_.health_check_interval.count(),
                       config_.health_check_interval.count());

        // Setup rebalance timer
        uv_timer_init(loop_, &rebalance_timer_);
        rebalance_timer_.data = this;
        uv_timer_start(&rebalance_timer_, on_rebalance,
                       config_.rebalance_interval.count(),
                       config_.rebalance_interval.count());

        running_ = true;
        return {};
    }

    /// Stop the coordinator gracefully
    void stop() {
        if (!running_) return;
        running_ = false;
        uv_timer_stop(&health_timer_);
        uv_timer_stop(&rebalance_timer_);
    }

    /// Submit a new task for execution, returns a TaskId for tracking
    Result<TaskId> submit_task(TaskPayload payload) {
        std::lock_guard lock(mutex_);

        TaskId id{std::format("task-{}", next_task_id_++)};
        TaskEntry entry{
            .id = id,
            .payload = std::move(payload),
            .status = TaskStatus::Pending,
            .assigned_runner = std::nullopt,
            .submitted_at = std::chrono::system_clock::now(),
            .started_at = std::nullopt,
            .report = std::nullopt,
        };

        // Try auto-assignment immediately
        if (auto runner_id = balancer_.select_runner(entry.payload, runners_)) {
            entry.assigned_runner = *runner_id;
            entry.status = TaskStatus::Running;
            entry.started_at = std::chrono::system_clock::now();
            runners_[*runner_id].current_load++;
        } else {
            // Queue for later assignment
            pending_queue_.push(id);
        }

        tasks_[id.value] = std::move(entry);
        return id;
    }

    /// Get the current status of a task
    Result<TaskStatus> get_task_status(const TaskId& id) const {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(id.value);
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound, std::format("Task not found: {}", id.value)));
        }
        return it->second.status;
    }

    /// List all connected runners
    [[nodiscard]] std::vector<RunnerInfo> list_runners() const {
        std::lock_guard lock(mutex_);
        std::vector<RunnerInfo> result;
        result.reserve(runners_.size());
        for (const auto& [_, info] : runners_) {
            result.push_back(info);
        }
        return result;
    }

    /// Explicitly assign a task to a specific runner
    VoidResult assign_to_runner(const TaskId& task_id, const std::string& runner_id) {
        std::lock_guard lock(mutex_);

        auto task_it = tasks_.find(task_id.value);
        if (task_it == tasks_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound, "Task not found"));
        }
        auto runner_it = runners_.find(runner_id);
        if (runner_it == runners_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound, "Runner not found"));
        }
        if (!runner_it->second.has_capacity()) {
            return std::unexpected(Error::make(
                ErrorCode::OverloadedError, "Runner at capacity"));
        }

        task_it->second.assigned_runner = runner_id;
        task_it->second.status = TaskStatus::Running;
        task_it->second.started_at = std::chrono::system_clock::now();
        runner_it->second.current_load++;
        return {};
    }

    /// Auto-assign a task to the best available runner
    Result<std::string> auto_assign(const TaskId& task_id) {
        std::lock_guard lock(mutex_);

        auto task_it = tasks_.find(task_id.value);
        if (task_it == tasks_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound, "Task not found"));
        }

        auto runner_id = balancer_.select_runner(task_it->second.payload, runners_);
        if (!runner_id) {
            return std::unexpected(Error::make(
                ErrorCode::OverloadedError, "No runner available"));
        }

        task_it->second.assigned_runner = *runner_id;
        task_it->second.status = TaskStatus::Running;
        task_it->second.started_at = std::chrono::system_clock::now();
        runners_[*runner_id].current_load++;
        return *runner_id;
    }

    /// Register a new runner with the coordinator
    void register_runner(RunnerInfo info) {
        std::lock_guard lock(mutex_);
        info.last_heartbeat = std::chrono::system_clock::now();
        info.registered_at = std::chrono::system_clock::now();
        runners_[info.id] = std::move(info);
    }

    /// Record a heartbeat from a runner
    void heartbeat(const std::string& runner_id) {
        std::lock_guard lock(mutex_);
        auto it = runners_.find(runner_id);
        if (it != runners_.end()) {
            HealthChecker::record_heartbeat(it->second);
        }
    }

    /// Record task completion
    void complete_task(const TaskId& task_id, TaskReport report) {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(task_id.value);
        if (it == tasks_.end()) return;

        // Decrement runner load
        if (it->second.assigned_runner) {
            auto runner_it = runners_.find(*it->second.assigned_runner);
            if (runner_it != runners_.end() && runner_it->second.current_load > 0) {
                runner_it->second.current_load--;
            }
        }

        it->second.status = report.status;
        it->second.report = std::move(report);
    }

    // Accessors
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] size_t runner_count() const {
        std::lock_guard lock(mutex_);
        return runners_.size();
    }
    [[nodiscard]] size_t pending_task_count() const {
        std::lock_guard lock(mutex_);
        return pending_queue_.size();
    }

private:
    /// Health check callback: remove dead runners, requeue their tasks
    static void on_health_check(uv_timer_t* handle) {
        auto* self = static_cast<CoordinatorMode*>(handle->data);
        std::lock_guard lock(self->mutex_);

        auto dead = self->health_checker_.check_health(self->runners_);
        for (const auto& dead_id : dead) {
            // Requeue tasks assigned to dead runner
            for (auto& [_, task] : self->tasks_) {
                if (task.assigned_runner == dead_id && task.status == TaskStatus::Running) {
                    task.status = TaskStatus::Pending;
                    task.assigned_runner = std::nullopt;
                    self->pending_queue_.push(task.id);
                }
            }
            self->runners_.erase(dead_id);
        }
    }

    /// Rebalance callback: assign pending tasks to available runners
    static void on_rebalance(uv_timer_t* handle) {
        auto* self = static_cast<CoordinatorMode*>(handle->data);
        std::lock_guard lock(self->mutex_);

        while (!self->pending_queue_.empty()) {
            auto task_id = self->pending_queue_.front();
            auto task_it = self->tasks_.find(task_id.value);
            if (task_it == self->tasks_.end()) {
                self->pending_queue_.pop();
                continue;
            }

            auto runner_id = self->balancer_.select_runner(
                task_it->second.payload, self->runners_);
            if (!runner_id) break; // No capacity available

            task_it->second.assigned_runner = *runner_id;
            task_it->second.status = TaskStatus::Running;
            task_it->second.started_at = std::chrono::system_clock::now();
            self->runners_[*runner_id].current_load++;
            self->pending_queue_.pop();
        }
    }

    CoordinatorConfig config_;
    LoadBalancer balancer_;
    HealthChecker health_checker_;
    bool running_ = false;
    uint64_t next_task_id_;
    uv_loop_t* loop_ = nullptr;
    uv_timer_t health_timer_{};
    uv_timer_t rebalance_timer_{};

    mutable std::mutex mutex_;
    std::unordered_map<std::string, RunnerInfo> runners_;
    std::unordered_map<std::string, TaskEntry> tasks_;
    std::queue<TaskId> pending_queue_;
};

} // namespace cc::core
