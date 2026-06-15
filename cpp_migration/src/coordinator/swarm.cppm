/// @file swarm.cppm
/// @brief Swarm/multi-agent backend system for the Claude Code CLI engine.
/// Defines backend concepts, in-process and tmux backends, worker lifecycle,
/// permission synchronization, and the SwarmManager orchestrator.
module;

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <concepts>
#include <format>
#include <chrono>
#include <mutex>
#include <atomic>
#include <coroutine>
#include <deque>
#include <set>

export module cc.coordinator.swarm;

import cc.types.types;
import cc.utils.bash_execution;

export namespace cc::core {

// ============================================================
// Worker state and agent handle
// ============================================================

/// Lifecycle state of a worker agent
enum class WorkerState : std::uint8_t {
    Idle,                  // Spawned but not yet assigned work
    Working,               // Actively processing a task
    WaitingForPermission,  // Blocked on permission approval
    Completed,             // Finished its assigned work
    Failed,                // Terminated due to error
};

/// Convert WorkerState to display string
[[nodiscard]] constexpr std::string_view worker_state_to_string(WorkerState state) noexcept {
    switch (state) {
        case WorkerState::Idle:                 return "idle";
        case WorkerState::Working:              return "working";
        case WorkerState::WaitingForPermission: return "waiting_for_permission";
        case WorkerState::Completed:            return "completed";
        case WorkerState::Failed:               return "failed";
    }
    return "unknown";
}

/// Strong ID for worker agents
struct WorkerIdTag {};
using WorkerId = StrongId<WorkerIdTag>;

/// Configuration for spawning a worker
struct WorkerConfig {
    std::string name;                          // Human-readable worker name
    std::string task_description;              // What this worker should do
    std::optional<std::string> working_dir;    // Worker's working directory
    std::optional<std::uint32_t> timeout_ms;   // Max execution time
    std::vector<std::string> allowed_tools;    // Tool whitelist (empty = all)
};

/// Handle to a running worker agent
struct AgentHandle {
    WorkerId id;
    std::string name;
    WorkerState state = WorkerState::Idle;
    std::chrono::system_clock::time_point spawned_at;
    std::optional<std::string> last_output;    // Most recent output from worker
    std::optional<std::string> error_message;  // Error if state == Failed

    /// Check if the worker has finished (success or failure)
    [[nodiscard]] bool is_terminal() const noexcept {
        return state == WorkerState::Completed || state == WorkerState::Failed;
    }
};

// ============================================================
// Swarm configuration
// ============================================================

/// Backend type selection for agent execution
enum class BackendType : std::uint8_t {
    InProcess,  // Agents as coroutines in same process
    Tmux,       // Agents in separate tmux panes
};

/// Top-level swarm configuration
struct SwarmConfig {
    std::uint32_t max_workers = 4;             // Maximum concurrent workers
    BackendType backend_type = BackendType::InProcess;
    bool shared_permissions = true;            // Share permission decisions across workers
    bool shared_memory = false;                // Enable shared context memory
    std::optional<std::uint32_t> global_timeout_ms;  // Timeout for entire swarm operation
};

// ============================================================
// Permission synchronization
// ============================================================

/// Synchronizes permission decisions across all workers in a swarm.
/// When one worker gets a permission granted/denied, all workers benefit.
class PermissionSync {
public:
    /// Record a permission decision for sharing
    void record_decision(const std::string& tool_name,
                         const std::string& resource,
                         bool allowed) {
        std::lock_guard lock(mutex_);
        auto key = make_key(tool_name, resource);
        if (allowed) {
            deny_set_.erase(key);
            allow_set_.insert(std::move(key));
        } else {
            allow_set_.erase(key);
            deny_set_.insert(std::move(key));
        }
    }

    /// Query whether a permission has been previously decided
    [[nodiscard]] std::optional<bool> query(const std::string& tool_name,
                                            const std::string& resource) const {
        std::lock_guard lock(mutex_);
        auto key = make_key(tool_name, resource);
        if (allow_set_.contains(key)) return true;
        if (deny_set_.contains(key)) return false;
        return std::nullopt;  // No cached decision
    }

    /// Check if a specific tool+resource is explicitly allowed
    [[nodiscard]] bool is_allowed(const std::string& tool_name,
                                  const std::string& resource) const {
        auto result = query(tool_name, resource);
        return result.value_or(false);
    }

    /// Get count of recorded decisions
    [[nodiscard]] std::size_t decision_count() const {
        std::lock_guard lock(mutex_);
        return allow_set_.size() + deny_set_.size();
    }

    /// Clear all cached decisions
    void reset() {
        std::lock_guard lock(mutex_);
        allow_set_.clear();
        deny_set_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::set<std::string> allow_set_;   // tool:resource pairs that are allowed
    std::set<std::string> deny_set_;    // tool:resource pairs that are denied

    /// Construct a composite key for the lookup sets
    [[nodiscard]] static std::string make_key(const std::string& tool,
                                              const std::string& resource) {
        return std::format("{}:{}", tool, resource);
    }
};

// ============================================================
// SwarmBackend concept - defines how agents are spawned
// ============================================================

/// Concept constraining backend implementations for agent execution
template <typename B>
concept SwarmBackend = requires(B backend, WorkerConfig config, WorkerId id, std::string msg) {
    { backend.spawn(config) } -> std::same_as<Result<AgentHandle>>;
    { backend.terminate(id) } -> std::same_as<VoidResult>;
    { backend.send_input(id, msg) } -> std::same_as<VoidResult>;
    { backend.read_output(id) } -> std::same_as<Result<std::string>>;
    { backend.get_state(id) } -> std::same_as<WorkerState>;
};

// ============================================================
// InProcessBackend - agents as coroutines in same process
// ============================================================

/// Backend running agents as cooperative coroutines within the same process.
/// Uses libuv event loop for scheduling and IO multiplexing.
class InProcessBackend {
public:
    explicit InProcessBackend(std::uint32_t max_concurrency = 4)
        : max_concurrency_(max_concurrency) {}

    /// Spawn a new in-process worker coroutine
    [[nodiscard]] Result<AgentHandle> spawn(const WorkerConfig& config) {
        if (workers_.size() >= max_concurrency_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Worker limit reached ({}/{})", workers_.size(), max_concurrency_)));
        }

        auto id = generate_worker_id();
        AgentHandle handle{
            .id = id,
            .name = config.name,
            .state = WorkerState::Idle,
            .spawned_at = std::chrono::system_clock::now(),
            .last_output = std::nullopt,
            .error_message = std::nullopt,
        };

        workers_[id.value] = handle;
        output_buffers_[id.value] = {};
        return handle;
    }

    /// Terminate a running worker
    [[nodiscard]] VoidResult terminate(const WorkerId& id) {
        auto it = workers_.find(id.value);
        if (it == workers_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Worker not found"));
        }
        it->second.state = WorkerState::Failed;
        it->second.error_message = "Terminated by swarm manager";
        return {};
    }

    /// Send input/instructions to a worker
    [[nodiscard]] VoidResult send_input(const WorkerId& id, const std::string& message) {
        auto it = workers_.find(id.value);
        if (it == workers_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Worker not found"));
        }
        input_queues_[id.value].push_back(message);
        return {};
    }

    /// Read accumulated output from a worker
    [[nodiscard]] Result<std::string> read_output(const WorkerId& id) {
        auto it = output_buffers_.find(id.value);
        if (it == output_buffers_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Worker not found"));
        }
        // Drain buffer into single string
        std::string combined;
        for (auto& chunk : it->second) {
            combined += chunk;
        }
        it->second.clear();
        return combined;
    }

    /// Query current state of a worker
    [[nodiscard]] WorkerState get_state(const WorkerId& id) const {
        auto it = workers_.find(id.value);
        if (it == workers_.end()) return WorkerState::Failed;
        return it->second.state;
    }

private:
    std::uint32_t max_concurrency_;
    std::unordered_map<std::string, AgentHandle> workers_;
    std::unordered_map<std::string, std::deque<std::string>> output_buffers_;
    std::unordered_map<std::string, std::deque<std::string>> input_queues_;
    std::uint64_t next_id_ = 1;

    /// Generate a unique worker ID
    [[nodiscard]] WorkerId generate_worker_id() {
        return WorkerId{std::format("worker-inproc-{}", next_id_++)};
    }
};

// ============================================================
// TmuxBackend - agents in separate tmux panes
// ============================================================

/// Backend running agents in separate tmux panes for full process isolation.
/// Communicates via tmux send-keys/capture-pane mechanisms.
class TmuxBackend {
public:
    explicit TmuxBackend(const std::string& session_name = "cc-swarm")
        : session_name_(session_name) {}

    /// Spawn a new worker in a tmux pane
    [[nodiscard]] Result<AgentHandle> spawn(const WorkerConfig& config) {
        auto id = generate_worker_id();
        auto pane_name = std::format("{}-{}", session_name_, id.value);

        // Create tmux window for this worker
        auto create_cmd = std::format(
            "tmux new-window -t {} -n {} -d 2>/dev/null || "
            "tmux new-session -d -s {} -n {} 2>/dev/null",
            session_name_, pane_name, session_name_, pane_name);
        exec_shell(create_cmd);

        // Send the task description as initial command
        if (!config.task_description.empty()) {
            auto send_cmd = std::format(
                "tmux send-keys -t {}:{} '{}' Enter",
                session_name_, pane_name, escape_for_shell(config.task_description));
            exec_shell(send_cmd);
        }

        AgentHandle handle{
            .id = id,
            .name = config.name,
            .state = WorkerState::Working,
            .spawned_at = std::chrono::system_clock::now(),
            .last_output = std::nullopt,
            .error_message = std::nullopt,
        };

        pane_map_[id.value] = pane_name;
        workers_[id.value] = handle;
        return handle;
    }

    /// Terminate a tmux pane worker
    [[nodiscard]] VoidResult terminate(const WorkerId& id) {
        auto it = workers_.find(id.value);
        if (it == workers_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Worker not found"));
        }
        // Kill the tmux pane
        auto pane_it = pane_map_.find(id.value);
        if (pane_it != pane_map_.end()) {
            auto kill_cmd = std::format(
                "tmux kill-window -t {}:{} 2>/dev/null",
                session_name_, pane_it->second);
            exec_shell(kill_cmd);
        }
        it->second.state = WorkerState::Failed;
        it->second.error_message = "Terminated by swarm manager";
        pane_map_.erase(id.value);
        return {};
    }

    /// Send input to a tmux pane via send-keys
    [[nodiscard]] VoidResult send_input(const WorkerId& id, const std::string& message) {
        auto pane_it = pane_map_.find(id.value);
        if (pane_it == pane_map_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Pane not found"));
        }
        auto cmd = std::format(
            "tmux send-keys -t {}:{} '{}' Enter",
            session_name_, pane_it->second, escape_for_shell(message));
        exec_shell(cmd);
        return {};
    }

    /// Read output from a tmux pane via capture-pane
    [[nodiscard]] Result<std::string> read_output(const WorkerId& id) {
        auto pane_it = pane_map_.find(id.value);
        if (pane_it == pane_map_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError, "Pane not found"));
        }
        auto cmd = std::format(
            "tmux capture-pane -t {}:{} -p 2>/dev/null",
            session_name_, pane_it->second);
        return exec_shell_capture(cmd);
    }

    /// Query current state of a tmux worker
    [[nodiscard]] WorkerState get_state(const WorkerId& id) const {
        auto it = workers_.find(id.value);
        if (it == workers_.end()) return WorkerState::Failed;
        return it->second.state;
    }

private:
    std::string session_name_;
    std::unordered_map<std::string, AgentHandle> workers_;
    std::unordered_map<std::string, std::string> pane_map_;  // worker_id -> pane name
    std::uint64_t next_id_ = 1;

    /// Generate a unique worker ID for tmux backend
    [[nodiscard]] WorkerId generate_worker_id() {
        return WorkerId{std::format("worker-tmux-{}", next_id_++)};
    }

    /// Execute a shell command (fire-and-forget)
    static void exec_shell(const std::string& cmd) {
        [[maybe_unused]] auto ret = std::system(cmd.c_str());
    }

    /// Execute a shell command and capture stdout
    [[nodiscard]] static std::string exec_shell_capture(const std::string& cmd) {
        std::string output;
        FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
        if (!pipe) return output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        cc::utils::bash::pclose_spawn(pipe);
        return output;
    }

    /// Escape single quotes for shell command embedding
    [[nodiscard]] static std::string escape_for_shell(const std::string& input) {
        std::string result;
        result.reserve(input.size());
        for (char c : input) {
            if (c == '\'') {
                result += "'\\''";
            } else {
                result += c;
            }
        }
        return result;
    }
};

// ============================================================
// SwarmManager - high-level swarm orchestrator
// ============================================================

/// Manages a swarm of worker agents, handling lifecycle, messaging,
/// and result aggregation. Parameterized by the backend implementation.
class SwarmManager {
public:
    explicit SwarmManager(SwarmConfig config = {})
        : config_(std::move(config)), backend_(config_.max_workers) {}

    /// Spawn a new worker with given configuration
    [[nodiscard]] Result<AgentHandle> spawn_worker(const WorkerConfig& worker_config) {
        if (handles_.size() >= config_.max_workers) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Maximum worker count ({}) reached", config_.max_workers)));
        }
        auto result = backend_.spawn(worker_config);
        if (result) {
            handles_[result->id.value] = *result;
        }
        return result;
    }

    /// Broadcast a message to all active workers
    VoidResult broadcast(const std::string& message) {
        for (auto& [id, handle] : handles_) {
            if (!handle.is_terminal()) {
                auto res = backend_.send_input(handle.id, message);
                if (!res) return res;  // Propagate first error
            }
        }
        return {};
    }

    /// Collect results from all completed workers
    [[nodiscard]] Result<std::vector<std::string>> collect_results() {
        std::vector<std::string> results;
        for (auto& [id, handle] : handles_) {
            if (handle.state == WorkerState::Completed) {
                auto output = backend_.read_output(handle.id);
                if (output) {
                    results.push_back(std::move(*output));
                }
            }
        }
        return results;
    }

    /// Cancel all running workers
    VoidResult cancel_all() {
        for (auto& [id, handle] : handles_) {
            if (!handle.is_terminal()) {
                backend_.terminate(handle.id);
                handle.state = WorkerState::Failed;
                handle.error_message = "Cancelled by swarm manager";
            }
        }
        return {};
    }

    /// Update the maximum worker limit
    void set_max_workers(std::uint32_t n) noexcept { config_.max_workers = n; }

    /// Get the current state of all workers
    [[nodiscard]] std::unordered_map<std::string, WorkerState> get_worker_states() const {
        std::unordered_map<std::string, WorkerState> states;
        for (const auto& [id, handle] : handles_) {
            states[id] = handle.state;
        }
        return states;
    }

    /// Access the shared permission sync instance
    [[nodiscard]] PermissionSync& permissions() noexcept { return permissions_; }
    [[nodiscard]] const PermissionSync& permissions() const noexcept { return permissions_; }

    /// Get current swarm configuration
    [[nodiscard]] const SwarmConfig& config() const noexcept { return config_; }

    /// Get number of active (non-terminal) workers
    [[nodiscard]] std::size_t active_worker_count() const {
        return static_cast<std::size_t>(std::count_if(
            handles_.begin(), handles_.end(),
            [](const auto& entry) { return !entry.second.is_terminal(); }));
    }

private:
    SwarmConfig config_;
    InProcessBackend backend_;
    PermissionSync permissions_;
    std::unordered_map<std::string, AgentHandle> handles_;
};

} // namespace cc::core
