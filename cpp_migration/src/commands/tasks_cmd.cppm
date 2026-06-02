/// @file tasks_cmd.cppm
/// @brief TasksCommand implementing the /tasks slash command.
/// List running tasks, show task output, stop/cancel tasks.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <chrono>

export module cc.commands.tasks_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// State of a background task
enum class TaskState : std::uint8_t {
    Running,
    Completed,
    Failed,
    Cancelled,
};

/// Information about a running or completed task
struct TaskInfo {
    std::string id;
    std::string description;
    TaskState state = TaskState::Running;
    std::chrono::system_clock::time_point started_at;
    std::optional<std::string> output;
    std::optional<std::string> error;
};

/// TasksCommand implements the /tasks slash command.
/// Manages background tasks: list, show output, stop/cancel.
class TasksCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "tasks",
            .description = "Manage running background tasks",
            .aliases = {"task"},
            .args = {
                CommandArg{.name = "action", .description = "list | show | stop | cancel",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "show", "stop", "cancel"}}},
                CommandArg{.name = "task_id", .description = "Task ID to act on",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "show", "stop", "cancel"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: list|show|stop|cancel", action)));
        }
        if ((action == "show" || action == "stop" || action == "cancel") && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /tasks {} <task_id>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_list());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_list());
        if (action == "show") return show_task(std::string(ctx.args[1]));
        if (action == "stop" || action == "cancel") return stop_task(std::string(ctx.args[1]));

        return CommandResult::success(format_list());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "show", "stop", "cancel"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& t : tasks_) {
            if (t.id.starts_with(partial)) {
                suggestions.push_back(t.id);
            }
        }
        return suggestions;
    }

    /// Register a new task (called by the agent/tool system)
    void add_task(TaskInfo task) { tasks_.push_back(std::move(task)); }

    /// Update task state
    void update_task(const std::string& id, TaskState state, std::optional<std::string> output = std::nullopt) {
        auto it = std::ranges::find_if(tasks_, [&](const auto& t) { return t.id == id; });
        if (it != tasks_.end()) {
            it->state = state;
            if (output) it->output = std::move(output);
        }
    }

private:
    std::vector<TaskInfo> tasks_;

    [[nodiscard]] static std::string_view state_str(TaskState state) {
        switch (state) {
            case TaskState::Running:   return "running";
            case TaskState::Completed: return "completed";
            case TaskState::Failed:    return "failed";
            case TaskState::Cancelled: return "cancelled";
        }
        return "unknown";
    }

    [[nodiscard]] std::string format_list() const {
        auto running = std::ranges::count_if(tasks_,
            [](const auto& t) { return t.state == TaskState::Running; });
        if (tasks_.empty()) return "No tasks.";

        std::string out = std::format("Tasks ({} running, {} total):\n", running, tasks_.size());
        for (const auto& t : tasks_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now() - t.started_at);
            out += std::format("  [{}] {} — {} ({}s)\n",
                state_str(t.state), t.id, t.description, elapsed.count());
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> show_task(const std::string& id) const {
        auto it = std::ranges::find_if(tasks_, [&](const auto& t) { return t.id == id; });
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Task '{}' not found.", id)));
        }
        std::string out = std::format("Task: {}\n  State: {}\n  Description: {}",
            it->id, state_str(it->state), it->description);
        if (it->output) out += std::format("\n  Output:\n{}", *it->output);
        if (it->error) out += std::format("\n  Error: {}", *it->error);
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] Result<CommandResult> stop_task(const std::string& id) {
        auto it = std::ranges::find_if(tasks_, [&](const auto& t) { return t.id == id; });
        if (it == tasks_.end()) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Task '{}' not found.", id)));
        }
        if (it->state != TaskState::Running) {
            return CommandResult::success(std::format("Task '{}' is not running (state: {}).",
                id, state_str(it->state)));
        }
        it->state = TaskState::Cancelled;
        return CommandResult::success(std::format("Task '{}' cancelled.", id));
    }
};

} // namespace cc::commands
