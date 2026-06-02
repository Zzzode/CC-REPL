// C++23 Module: Task/todo tracking state management for the UI
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

export module cc.hooks.task_hooks;


export namespace cc::hooks {

// 任务状态枚举
enum class TaskStatus { Pending, InProgress, Completed };

// 任务优先级枚举
enum class TaskPriority { Low, Medium, High };

// 唯一任务标识符
using TaskId = std::string;

// 单个任务项
struct TaskItem {
    TaskId id;
    std::string content;
    TaskStatus status{TaskStatus::Pending};
    TaskPriority priority{TaskPriority::Medium};
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::optional<std::string> summary;               // 完成时的摘要说明
    std::vector<TaskId> dependencies;                 // 依赖的其他任务 ID

    // 是否为终态
    [[nodiscard]] auto is_terminal() const -> bool {
        return status == TaskStatus::Completed;
    }

    // 是否所有依赖已完成（需外部提供已完成集合）
    [[nodiscard]] auto dependencies_met(
        const std::vector<TaskItem>& all_tasks) const -> bool {
        return std::all_of(dependencies.begin(), dependencies.end(), [&](const TaskId& dep_id) {
            auto it = std::find_if(all_tasks.begin(), all_tasks.end(),
                [&](const auto& t) { return t.id == dep_id; });
            return it != all_tasks.end() && it->status == TaskStatus::Completed;
        });
    }

    // 优先级排序权重（越高越靠前）
    [[nodiscard]] auto sort_weight() const -> int {
        int w = 0;
        // 状态权重：InProgress > Pending > Completed
        switch (status) {
            case TaskStatus::InProgress: w += 300; break;
            case TaskStatus::Pending:    w += 200; break;
            case TaskStatus::Completed:  w += 100; break;
        }
        // 优先级权重
        switch (priority) {
            case TaskPriority::High:   w += 30; break;
            case TaskPriority::Medium: w += 20; break;
            case TaskPriority::Low:    w += 10; break;
        }
        return w;
    }
};

// 任务 hook 内部状态
struct TaskHookState {
    std::vector<TaskItem> tasks;
    std::optional<TaskId> active_task_id;   // 当前 InProgress 的任务 ID
};

// 状态变更监听回调
using TaskChangeCallback = std::function<void(const TaskHookState&)>;

// ─── TaskHook: 核心任务管理类 ───────────────────────────────────
class TaskHook {
public:
    TaskHook() = default;

    // 添加新任务，返回生成的唯一 ID
    [[nodiscard]] auto add_task(std::string_view content,
                                TaskPriority priority = TaskPriority::Medium)
        -> TaskId {
        auto id = generate_id();
        state_.tasks.push_back(TaskItem{
            .id = id,
            .content = std::string(content),
            .status = TaskStatus::Pending,
            .priority = priority,
            .created_at = std::chrono::system_clock::now(),
            .completed_at = std::nullopt,
            .summary = std::nullopt,
            .dependencies = {}
        });
        notify_change();
        return id;
    }

    // 添加带依赖关系的任务
    [[nodiscard]] auto add_task_with_deps(std::string_view content,
                                          TaskPriority priority,
                                          std::vector<TaskId> deps) -> TaskId {
        auto id = generate_id();
        state_.tasks.push_back(TaskItem{
            .id = id,
            .content = std::string(content),
            .status = TaskStatus::Pending,
            .priority = priority,
            .created_at = std::chrono::system_clock::now(),
            .completed_at = std::nullopt,
            .summary = std::nullopt,
            .dependencies = std::move(deps)
        });
        notify_change();
        return id;
    }

    // 更新任务状态，可附加完成摘要
    auto update_task(const TaskId& id, TaskStatus new_status,
                     std::optional<std::string> summary = std::nullopt)
        -> std::expected<void, std::string> {
        auto it = find_task(id);
        if (it == state_.tasks.end()) {
            return std::unexpected(std::format("Task '{}' not found", id));
        }

        // 切换到 InProgress 时，确保只有一个活跃任务
        if (new_status == TaskStatus::InProgress) {
            deactivate_current();
            state_.active_task_id = id;
        }

        it->status = new_status;
        if (new_status == TaskStatus::Completed) {
            it->completed_at = std::chrono::system_clock::now();
            it->summary = std::move(summary);
            // 如果是当前活跃任务，清除活跃标记
            if (state_.active_task_id == id) {
                state_.active_task_id = std::nullopt;
            }
        }
        notify_change();
        return {};
    }

    // 移除任务
    auto remove_task(const TaskId& id) -> bool {
        auto it = find_task(id);
        if (it == state_.tasks.end()) return false;
        if (state_.active_task_id == id) {
            state_.active_task_id = std::nullopt;
        }
        state_.tasks.erase(it);
        notify_change();
        return true;
    }

    // 获取当前活跃任务
    [[nodiscard]] auto get_active_task() const -> std::optional<TaskItem> {
        if (!state_.active_task_id) return std::nullopt;
        auto it = std::find_if(state_.tasks.begin(), state_.tasks.end(),
            [&](const auto& t) { return t.id == *state_.active_task_id; });
        if (it == state_.tasks.end()) return std::nullopt;
        return *it;
    }

    // 获取进度: (已完成数, 总数)
    [[nodiscard]] auto get_progress() const -> std::pair<int, int> {
        int completed = static_cast<int>(std::count_if(state_.tasks.begin(), state_.tasks.end(),
            [](const auto& t) { return t.status == TaskStatus::Completed; }));
        int total = static_cast<int>(state_.tasks.size());
        return {completed, total};
    }

    // 按状态筛选任务
    [[nodiscard]] auto get_tasks_by_status(TaskStatus status) const
        -> std::vector<TaskItem> {
        std::vector<TaskItem> result;
        std::copy_if(state_.tasks.begin(), state_.tasks.end(), std::back_inserter(result),
            [status](const auto& t) { return t.status == status; });
        return result;
    }

    // 重新排序：按权重降序排列
    auto reorder_tasks() -> void {
        std::sort(state_.tasks.begin(), state_.tasks.end(), [](const auto& a, const auto& b) {
            return a.sort_weight() > b.sort_weight();
        });
        notify_change();
    }

    // 获取全部任务（只读引用）
    [[nodiscard]] auto tasks() const -> const std::vector<TaskItem>& {
        return state_.tasks;
    }

    // 获取完整状态快照
    [[nodiscard]] auto state() const -> const TaskHookState& { return state_; }

    // 注册变更监听器，返回取消订阅的函数
    auto subscribe(TaskChangeCallback cb) -> std::function<void()> {
        auto id = next_listener_id_++;
        listeners_.emplace_back(id, std::move(cb));
        return [this, id]() {
            std::erase_if(listeners_,
                [id](const auto& p) { return p.first == id; });
        };
    }

    // 批量替换所有任务（用于从外部恢复状态）
    auto replace_all(std::vector<TaskItem> tasks) -> void {
        state_.tasks = std::move(tasks);
        state_.active_task_id = std::nullopt;
        // 查找是否有 InProgress 任务
        for (const auto& t : state_.tasks) {
            if (t.status == TaskStatus::InProgress) {
                state_.active_task_id = t.id;
                break;
            }
        }
        notify_change();
    }

private:
    TaskHookState state_;
    std::uint64_t id_counter_{0};
    std::uint64_t next_listener_id_{0};
    std::vector<std::pair<std::uint64_t, TaskChangeCallback>> listeners_;

    // 生成唯一 ID
    [[nodiscard]] auto generate_id() -> TaskId {
        return std::format("task_{}", ++id_counter_);
    }

    // 查找任务迭代器
    auto find_task(const TaskId& id) -> std::vector<TaskItem>::iterator {
        return std::ranges::find_if(state_.tasks,
            [&](const auto& t) { return t.id == id; });
    }

    // 将当前活跃任务恢复为 Pending
    auto deactivate_current() -> void {
        if (!state_.active_task_id) return;
        auto it = find_task(*state_.active_task_id);
        if (it != state_.tasks.end() && it->status == TaskStatus::InProgress) {
            it->status = TaskStatus::Pending;
        }
        state_.active_task_id = std::nullopt;
    }

    // 通知所有监听器
    auto notify_change() -> void {
        for (const auto& [_, cb] : listeners_) {
            if (cb) cb(state_);
        }
    }
};

} // namespace cc::hooks
