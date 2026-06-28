module;
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module cc.hooks.tasks;

export namespace cc::hooks {

enum class TaskStatus {
  pending,
  in_progress,
  completed,
  cancelled,
  failed
};

struct Task {
  std::string id;
  std::string title;
  std::string description;
  TaskStatus status = TaskStatus::pending;
  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point updated_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::unordered_map<std::string, std::string> metadata;
};

using TaskChangeCallback = std::function<void(const Task&)>;
using TaskListChangeCallback = std::function<void()>;

class TasksHook {
public:
  TasksHook() = default;

  auto add_task(Task task) -> std::string {
    task.id = generate_task_id();
    task.created_at = std::chrono::system_clock::now();
    task.updated_at = task.created_at;
    tasks_[task.id] = std::move(task);
    if (list_callback_) {
      list_callback_();
    }
    return task.id;
  }

  auto update_task(std::string_view id, TaskStatus status) -> bool {
    if (auto it = tasks_.find(std::string(id)); it != tasks_.end()) {
      it->second.status = status;
      it->second.updated_at = std::chrono::system_clock::now();
      if (status == TaskStatus::completed) {
        it->second.completed_at = std::chrono::system_clock::now();
      }
      if (change_callback_) {
        change_callback_(it->second);
      }
      if (list_callback_) {
        list_callback_();
      }
      return true;
    }
    return false;
  }

  auto get_task(std::string_view id) const -> std::optional<Task> {
    if (auto it = tasks_.find(std::string(id)); it != tasks_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  auto get_tasks() const -> std::vector<Task> {
    std::vector<Task> result;
    result.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_) {
      result.push_back(task);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      return a.updated_at > b.updated_at;
    });
    return result;
  }

  auto get_pending_tasks() const -> std::vector<Task> {
    std::vector<Task> result;
    for (const auto& [id, task] : tasks_) {
      if (task.status != TaskStatus::completed && task.status != TaskStatus::cancelled) {
        result.push_back(task);
      }
    }
    return result;
  }

  auto delete_task(std::string_view id) -> bool {
    if (tasks_.erase(std::string(id)) > 0) {
      if (list_callback_) {
        list_callback_();
      }
      return true;
    }
    return false;
  }

  auto on_task_change(TaskChangeCallback callback) -> void {
    change_callback_ = std::move(callback);
  }

  auto on_list_change(TaskListChangeCallback callback) -> void {
    list_callback_ = std::move(callback);
  }

  auto set_hide_delay(std::chrono::milliseconds delay) -> void {
    hide_delay_ = delay;
  }

private:
  auto generate_task_id() -> std::string {
    static uint64_t counter = 0;
    return "task_" + std::to_string(++counter);
  }

  std::unordered_map<std::string, Task> tasks_;
  TaskChangeCallback change_callback_;
  TaskListChangeCallback list_callback_;
  std::chrono::milliseconds hide_delay_{5000};
};

}
