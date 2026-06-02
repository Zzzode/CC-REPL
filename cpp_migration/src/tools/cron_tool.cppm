// ScheduleCronTool - Cron-based periodic task scheduling and management
module;
#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cctype>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.cron;


export namespace cc::tools {

// Cron task actions
enum class CronAction {
    Create,
    Update,
    Pause,
    Resume,
    Delete,
    List,
    Get,
    Trigger,
};

constexpr auto cron_action_name(CronAction a) -> std::string_view {
    switch (a) {
        case CronAction::Create:  return "create";
        case CronAction::Update:  return "update";
        case CronAction::Pause:   return "pause";
        case CronAction::Resume:  return "resume";
        case CronAction::Delete:  return "delete";
        case CronAction::List:    return "list";
        case CronAction::Get:     return "get";
        case CronAction::Trigger: return "trigger";
        default:                  return "unknown";
    }
}

// Error types for cron operations
enum class CronError {
    InvalidAction,
    InvalidExpression,
    TaskNotFound,
    TaskAlreadyExists,
    NameEmpty,
    MessageEmpty,
    TooManyTasks,
    IntervalTooSmall,
    TimezoneInvalid,
    TaskPaused,
    PersistenceFailed,
};

constexpr auto format_error(CronError err) -> std::string_view {
    switch (err) {
        case CronError::InvalidAction:     return "Invalid cron action";
        case CronError::InvalidExpression:  return "Invalid cron expression (5-field format required)";
        case CronError::TaskNotFound:       return "Scheduled task not found";
        case CronError::TaskAlreadyExists:  return "Task with this name already exists";
        case CronError::NameEmpty:          return "Task name is empty";
        case CronError::MessageEmpty:       return "Task message/content is empty";
        case CronError::TooManyTasks:       return "Maximum task limit (10) reached";
        case CronError::IntervalTooSmall:   return "Minimum interval is 5 minutes";
        case CronError::TimezoneInvalid:    return "Invalid timezone identifier";
        case CronError::TaskPaused:         return "Task is currently paused";
        case CronError::PersistenceFailed:  return "Failed to persist task state";
        default:                            return "Unknown cron error";
    }
}

// Parsed cron expression (5-field: minute hour day-of-month month day-of-week)
struct CronExpression {
    std::string raw;
    std::array<std::string, 5> fields;  // minute, hour, dom, month, dow

    // Parse a 5-field cron expression string
    static auto parse(std::string_view expr) -> std::expected<CronExpression, CronError> {
        CronExpression result;
        result.raw = std::string(expr);

        // Split by whitespace
        std::vector<std::string> parts;
        std::string current;
        for (char c : expr) {
            if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    parts.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(std::move(current));

        if (parts.size() != 5) {
            return std::unexpected(CronError::InvalidExpression);
        }

        for (size_t i = 0; i < 5; ++i) {
            result.fields[i] = std::move(parts[i]);
        }

        // Basic validation of each field
        if (!validate_field(result.fields[0], 0, 59) ||  // minute
            !validate_field(result.fields[1], 0, 23) ||  // hour
            !validate_field(result.fields[2], 1, 31) ||  // day of month
            !validate_field(result.fields[3], 1, 12) ||  // month
            !validate_field(result.fields[4], 0, 6)) {   // day of week
            return std::unexpected(CronError::InvalidExpression);
        }

        return result;
    }

    // Check minimum interval constraint (>= 5 minutes)
    [[nodiscard]] bool meets_minimum_interval() const {
        // If minute field is */N where N < 5, reject it
        if (fields[0].starts_with("*/")) {
            auto interval_str = fields[0].substr(2);
            int interval = std::atoi(interval_str.c_str());
            if (interval > 0 && interval < 5) return false;
        }
        return true;
    }

private:
    static bool validate_field(const std::string& field, int min_val, int max_val) {
        if (field == "*") return true;
        if (field.starts_with("*/")) {
            auto step = std::atoi(field.substr(2).c_str());
            return step > 0 && step <= max_val;
        }
        // Simple numeric check
        if (std::all_of(field.begin(), field.end(), [](unsigned char c) { return std::isdigit(c); })) {
            int val = std::atoi(field.c_str());
            return val >= min_val && val <= max_val;
        }
        // Allow comma-separated values and ranges
        return !field.empty();
    }
};

// Cron task state
enum class CronTaskState {
    Active,
    Paused,
};

// Scheduled task structure
struct CronTask {
    std::string id;
    std::string name;
    std::string message;            // Task content to execute
    CronExpression expression;
    std::string timezone;           // IANA timezone (e.g., "Asia/Shanghai")
    CronTaskState state{CronTaskState::Active};
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> last_run;
    std::optional<std::chrono::system_clock::time_point> next_run;
    size_t run_count{0};
};

// Cron task request
struct CronRequest {
    CronAction action;
    std::optional<std::string> task_id;
    std::optional<std::string> name;
    std::optional<std::string> message;
    std::optional<std::string> cron_expression;
    std::optional<std::string> timezone;
};

// Cron task store
class CronStore {
public:
    static constexpr size_t kMaxTasks = 10;

    auto create(std::string name, std::string message, CronExpression expr, std::string timezone)
        -> std::expected<CronTask*, CronError>
    {
        if (name.empty()) return std::unexpected(CronError::NameEmpty);
        if (message.empty()) return std::unexpected(CronError::MessageEmpty);
        if (tasks_.size() >= kMaxTasks) return std::unexpected(CronError::TooManyTasks);
        if (!expr.meets_minimum_interval()) return std::unexpected(CronError::IntervalTooSmall);

        auto id = generate_id();
        CronTask task{
            .id = id,
            .name = std::move(name),
            .message = std::move(message),
            .expression = std::move(expr),
            .timezone = std::move(timezone),
            .created_at = std::chrono::system_clock::now(),
        };
        auto [it, _] = tasks_.emplace(id, std::move(task));
        return &it->second;
    }

    auto get(const std::string& id) -> std::expected<CronTask*, CronError> {
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return std::unexpected(CronError::TaskNotFound);
        return &it->second;
    }

    auto list() const -> std::vector<const CronTask*> {
        std::vector<const CronTask*> result;
        for (const auto& [_, task] : tasks_) {
            result.push_back(&task);
        }
        return result;
    }

    auto pause(const std::string& id) -> std::expected<void, CronError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        (*task)->state = CronTaskState::Paused;
        return {};
    }

    auto resume(const std::string& id) -> std::expected<void, CronError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        (*task)->state = CronTaskState::Active;
        return {};
    }

    auto remove(const std::string& id) -> std::expected<void, CronError> {
        if (!tasks_.contains(id)) return std::unexpected(CronError::TaskNotFound);
        tasks_.erase(id);
        return {};
    }

    auto trigger(const std::string& id) -> std::expected<void, CronError> {
        auto task = get(id);
        if (!task) return std::unexpected(task.error());
        if ((*task)->state == CronTaskState::Paused) return std::unexpected(CronError::TaskPaused);
        (*task)->last_run = std::chrono::system_clock::now();
        (*task)->run_count++;
        return {};
    }

private:
    std::unordered_map<std::string, CronTask> tasks_;
    size_t next_id_{0};

    auto generate_id() -> std::string {
        return std::format("cron_{}", next_id_++);
    }
};

// Global cron store singleton
inline CronStore& global_cron_store() {
    static CronStore store;
    return store;
}

// ScheduleCronTool - manages cron-based periodic tasks
class ScheduleCronTool {
public:
    static constexpr std::string_view name = "schedule_cron";
    static constexpr std::string_view description = "Create and manage cron-based periodic scheduled tasks";

    auto execute(CronRequest request) -> std::expected<std::string, CronError> {
        switch (request.action) {
            case CronAction::Create: {
                if (!request.name) return std::unexpected(CronError::NameEmpty);
                if (!request.message) return std::unexpected(CronError::MessageEmpty);
                if (!request.cron_expression) return std::unexpected(CronError::InvalidExpression);

                auto expr = CronExpression::parse(*request.cron_expression);
                if (!expr) return std::unexpected(expr.error());

                auto tz = request.timezone.value_or("UTC");
                auto task = global_cron_store().create(*request.name, *request.message,
                                                       std::move(*expr), std::move(tz));
                if (!task) return std::unexpected(task.error());
                return std::format("Created task '{}' (id: {})", (*task)->name, (*task)->id);
            }
            case CronAction::Pause: {
                if (!request.task_id) return std::unexpected(CronError::TaskNotFound);
                auto result = global_cron_store().pause(*request.task_id);
                if (!result) return std::unexpected(result.error());
                return std::format("Paused task '{}'", *request.task_id);
            }
            case CronAction::Resume: {
                if (!request.task_id) return std::unexpected(CronError::TaskNotFound);
                auto result = global_cron_store().resume(*request.task_id);
                if (!result) return std::unexpected(result.error());
                return std::format("Resumed task '{}'", *request.task_id);
            }
            case CronAction::Delete: {
                if (!request.task_id) return std::unexpected(CronError::TaskNotFound);
                auto result = global_cron_store().remove(*request.task_id);
                if (!result) return std::unexpected(result.error());
                return std::format("Deleted task '{}'", *request.task_id);
            }
            case CronAction::Trigger: {
                if (!request.task_id) return std::unexpected(CronError::TaskNotFound);
                auto result = global_cron_store().trigger(*request.task_id);
                if (!result) return std::unexpected(result.error());
                return std::format("Triggered task '{}'", *request.task_id);
            }
            case CronAction::Get: {
                if (!request.task_id) return std::unexpected(CronError::TaskNotFound);
                auto task = global_cron_store().get(*request.task_id);
                if (!task) return std::unexpected(task.error());
                return std::format("Task '{}': {} [{}]",
                    (*task)->name, (*task)->expression.raw,
                    (*task)->state == CronTaskState::Active ? "active" : "paused");
            }
            case CronAction::List: {
                auto tasks = global_cron_store().list();
                std::string output = std::format("Scheduled tasks ({}):\n", tasks.size());
                for (const auto* task : tasks) {
                    output += std::format("  - {} ({}): {} [{}]\n",
                        task->name, task->id, task->expression.raw,
                        task->state == CronTaskState::Active ? "active" : "paused");
                }
                return output;
            }
            default:
                return std::unexpected(CronError::InvalidAction);
        }
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["create", "update", "pause", "resume", "delete", "list", "get", "trigger"] }},
      "scheduled_task_id": {{ "type": "string", "description": "Task ID for manage operations" }},
      "name": {{ "type": "string", "description": "Task name (for create)" }},
      "message": {{ "type": "string", "description": "Task content to execute (for create)" }},
      "cron_expression": {{ "type": "string", "description": "5-field cron expression" }},
      "timezone": {{ "type": "string", "description": "IANA timezone (default: UTC)" }}
    }},
    "required": ["action"]
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
