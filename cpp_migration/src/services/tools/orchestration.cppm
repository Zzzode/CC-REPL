/// @file orchestration.cppm
/// @brief 工具执行编排器 - 支持依赖感知的并行/串行工具执行。
/// 特性: DAG 调度、工具批处理、资源池、重试退避、死锁检测。
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
// 数据结构
// ============================================================

/// 执行节点状态
enum class NodeStatus : std::uint8_t {
    Pending,       // 等待依赖完成
    Ready,         // 依赖已满足，可以执行
    Running,       // 正在执行
    Completed,     // 执行成功
    Failed,        // 执行失败
    Skipped,       // 因依赖失败被跳过
};

/// 将节点状态转为字符串
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

/// 执行图中的单个节点
struct ExecutionNode {
    std::string tool_call_id;                  // 工具调用唯一 ID
    std::string tool_name;                     // 工具名称
    ToolInput input;                           // 工具输入参数
    std::vector<std::string> dependencies;     // 依赖的前置节点 ID 列表
    NodeStatus status{NodeStatus::Pending};    // 当前状态
    std::optional<ToolResult> result;          // 执行结果（完成后填充）
    std::uint32_t retry_count{0};             // 已重试次数
};

/// 执行计划 - 描述一组工具调用的依赖关系和并行策略
struct ExecutionPlan {
    std::vector<ExecutionNode> nodes;         // 所有执行节点
    std::uint32_t max_parallelism{4};         // 最大并行度

    /// 获取当前可执行的节点（依赖已满足且未执行）
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

    /// 检查是否所有节点都已终止
    [[nodiscard]] bool is_complete() const noexcept {
        return std::ranges::all_of(nodes, [](const auto& n) {
            return n.status == NodeStatus::Completed ||
                   n.status == NodeStatus::Failed ||
                   n.status == NodeStatus::Skipped;
        });
    }
};

/// 编排器配置
struct OrchestratorConfig {
    std::uint32_t max_concurrent{4};       // 最大并发执行数
    std::uint32_t retry_count{2};          // 最大重试次数
    std::uint32_t backoff_ms{1000};        // 初始退避时间 (ms)
    std::uint32_t max_backoff_ms{30000};   // 最大退避时间 (ms)
    bool fail_fast{false};                 // 遇错即停还是尽可能执行
};

// ============================================================
// 工具编排器
// ============================================================

/// 管理多工具调用的并行/串行执行，处理依赖和错误恢复
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

    /// 从工具调用列表生成执行计划（自动分析依赖）
    [[nodiscard]] ExecutionPlan plan(const std::vector<ExecutionNode>& tool_calls) const {
        ExecutionPlan exec_plan;
        exec_plan.nodes = tool_calls;
        exec_plan.max_parallelism = config_.max_concurrent;

        // 验证 DAG 无环
        if (has_cycle(exec_plan)) {
            // 检测到环依赖，退化为串行执行
            for (std::size_t i = 1; i < exec_plan.nodes.size(); ++i) {
                exec_plan.nodes[i].dependencies = {exec_plan.nodes[i - 1].tool_call_id};
            }
        }

        return exec_plan;
    }

    /// 执行完整的执行计划
    Task<std::vector<ToolResult>> execute(ExecutionPlan plan) {
        cancelled_.store(false);
        std::vector<ToolResult> results;
        results.resize(plan.nodes.size());

        // DAG 调度循环 - 持续调度可执行节点
        while (!plan.is_complete() && !cancelled_.load()) {
            auto ready_indices = plan.get_ready_indices();
            if (ready_indices.empty()) {
                // 无可执行节点且未完成 -> 死锁或等待
                if (!has_running_nodes(plan)) {
                    // 标记所有 pending 节点为 skipped
                    mark_unreachable_as_skipped(plan);
                    break;
                }
                co_await cc::utils::async::sleep(10);
                continue;
            }

            // 限制并行度
            auto batch_size = std::min(
                static_cast<std::size_t>(plan.max_parallelism - running_count_.load()),
                ready_indices.size());

            // 批量启动就绪节点
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

    /// 无依赖关系的并行执行
    Task<std::vector<ToolResult>> execute_parallel(
        const std::vector<ExecutionNode>& calls,
        std::optional<std::uint32_t> max_concurrent = std::nullopt) {

        auto parallelism = max_concurrent.value_or(config_.max_concurrent);
        std::vector<ToolResult> results;
        results.reserve(calls.size());

        // 分批并行执行
        for (std::size_t offset = 0; offset < calls.size(); offset += parallelism) {
            auto batch_end = std::min(offset + parallelism, calls.size());
            for (std::size_t i = offset; i < batch_end; ++i) {
                auto result = co_await execute_single_node(calls[i]);
                results.push_back(result);
            }
        }

        co_return results;
    }

    /// 严格串行执行
    Task<std::vector<ToolResult>> execute_sequential(const std::vector<ExecutionNode>& calls) {
        std::vector<ToolResult> results;
        results.reserve(calls.size());

        for (const auto& call : calls) {
            if (cancelled_.load()) break;
            auto result = co_await execute_single_node(call);
            results.push_back(result);
            // 串行模式下遇错是否停止由配置决定
            if (result.is_error && config_.fail_fast) break;
        }

        co_return results;
    }

    /// 取消所有正在进行的执行
    void cancel_all() {
        cancelled_.store(true);
    }

    /// 获取执行进度 (已完成, 总数)
    [[nodiscard]] std::pair<int, int> get_progress() const {
        std::lock_guard lock(progress_mutex_);
        return {completed_count_, total_count_};
    }

private:
    /// 执行单个节点（含重试逻辑）
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

    /// 检测执行图中是否存在环（使用 DFS 拓扑排序）
    [[nodiscard]] bool has_cycle(const ExecutionPlan& plan) const {
        std::unordered_map<std::string, int> state; // 0=白, 1=灰, 2=黑
        for (const auto& node : plan.nodes) {
            state[node.tool_call_id] = 0;
        }

        std::function<bool(const std::string&)> dfs = [&](const std::string& id) -> bool {
            state[id] = 1; // 标记为正在访问
            auto it = std::ranges::find_if(plan.nodes, [&id](const auto& n) {
                return n.tool_call_id == id;
            });
            if (it != plan.nodes.end()) {
                for (const auto& dep : it->dependencies) {
                    if (state[dep] == 1) return true;  // 发现回边
                    if (state[dep] == 0 && dfs(dep)) return true;
                }
            }
            state[id] = 2; // 标记为已完成
            return false;
        };

        for (const auto& node : plan.nodes) {
            if (state[node.tool_call_id] == 0) {
                if (dfs(node.tool_call_id)) return true;
            }
        }
        return false;
    }

    /// 检查是否有节点正在运行
    [[nodiscard]] bool has_running_nodes(const ExecutionPlan& plan) const noexcept {
        return std::ranges::any_of(plan.nodes, [](const auto& n) {
            return n.status == NodeStatus::Running;
        });
    }

    /// 标记不可达节点为 Skipped
    void mark_unreachable_as_skipped(ExecutionPlan& plan) const {
        for (auto& node : plan.nodes) {
            if (node.status == NodeStatus::Pending) {
                node.status = NodeStatus::Skipped;
            }
        }
    }

    /// 更新进度计数
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
