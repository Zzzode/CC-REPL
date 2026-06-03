// cc.hooks.scheduled_tasks — migrated from useScheduledTasks.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <functional>
#include <cstdint>
#include <ctime>
#include <sstream>

export module cc.hooks.scheduled_tasks;

export namespace cc::hooks::scheduled_tasks {

enum class ScheduleType {
    Once,
    Recurring,
    Cron
};

struct ScheduledTask {
    std::string id;
    std::string name;
    std::string description;
    ScheduleType type;
    std::string cron_expr;                          // For Cron type
    std::chrono::seconds interval{0};               // For Recurring type
    std::chrono::system_clock::time_point next_run;
    std::chrono::system_clock::time_point last_run;
    bool enabled{true};
    std::string command;                            // The action to execute
    int run_count{0};
    int max_runs{0};                               // 0 = unlimited
};

struct TaskResult {
    std::string task_id;
    std::string task_name;
    bool success;
    std::optional<std::string> error;
    std::string output;
    std::chrono::milliseconds duration;
    std::chrono::system_clock::time_point completed_at;
};

using TaskExecutor = std::function<TaskResult(const ScheduledTask&)>;

namespace detail {

struct SchedulerState {
    std::mutex mutex;
    std::vector<ScheduledTask> tasks;
    std::vector<TaskResult> history;
    TaskExecutor executor;
    std::uint64_t next_id{1};
    static constexpr int MAX_HISTORY = 100;
};

inline auto get_state() -> SchedulerState& {
    static SchedulerState state;
    return state;
}

inline auto generate_task_id() -> std::string {
    auto& state = get_state();
    return "task-" + std::to_string(state.next_id++);
}

/// Simple cron expression parser for "minute hour day month weekday" format.
/// Returns true if the given time matches the cron expression.
inline auto cron_field_matches(std::string_view field, int value, int min, int max) -> bool {
    if (field == "*") return true;
    std::stringstream items{std::string(field)};
    std::string item;
    while (std::getline(items, item, ',')) {
        if (item.empty()) continue;
        auto slash = item.find('/');
        int step = 1;
        if (slash != std::string::npos) {
            step = std::max(1, std::stoi(item.substr(slash + 1)));
            item = item.substr(0, slash);
        }
        int start = min;
        int end = max;
        if (item != "*") {
            auto dash = item.find('-');
            if (dash == std::string::npos) {
                start = end = std::stoi(item);
            } else {
                start = std::stoi(item.substr(0, dash));
                end = std::stoi(item.substr(dash + 1));
            }
        }
        if (value >= start && value <= end && ((value - start) % step == 0)) return true;
    }
    return false;
}

inline auto matches_cron(std::string_view cron_expr,
                         std::chrono::system_clock::time_point tp) -> bool {
    if (cron_expr == "*") cron_expr = "* * * * *";
    std::stringstream fields{std::string(cron_expr)};
    std::vector<std::string> parts;
    std::string field;
    while (fields >> field) parts.push_back(field);
    if (parts.size() != 5) return false;

    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    localtime_r(&tt, &local);
    return cron_field_matches(parts[0], local.tm_min, 0, 59) &&
           cron_field_matches(parts[1], local.tm_hour, 0, 23) &&
           cron_field_matches(parts[2], local.tm_mday, 1, 31) &&
           cron_field_matches(parts[3], local.tm_mon + 1, 1, 12) &&
           cron_field_matches(parts[4], local.tm_wday, 0, 6);
}

} // namespace detail

/// Schedule a new task. Returns the assigned task ID.
inline std::expected<std::string, std::string> schedule_task(ScheduledTask task) {
    if (task.name.empty()) {
        return std::unexpected(std::string{"Task name must not be empty"});
    }
    if (task.type == ScheduleType::Cron && task.cron_expr.empty()) {
        return std::unexpected(std::string{"Cron expression required for Cron-type tasks"});
    }
    if (task.type == ScheduleType::Recurring && task.interval.count() <= 0) {
        return std::unexpected(std::string{"Positive interval required for Recurring tasks"});
    }

    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    task.id = detail::generate_task_id();

    // Set initial next_run if not already set
    if (task.next_run == std::chrono::system_clock::time_point{}) {
        if (task.type == ScheduleType::Recurring) {
            task.next_run = std::chrono::system_clock::now() + task.interval;
        } else {
            task.next_run = std::chrono::system_clock::now();
        }
    }

    auto id = task.id;
    state.tasks.push_back(std::move(task));
    return id;
}

/// Cancel (remove) a scheduled task by ID.
inline bool cancel_task(std::string_view id) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    auto it = std::ranges::find_if(state.tasks,
        [id](const auto& t) { return t.id == id; });
    if (it == state.tasks.end()) return false;
    state.tasks.erase(it);
    return true;
}

/// Enable or disable a task by ID.
inline void enable_task(std::string_view id, bool enabled) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    for (auto& task : state.tasks) {
        if (task.id == id) {
            task.enabled = enabled;
            break;
        }
    }
}

/// Get all scheduled tasks.
inline std::vector<ScheduledTask> get_scheduled_tasks() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.tasks;
}

/// Get the next task that is due to run.
inline std::optional<ScheduledTask> get_next_task() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    auto now = std::chrono::system_clock::now();
    ScheduledTask* next = nullptr;

    for (auto& task : state.tasks) {
        if (!task.enabled) continue;
        if (task.max_runs > 0 && task.run_count >= task.max_runs) continue;
        if (task.next_run <= now) {
            if (!next || task.next_run < next->next_run) {
                next = &task;
            }
        }
    }

    if (next) return *next;
    return std::nullopt;
}

/// Run all tasks that are currently due. Returns results for each executed task.
inline std::vector<TaskResult> run_due_tasks() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    auto now = std::chrono::system_clock::now();
    std::vector<TaskResult> results;

    for (auto& task : state.tasks) {
        if (!task.enabled) continue;
        if (task.max_runs > 0 && task.run_count >= task.max_runs) continue;
        if (task.next_run > now) continue;

        auto start = std::chrono::steady_clock::now();

        TaskResult result;
        result.task_id = task.id;
        result.task_name = task.name;
        result.completed_at = std::chrono::system_clock::now();

        if (state.executor) {
            result = state.executor(task);
        } else {
            // No executor set — just mark as success with empty output
            result.success = true;
            result.output = "Task executed (no executor configured)";
        }

        auto end = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        // Update task state
        task.last_run = now;
        task.run_count++;

        // Calculate next run time
        if (task.type == ScheduleType::Recurring) {
            task.next_run = now + task.interval;
        } else if (task.type == ScheduleType::Once) {
            task.enabled = false; // One-shot, disable after execution
        }
        // For Cron: next_run should be computed from cron expression
        // (simplified: just add 60 seconds for the next minute check)
        if (task.type == ScheduleType::Cron) {
            task.next_run = now + std::chrono::seconds{60};
        }

        results.push_back(result);

        // Add to history (bounded)
        if (static_cast<int>(state.history.size()) >= detail::SchedulerState::MAX_HISTORY) {
            state.history.erase(state.history.begin());
        }
        state.history.push_back(result);
    }

    return results;
}

/// Set the task executor function (the callback that actually runs tasks).
inline void set_executor(TaskExecutor executor) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.executor = std::move(executor);
}

/// Get execution history.
inline std::vector<TaskResult> get_task_history() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.history;
}

/// Get a specific task by ID.
inline std::optional<ScheduledTask> get_task(std::string_view id) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    for (const auto& task : state.tasks) {
        if (task.id == id) return task;
    }
    return std::nullopt;
}

/// Clear all tasks.
inline void clear_all_tasks() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.tasks.clear();
}

} // namespace cc::hooks::scheduled_tasks
