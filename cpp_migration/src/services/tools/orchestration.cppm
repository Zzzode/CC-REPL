/// @file orchestration.cppm


module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <coroutine>
#include <format>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <ranges>
#include <queue>

export module cc.services.tool_orchestration;

import cc.types.types;
import cc.tools.tool;
import cc.utils.async;
import cc.utils.error;

export namespace cc::services::tool_orchestration {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using cc::core::ToolInput;
using cc::core::ToolResult;

// --- Callback types for dependency injection ---

/// Tool dispatch function: given a tool name and input, execute and return result.
using ToolDispatchFn = std::function<Task<ToolResult>(std::string_view, const ToolInput&)>;

/// Pre-hook: return true to allow, false to block.
using PreHookFn = std::function<bool(std::string_view tool_name, const ToolInput& input)>;

/// Post-hook: fire-and-forget notification after execution.
using PostHookFn = std::function<void(std::string_view tool_name, const ToolResult& result)>;

// ============================================================

// ============================================================


enum class NodeStatus : std::uint8_t {
    Pending,
    Ready,
    Running,
    Completed,
    Failed,
    Skipped,
};


[[nodiscard]] constexpr std::string_view node_status_to_string(NodeStatus s) noexcept {
    switch (s) {
        case NodeStatus::Pending:   return "pending";
        case NodeStatus::Ready:     return "ready";
        case NodeStatus::Running:   return "running";
        case NodeStatus::Completed: return "completed";
        case NodeStatus::Failed:    return "failed";
        case NodeStatus::Skipped:   return "skipped";
    }
    return "unknown";
}


struct ExecutionNode {
    std::string tool_call_id;
    std::string tool_name;
    ToolInput input;
    std::vector<std::string> dependencies;
    NodeStatus status{NodeStatus::Pending};
    std::optional<ToolResult> result;
    std::uint32_t retry_count{0};
};


struct ExecutionPlan {
    std::vector<ExecutionNode> nodes;
    std::uint32_t max_parallelism{4};


    [[nodiscard]] std::vector<std::size_t> get_ready_indices() const {
        std::vector<std::size_t> ready;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].status != NodeStatus::Pending) continue;
            bool deps_satisfied = std::ranges::all_of(
                nodes[i].dependencies, [this](const auto& dep_id) {
                    auto it = std::ranges::find_if(nodes, [&dep_id](const auto& n) {
                        return n.tool_call_id == dep_id;
                    });
                    return it != nodes.end() && it->status == NodeStatus::Completed;
                });
            if (deps_satisfied) {
                ready.push_back(i);
            }
        }
        return ready;
    }


    [[nodiscard]] bool is_complete() const noexcept {
        return std::ranges::all_of(nodes, [](const auto& n) {
            return n.status == NodeStatus::Completed ||
                   n.status == NodeStatus::Failed ||
                   n.status == NodeStatus::Skipped;
        });
    }
};


struct OrchestratorConfig {
    std::uint32_t max_concurrent{4};
    std::uint32_t retry_count{2};
    std::uint32_t backoff_ms{1000};
    std::uint32_t max_backoff_ms{30000};
    bool fail_fast{false};
};

// ============================================================

// ============================================================


class ToolOrchestrator {
public:
    explicit ToolOrchestrator(ToolDispatchFn dispatch_fn,
                              OrchestratorConfig config = {},
                              PreHookFn pre_hook = nullptr,
                              PostHookFn post_hook = nullptr)
        : config_(std::move(config))
        , dispatch_fn_(std::move(dispatch_fn))
        , pre_hook_(std::move(pre_hook))
        , post_hook_(std::move(post_hook))
        , cancelled_(false)
        , running_count_(0) {}

    ~ToolOrchestrator() = default;

    ToolOrchestrator(const ToolOrchestrator&) = delete;
    ToolOrchestrator& operator=(const ToolOrchestrator&) = delete;
    ToolOrchestrator(ToolOrchestrator&&) noexcept = delete;
    ToolOrchestrator& operator=(ToolOrchestrator&&) noexcept = delete;


    [[nodiscard]] ExecutionPlan plan(const std::vector<ExecutionNode>& tool_calls) const {
        ExecutionPlan exec_plan;
        exec_plan.nodes = tool_calls;
        exec_plan.max_parallelism = config_.max_concurrent;


        if (has_cycle(exec_plan)) {

            for (std::size_t i = 1; i < exec_plan.nodes.size(); ++i) {
                exec_plan.nodes[i].dependencies = {exec_plan.nodes[i - 1].tool_call_id};
            }
        }

        return exec_plan;
    }


    Task<std::vector<ToolResult>> execute(ExecutionPlan plan) {
        cancelled_.store(false);
        std::vector<ToolResult> results;
        results.resize(plan.nodes.size());


        while (!plan.is_complete() && !cancelled_.load()) {
            auto ready_indices = plan.get_ready_indices();
            if (ready_indices.empty()) {

                if (!has_running_nodes(plan)) {

                    mark_unreachable_as_skipped(plan);
                    break;
                }
                co_await cc::utils::async::sleep(10);
                continue;
            }


            auto batch_size = std::min(
                static_cast<std::size_t>(plan.max_parallelism - running_count_.load()),
                ready_indices.size());


            for (std::size_t i = 0; i < batch_size; ++i) {
                auto idx = ready_indices[i];
                plan.nodes[idx].status = NodeStatus::Running;
                running_count_++;

                auto result = co_await execute_single_node(plan.nodes[idx]);
                plan.nodes[idx].result = result;
                results[idx] = result;

                if (result.is_error) {
                    plan.nodes[idx].status = NodeStatus::Failed;
                    if (config_.fail_fast) {
                        cancel_all();
                        break;
                    }
                } else {
                    plan.nodes[idx].status = NodeStatus::Completed;
                }
                running_count_--;
            }
        }

        co_return results;
    }


    Task<std::vector<ToolResult>> execute_parallel(
        const std::vector<ExecutionNode>& calls,
        std::optional<std::uint32_t> max_concurrent = std::nullopt) {

        auto parallelism = max_concurrent.value_or(config_.max_concurrent);
        std::vector<ToolResult> results;
        results.reserve(calls.size());


        for (std::size_t offset = 0; offset < calls.size(); offset += parallelism) {
            auto batch_end = std::min(offset + parallelism, calls.size());
            for (std::size_t i = offset; i < batch_end; ++i) {
                auto result = co_await execute_single_node(calls[i]);
                results.push_back(result);
            }
        }

        co_return results;
    }


    Task<std::vector<ToolResult>> execute_sequential(const std::vector<ExecutionNode>& calls) {
        std::vector<ToolResult> results;
        results.reserve(calls.size());

        for (const auto& call : calls) {
            if (cancelled_.load()) break;
            auto result = co_await execute_single_node(call);
            results.push_back(result);

            if (result.is_error && config_.fail_fast) break;
        }

        co_return results;
    }


    void cancel_all() {
        cancelled_.store(true);
    }


    [[nodiscard]] std::pair<int, int> get_progress() const {
        std::lock_guard lock(progress_mutex_);
        return {completed_count_, total_count_};
    }

private:

    Task<ToolResult> execute_single_node(const ExecutionNode& node) {
        // Pre-hook permission check
        if (pre_hook_ && !pre_hook_(node.tool_name, node.input)) {
            co_return ToolResult::error(
                std::format("tool '{}' blocked by pre-hook", node.tool_name));
        }

        std::uint32_t attempts = 0;
        std::uint32_t backoff = config_.backoff_ms;

        while (attempts <= config_.retry_count) {
            if (cancelled_.load()) {
                co_return ToolResult{.content = {}, .is_error = true};
            }

            auto result = co_await dispatch_fn_(node.tool_name, node.input);

            if (!result.is_error) {
                if (post_hook_) post_hook_(node.tool_name, result);
                update_progress(1);
                co_return result;
            }

            attempts++;
            if (attempts <= config_.retry_count) {
                co_await cc::utils::async::sleep(backoff);
                backoff = std::min(backoff * 2, config_.max_backoff_ms);
            }
        }

        co_return ToolResult::error(
            std::format("tool '{}' failed after {} retries", node.tool_name, config_.retry_count));
    }


    [[nodiscard]] bool has_cycle(const ExecutionPlan& plan) const {
        std::unordered_map<std::string, int> state;
        for (const auto& node : plan.nodes) {
            state[node.tool_call_id] = 0;
        }

        std::function<bool(const std::string&)> dfs = [&](const std::string& id) -> bool {
            state[id] = 1;
            auto it = std::ranges::find_if(plan.nodes, [&id](const auto& n) {
                return n.tool_call_id == id;
            });
            if (it != plan.nodes.end()) {
                for (const auto& dep : it->dependencies) {
                    if (state[dep] == 1) return true;
                    if (state[dep] == 0 && dfs(dep)) return true;
                }
            }
            state[id] = 2;
            return false;
        };

        for (const auto& node : plan.nodes) {
            if (state[node.tool_call_id] == 0) {
                if (dfs(node.tool_call_id)) return true;
            }
        }
        return false;
    }


    [[nodiscard]] bool has_running_nodes(const ExecutionPlan& plan) const noexcept {
        return std::ranges::any_of(plan.nodes, [](const auto& n) {
            return n.status == NodeStatus::Running;
        });
    }


    void mark_unreachable_as_skipped(ExecutionPlan& plan) const {
        for (auto& node : plan.nodes) {
            if (node.status == NodeStatus::Pending) {
                node.status = NodeStatus::Skipped;
            }
        }
    }


    void update_progress(int delta) {
        std::lock_guard lock(progress_mutex_);
        completed_count_ += delta;
    }

    OrchestratorConfig config_;
    ToolDispatchFn dispatch_fn_;
    PreHookFn pre_hook_;
    PostHookFn post_hook_;
    std::atomic<bool> cancelled_;
    std::atomic<std::uint32_t> running_count_;
    mutable std::mutex progress_mutex_;
    int completed_count_{0};
    int total_count_{0};
};

} // namespace cc::services::tool_orchestration
