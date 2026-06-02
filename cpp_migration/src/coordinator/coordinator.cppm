/// @file coordinator.cppm
/// @brief Multi-agent coordinator module for the Claude Code CLI engine.
/// Implements task planning, assignment, DAG-based dependency resolution,
/// worker monitoring, and result aggregation strategies.
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
#include <queue>
#include <mutex>

export module cc.coordinator.coordinator;

import cc.types.types;
import cc.coordinator.swarm;

export namespace cc::core {

// ============================================================
// Coordinator strategy enum
// ============================================================

/// Strategy for distributing work across agents
enum class CoordinatorStrategy : std::uint8_t {
    Sequential,  // Execute subtasks one after another
    Parallel,    // Execute independent subtasks concurrently
    Pipeline,    // Chain subtasks in a streaming pipeline
    MapReduce,   // Fan-out map tasks, then reduce results
};

/// Convert strategy to display string
[[nodiscard]] constexpr std::string_view strategy_to_string(CoordinatorStrategy s) noexcept {
    switch (s) {
        case CoordinatorStrategy::Sequential: return "sequential";
        case CoordinatorStrategy::Parallel:   return "parallel";
        case CoordinatorStrategy::Pipeline:   return "pipeline";
        case CoordinatorStrategy::MapReduce:  return "map_reduce";
    }
    return "unknown";
}

// ============================================================
// SubTask definition
// ============================================================

/// Status of an individual subtask
enum class SubTaskStatus : std::uint8_t {
    Pending,      // Not yet started
    Assigned,     // Assigned to a worker
    InProgress,   // Worker is actively processing
    Completed,    // Successfully finished
    Failed,       // Failed during execution
    Blocked,      // Waiting on dependencies
};

/// Strong ID for subtasks
struct SubTaskIdTag {};
using SubTaskId = StrongId<SubTaskIdTag>;

/// A unit of work within a coordinated multi-agent operation
struct SubTask {
    SubTaskId id;
    std::string description;                   // What this subtask should accomplish
    std::vector<SubTaskId> dependencies;       // IDs of subtasks that must finish first
    std::optional<WorkerId> assignee;          // Worker handling this subtask
    SubTaskStatus status = SubTaskStatus::Pending;
    std::optional<std::string> result;         // Output when completed
    std::optional<std::string> error;          // Error details if failed
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> started_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;

    /// Check if all dependencies are in the given completed set
    [[nodiscard]] bool deps_satisfied(const std::unordered_set<std::string>& completed) const {
        return std::ranges::all_of(dependencies,
            [&](const SubTaskId& dep) { return completed.contains(dep.value); });
    }
};

// ============================================================
// TaskGraph - DAG of subtasks with dependency resolution
// ============================================================

/// Directed acyclic graph managing subtask dependencies and execution order
class TaskGraph {
public:
    /// Add a subtask to the graph
    void add_task(SubTask task) {
        auto id = task.id.value;
        tasks_[id] = std::move(task);
    }

    /// Add a dependency edge: `dependent` requires `dependency` to complete first
    VoidResult add_dependency(const SubTaskId& dependent, const SubTaskId& dependency) {
        if (!tasks_.contains(dependent.value) || !tasks_.contains(dependency.value)) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Task ID not found in graph"));
        }
        tasks_[dependent.value].dependencies.push_back(dependency);
        return {};
    }

    /// Get all tasks whose dependencies are fully satisfied
    [[nodiscard]] std::vector<SubTask*> get_ready_tasks() {
        std::vector<SubTask*> ready;
        auto completed = get_completed_ids();

        for (auto& [id, task] : tasks_) {
            if (task.status == SubTaskStatus::Pending && task.deps_satisfied(completed)) {
                ready.push_back(&task);
            }
        }
        return ready;
    }

    /// Mark a task as completed with its result
    VoidResult mark_complete(const SubTaskId& task_id, std::string result) {
        auto it = tasks_.find(task_id.value);
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Task not found"));
        }
        it->second.status = SubTaskStatus::Completed;
        it->second.result = std::move(result);
        it->second.completed_at = std::chrono::system_clock::now();
        return {};
    }

    /// Mark a task as failed
    VoidResult mark_failed(const SubTaskId& task_id, std::string error) {
        auto it = tasks_.find(task_id.value);
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Task not found"));
        }
        it->second.status = SubTaskStatus::Failed;
        it->second.error = std::move(error);
        it->second.completed_at = std::chrono::system_clock::now();
        return {};
    }

    /// Check if all tasks in the graph are completed or failed
    [[nodiscard]] bool is_complete() const {
        return std::ranges::all_of(tasks_ | std::views::values,
            [](const SubTask& t) {
                return t.status == SubTaskStatus::Completed ||
                       t.status == SubTaskStatus::Failed;
            });
    }

    /// Produce a topological ordering of task IDs (Kahn's algorithm)
    [[nodiscard]] Result<std::vector<SubTaskId>> topological_sort() const {
        // Build in-degree map
        std::unordered_map<std::string, std::uint32_t> in_degree;
        std::unordered_map<std::string, std::vector<std::string>> dependents;

        for (const auto& [id, task] : tasks_) {
            in_degree[id] += 0;  // Ensure entry exists
            for (const auto& dep : task.dependencies) {
                in_degree[id]++;
                dependents[dep.value].push_back(id);
            }
        }

        // Collect zero in-degree nodes
        std::queue<std::string> queue;
        for (const auto& [id, deg] : in_degree) {
            if (deg == 0) queue.push(id);
        }

        std::vector<SubTaskId> sorted;
        while (!queue.empty()) {
            auto current = queue.front();
            queue.pop();
            sorted.push_back(SubTaskId{current});

            for (const auto& dep : dependents[current]) {
                if (--in_degree[dep] == 0) {
                    queue.push(dep);
                }
            }
        }

        // Cycle detection
        if (sorted.size() != tasks_.size()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Cycle detected in task graph"));
        }
        return sorted;
    }

    /// Get a task by ID
    [[nodiscard]] SubTask* get_task(const SubTaskId& id) {
        auto it = tasks_.find(id.value);
        return it != tasks_.end() ? &it->second : nullptr;
    }

    /// Get total number of tasks
    [[nodiscard]] std::size_t size() const noexcept { return tasks_.size(); }

    /// Get number of completed tasks
    [[nodiscard]] std::size_t completed_count() const {
        return std::ranges::count_if(tasks_ | std::views::values,
            [](const SubTask& t) { return t.status == SubTaskStatus::Completed; });
    }

private:
    std::unordered_map<std::string, SubTask> tasks_;

    /// Collect IDs of all completed tasks for dependency checks
    [[nodiscard]] std::unordered_set<std::string> get_completed_ids() const {
        std::unordered_set<std::string> completed;
        for (const auto& [id, task] : tasks_) {
            if (task.status == SubTaskStatus::Completed) {
                completed.insert(id);
            }
        }
        return completed;
    }
};

// ============================================================
// ResultAggregator - collects and merges worker outputs
// ============================================================

/// Collects results from multiple workers and merges them into a final output
class ResultAggregator {
public:
    /// Store a result from a specific worker
    void collect(const WorkerId& worker_id, std::string result) {
        std::lock_guard lock(mutex_);
        results_[worker_id.value].push_back(std::move(result));
    }

    /// Merge all collected results into a single combined output
    [[nodiscard]] std::string merge_results() const {
        std::lock_guard lock(mutex_);
        std::string merged;
        for (const auto& [worker_id, outputs] : results_) {
            merged += std::format("=== Worker: {} ===\n", worker_id);
            for (const auto& output : outputs) {
                merged += output;
                merged += "\n";
            }
        }
        return merged;
    }

    /// Detect and resolve conflicting results from different workers.
    /// Returns pairs of (worker_id_a, worker_id_b) that produced contradictions.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> resolve_conflicts() const {
        std::lock_guard lock(mutex_);
        std::vector<std::pair<std::string, std::string>> conflicts;
        auto keys = results_ | std::views::keys;
        auto key_vec = std::vector<std::string>(keys.begin(), keys.end());

        // Pairwise comparison for contradictions (simple hash-based detection)
        for (std::size_t i = 0; i < key_vec.size(); ++i) {
            for (std::size_t j = i + 1; j < key_vec.size(); ++j) {
                auto& results_a = results_.at(key_vec[i]);
                auto& results_b = results_.at(key_vec[j]);
                // Flag as conflict if results are non-empty but completely different
                if (!results_a.empty() && !results_b.empty() &&
                    results_a.back() != results_b.back()) {
                    conflicts.emplace_back(key_vec[i], key_vec[j]);
                }
            }
        }
        return conflicts;
    }

    /// Get number of workers that have submitted results
    [[nodiscard]] std::size_t worker_count() const {
        std::lock_guard lock(mutex_);
        return results_.size();
    }

    /// Clear all collected results
    void clear() {
        std::lock_guard lock(mutex_);
        results_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::string>> results_;
};

// ============================================================
// Coordinator - orchestrates multi-agent work
// ============================================================

/// High-level coordinator that plans, assigns, monitors, and aggregates
/// multi-agent work using a task graph and swarm manager.
class Coordinator {
public:
    explicit Coordinator(SwarmManager& swarm,
                         CoordinatorStrategy strategy = CoordinatorStrategy::Parallel)
        : swarm_(swarm), strategy_(strategy) {}

    /// Break a high-level task into subtasks and populate the task graph.
    /// In production, this would invoke LLM planning; here we provide the structure.
    Result<std::vector<SubTaskId>> plan(const std::string& task_description,
                                        std::vector<std::string> subtask_descriptions) {
        std::vector<SubTaskId> ids;
        std::uint32_t counter = 0;

        for (auto& desc : subtask_descriptions) {
            SubTaskId id{std::format("subtask-{}", ++counter)};
            SubTask subtask{
                .id = id,
                .description = std::move(desc),
                .dependencies = {},
                .assignee = std::nullopt,
                .status = SubTaskStatus::Pending,
                .result = std::nullopt,
                .error = std::nullopt,
                .created_at = std::chrono::system_clock::now(),
                .started_at = std::nullopt,
                .completed_at = std::nullopt,
            };
            graph_.add_task(std::move(subtask));
            ids.push_back(id);
        }
        return ids;
    }

    /// Assign a subtask to a specific worker, sending XML task notification
    VoidResult assign(const SubTaskId& subtask_id, const WorkerId& worker_id) {
        auto* task = graph_.get_task(subtask_id);
        if (!task) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Subtask not found"));
        }
        task->assignee = worker_id;
        task->status = SubTaskStatus::Assigned;
        task->started_at = std::chrono::system_clock::now();

        // Send XML task notification to the worker
        return swarm_.broadcast(format_task_notification(*task));
    }

    /// Monitor progress and return completion ratio
    [[nodiscard]] double monitor() const {
        auto total = graph_.size();
        if (total == 0) return 1.0;
        return static_cast<double>(graph_.completed_count()) / static_cast<double>(total);
    }

    /// Rebalance work: reassign failed tasks to available workers
    Result<std::uint32_t> rebalance() {
        auto ready = graph_.get_ready_tasks();
        auto states = swarm_.get_worker_states();
        std::uint32_t reassigned = 0;

        // Find idle workers
        std::vector<std::string> idle_workers;
        for (const auto& [id, state] : states) {
            if (state == WorkerState::Idle) {
                idle_workers.push_back(id);
            }
        }

        // Assign ready tasks to idle workers
        for (auto* task : ready) {
            if (idle_workers.empty()) break;
            task->assignee = WorkerId{idle_workers.back()};
            task->status = SubTaskStatus::Assigned;
            task->started_at = std::chrono::system_clock::now();
            idle_workers.pop_back();
            ++reassigned;
        }
        return reassigned;
    }

    /// Collect and summarize all results
    [[nodiscard]] std::string summarize() const {
        return aggregator_.merge_results();
    }

    /// Submit a result for a completed subtask
    VoidResult submit_result(const SubTaskId& subtask_id,
                             const WorkerId& worker_id,
                             std::string result) {
        aggregator_.collect(worker_id, result);
        return graph_.mark_complete(subtask_id, std::move(result));
    }

    /// Access the underlying task graph
    [[nodiscard]] TaskGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const TaskGraph& graph() const noexcept { return graph_; }

    /// Access the result aggregator
    [[nodiscard]] ResultAggregator& aggregator() noexcept { return aggregator_; }

    /// Get the current strategy
    [[nodiscard]] CoordinatorStrategy strategy() const noexcept { return strategy_; }

    /// Check if all planned work is complete
    [[nodiscard]] bool is_done() const { return graph_.is_complete(); }

    /// Generate the coordinator system prompt injection for worker agents
    [[nodiscard]] std::string coordinator_system_prompt() const {
        return std::format(
            "<coordinator-context>\n"
            "  <strategy>{}</strategy>\n"
            "  <total-tasks>{}</total-tasks>\n"
            "  <completed-tasks>{}</completed-tasks>\n"
            "  <instructions>\n"
            "    You are a worker agent in a multi-agent coordination system.\n"
            "    Execute your assigned task completely and report results.\n"
            "    Do not deviate from the task description.\n"
            "    Respond with a clear summary when done.\n"
            "  </instructions>\n"
            "</coordinator-context>",
            strategy_to_string(strategy_),
            graph_.size(),
            graph_.completed_count());
    }

    /// Format a progress update as XML for broadcasting
    [[nodiscard]] std::string format_progress_notification() const {
        std::string tasks_xml;
        // Build status for each active subtask
        auto states = swarm_.get_worker_states();
        for (const auto& [worker_id, state] : states) {
            tasks_xml += std::format(
                "    <worker id=\"{}\" state=\"{}\"/>\n",
                worker_id, worker_state_to_string(state));
        }

        return std::format(
            "<coordinator-progress>\n"
            "  <completion>{:.0f}%</completion>\n"
            "  <workers>\n"
            "{}"
            "  </workers>\n"
            "</coordinator-progress>",
            monitor() * 100.0,
            tasks_xml);
    }

private:
    SwarmManager& swarm_;
    CoordinatorStrategy strategy_;
    TaskGraph graph_;
    ResultAggregator aggregator_;

    /// Format a task assignment as XML notification to send to workers
    [[nodiscard]] static std::string format_task_notification(const SubTask& task) {
        std::string deps_xml;
        for (const auto& dep : task.dependencies) {
            deps_xml += std::format("    <dependency>{}</dependency>\n", dep.value);
        }

        return std::format(
            "<task-notification>\n"
            "  <id>{}</id>\n"
            "  <description>{}</description>\n"
            "  <status>{}</status>\n"
            "{}"
            "</task-notification>",
            task.id.value,
            task.description,
            task.status == SubTaskStatus::Assigned ? "assigned" : "pending",
            deps_xml.empty() ? "" : std::format("  <dependencies>\n{}  </dependencies>\n", deps_xml));
    }
};

} // namespace cc::core
