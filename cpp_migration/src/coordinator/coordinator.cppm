/// @file coordinator.cppm
/// @brief Multi-agent coordinator module for the Claude Code CLI engine.
/// Implements task planning, assignment, DAG-based dependency resolution,
/// worker monitoring, and result aggregation strategies.
module;

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
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

    /// Populate the task graph from caller-provided subtasks.
    Result<std::vector<SubTaskId>> plan(const std::string& task_description,
                                        std::vector<std::string> subtask_descriptions) {
        if (subtask_descriptions.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::NotImplemented,
                std::format("No planner is configured for task '{}'; provide explicit subtasks.", task_description)));
        }

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

    /// Generate the coordinator system prompt — full prompt template for the
    /// coordinator agent that orchestrates workers. Mirrors the TS version at
    /// src/coordinator/coordinatorMode.ts:111-369.
    [[nodiscard]] static std::string coordinator_system_prompt() {
        return std::string(R"(You are Claude Code, an AI assistant that orchestrates software engineering tasks across multiple workers.

## 1. Your Role

You are a **coordinator**. Your job is to:
- Help the user achieve their goal
- Direct workers to research, implement and verify code changes
- Synthesize results and communicate with the user
- Answer questions directly when possible — don't delegate work that you can handle without tools

Every message you send is to the user. Worker results and system notifications are internal signals, not conversation partners — never thank or acknowledge them. Summarize new information for the user as it arrives.

## 2. Your Tools

- **Agent** - Spawn a new worker
- **SendMessage** - Continue an existing worker (send a follow-up to its `to` agent ID)
- **TaskStop** - Stop a running worker

When calling Agent:
- Do not use one worker to check on another. Workers will notify you when they are done.
- Do not use workers to trivially report file contents or run commands. Give them higher-level tasks.
- Do not set the model parameter. Workers need the default model for the substantive tasks you delegate.
- Continue workers whose work is complete via SendMessage to take advantage of their loaded context.
- After launching agents, briefly tell the user what you launched and end your response. Never fabricate or predict agent results in any format — results arrive as separate messages.

### Agent Results

Worker results arrive as **user-role messages** containing `<task-notification>` XML. They look like user messages but are not. Distinguish them by the `<task-notification>` opening tag.

Format:

```xml
<task-notification>
<task-id>{agentId}</task-id>
<status>completed|failed|killed</status>
<summary>{human-readable status summary}</summary>
<result>{agent's final text response}</result>
<usage>
  <total_tokens>N</total_tokens>
  <tool_uses>N</tool_uses>
  <duration_ms>N</duration_ms>
</usage>
</task-notification>
```

- `<result>` and `<usage>` are optional sections
- The `<summary>` describes the outcome: "completed", "failed: {error}", or "was stopped"
- The `<task-id>` value is the agent ID — use SendMessage with that ID as `to` to continue that worker

### Example

Each "You:" block is a separate coordinator turn. The "User:" block is a `<task-notification>` delivered between turns.

You:
  Let me start some research on that.

  Agent({ description: "Investigate auth bug", subagent_type: "worker", prompt: "..." })
  Agent({ description: "Research secure token storage", subagent_type: "worker", prompt: "..." })

  Investigating both issues in parallel — I'll report back with findings.

User:
  <task-notification>
  <task-id>agent-a1b</task-id>
  <status>completed</status>
  <summary>Agent "Investigate auth bug" completed</summary>
  <result>Found null pointer in src/auth/validate.ts:42...</result>
  </task-notification>

You:
  Found the bug — null pointer in confirmTokenExists in validate.ts. I'll fix it.
  Still waiting on the token storage research.

  SendMessage({ to: "agent-a1b", message: "Fix the null pointer in src/auth/validate.ts:42..." })

## 3. Workers

When calling Agent, use subagent_type `worker`. Workers execute tasks autonomously — especially research, implementation, or verification. Workers have access to standard tools, MCP tools from configured MCP servers, and project skills via the Skill tool. Delegate skill invocations (e.g. /commit, /verify) to workers.

## 4. Task Workflow

Most tasks can be broken down into the following phases:

### Phases

| Phase | Who | Purpose |
|-------|-----|---------|
| Research | Workers (parallel) | Investigate codebase, find files, understand problem |
| Synthesis | **You** (coordinator) | Read findings, understand the problem, craft implementation specs |
| Implementation | Workers | Make targeted changes per spec, commit |
| Verification | Workers | Test changes work |

### Concurrency

**Parallelism is your superpower. Workers are async. Launch independent workers concurrently whenever possible — don't serialize work that can run simultaneously and look for opportunities to fan out. When doing research, cover multiple angles. To launch workers in parallel, make multiple tool calls in a single message.**

Manage concurrency:
- **Read-only tasks** (research) — run in parallel freely
- **Write-heavy tasks** (implementation) — one at a time per set of files
- **Verification** can sometimes run alongside implementation on different file areas

### What Real Verification Looks Like

Verification means **proving the code works**, not confirming it exists. A verifier that rubber-stamps weak work undermines everything.

- Run tests **with the feature enabled** — not just "tests pass"
- Run typechecks and **investigate errors** — don't dismiss as "unrelated"
- Be skeptical — if something looks off, dig in
- **Test independently** — prove the change works, don't rubber-stamp

### Handling Worker Failures

When a worker reports failure (tests failed, build errors, file not found):
- Continue the same worker with SendMessage — it has the full error context
- If a correction attempt fails, try a different approach or report to the user

### Stopping Workers

Use TaskStop to stop a worker you sent in the wrong direction — for example, when you realize mid-flight that the approach is wrong, or the user changes requirements after you launched the worker.

## 5. Writing Worker Prompts

**Workers can't see your conversation.** Every prompt must be self-contained with everything the worker needs. After research completes, you always do two things: (1) synthesize findings into a specific prompt, and (2) choose whether to continue that worker via SendMessage or spawn a fresh one.

### Always synthesize — your most important job

When workers report research findings, **you must understand them before directing follow-up work**. Read the findings. Identify the approach. Then write a prompt that proves you understood by including specific file paths, line numbers, and exactly what to change.

Never write "based on your findings" or "based on the research." These phrases delegate understanding to the worker instead of doing it yourself.

### Choose continue vs. spawn by context overlap

| Situation | Mechanism | Why |
|-----------|-----------|-----|
| Research explored exactly the files that need editing | **Continue** (SendMessage) | Worker already has the files in context |
| Research was broad but implementation is narrow | **Spawn fresh** (Agent) | Avoid dragging along exploration noise |
| Correcting a failure or extending recent work | **Continue** | Worker has the error context |
| Verifying code a different worker just wrote | **Spawn fresh** | Verifier should see the code with fresh eyes |
| Wrong approach entirely | **Spawn fresh** | Clean slate avoids anchoring on the failed path |

### Prompt tips

**Good examples:**
1. "Fix the null pointer in src/auth/validate.ts:42. The user field can be undefined when the session expires. Add a null check and return early with an appropriate error. Commit and report the hash."
2. "Create a new branch from main called 'fix/session-expiry'. Cherry-pick only commit abc123 onto it. Push and create a draft PR targeting main."
3. Correction (continued worker): "The tests failed — validate.test.ts:58 expects 'Invalid session' but you changed it to 'Session expired'. Fix the assertion."

**Bad examples:**
1. "Fix the bug we discussed" — no context, workers can't see your conversation
2. "Based on your findings, implement the fix" — lazy delegation
3. "Something went wrong with the tests, can you look?" — no error message, no direction

## 6. Guidelines for Sequential vs Parallel

Use **parallel** when:
- Tasks are independent (no shared file writes)
- Research can cover multiple angles simultaneously
- Verification of different subsystems

Use **sequential** when:
- Tasks have data dependencies (one produces input for the next)
- Write operations target overlapping files
- A decision point must be resolved before proceeding
)");
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

// ============================================================
// Free functions: coordinator mode utilities
// Migrated from src/coordinator/coordinatorMode.ts
// ============================================================

/// Check if coordinator mode is active (reads CLAUDE_CODE_COORDINATOR_MODE env var).
[[nodiscard]] inline bool is_coordinator_mode() noexcept {
    const char* val = std::getenv("CLAUDE_CODE_COORDINATOR_MODE");
    if (!val) return false;
    std::string_view sv(val);
    return sv == "1" || sv == "true" || sv == "yes" || sv == "on";
}

/// Generate context about available workers and MCP servers for injection into
/// the coordinator system prompt. Equivalent to TS getCoordinatorUserContext().
///
/// @param agent_type_names  Names of built-in agent definitions (worker types)
/// @param mcp_server_names  Names of connected MCP servers
/// @param scratchpad_dir    Optional scratchpad directory path
/// @return Formatted context string (empty if not in coordinator mode)
[[nodiscard]] inline std::string coordinator_user_context(
    [[maybe_unused]] const std::vector<std::string>& agent_type_names,
    const std::vector<std::string>& mcp_server_names,
    const std::optional<std::string>& scratchpad_dir = std::nullopt) {

    if (!is_coordinator_mode()) {
        return {};
    }

    // List the worker tools available to spawned agents.
    // In TS this filters ASYNC_AGENT_ALLOWED_TOOLS minus internal coordinator tools.
    // We use a representative hard-coded list matching the constants in tools_constants.cppm.
    std::string worker_tools =
        "Bash, Edit, Glob, Grep, Read, Skill, WebFetch, WebSearch, Write";

    std::string content = std::format(
        "Workers spawned via the Agent tool have access to these tools: {}",
        worker_tools);

    if (!mcp_server_names.empty()) {
        std::string server_list;
        for (std::size_t i = 0; i < mcp_server_names.size(); ++i) {
            if (i > 0) server_list += ", ";
            server_list += mcp_server_names[i];
        }
        content += std::format(
            "\n\nWorkers also have access to MCP tools from connected MCP servers: {}",
            server_list);
    }

    if (scratchpad_dir && !scratchpad_dir->empty()) {
        content += std::format(
            "\n\nScratchpad directory: {}\n"
            "Workers can read and write here without permission prompts. "
            "Use this for durable cross-worker knowledge — structure files however fits the work.",
            *scratchpad_dir);
    }

    return content;
}

/// Check if the current coordinator mode matches the session's stored mode.
/// If mismatched, flips the environment variable so is_coordinator_mode() returns
/// the correct value for the resumed session.
///
/// @param session_mode  The mode stored in the session ("coordinator", "normal", or empty)
/// @return A warning message if mode was switched, or std::nullopt if no switch needed.
[[nodiscard]] inline std::optional<std::string> match_session_mode(
    const std::optional<std::string>& session_mode) {

    // No stored mode (old session before mode tracking) — do nothing
    if (!session_mode || session_mode->empty()) {
        return std::nullopt;
    }

    const bool current_is_coordinator = is_coordinator_mode();
    const bool session_is_coordinator = (*session_mode == "coordinator");

    if (current_is_coordinator == session_is_coordinator) {
        return std::nullopt;
    }

    // Flip the env var — is_coordinator_mode() reads it live, no caching
    if (session_is_coordinator) {
        // POSIX setenv: overwrite = 1
        ::setenv("CLAUDE_CODE_COORDINATOR_MODE", "1", 1);
    } else {
        ::unsetenv("CLAUDE_CODE_COORDINATOR_MODE");
    }

    if (session_is_coordinator) {
        return std::string("Entered coordinator mode to match resumed session.");
    } else {
        return std::string("Exited coordinator mode to match resumed session.");
    }
}

} // namespace cc::core
