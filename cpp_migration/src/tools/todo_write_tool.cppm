// TodoWriteTool - Manages structured todo lists for task tracking
module;
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.tools.todo_write;

import cc.utils.error;
import cc.utils.json;
import cc.tools.tool;


export namespace cc::tools {

// Todo item status
enum class TodoStatus {
    Pending,
    InProgress,
    Completed,
};

constexpr auto todo_status_name(TodoStatus s) -> std::string_view {
    switch (s) {
        case TodoStatus::Pending:    return "pending";
        case TodoStatus::InProgress: return "in_progress";
        case TodoStatus::Completed:  return "completed";
        default:                     return "unknown";
    }
}

// Priority levels
enum class Priority {
    High,
    Medium,
    Low,
};

constexpr auto priority_name(Priority p) -> std::string_view {
    switch (p) {
        case Priority::High:   return "high";
        case Priority::Medium: return "medium";
        case Priority::Low:    return "low";
        default:               return "unknown";
    }
}

// A single todo item
struct TodoItem {
    std::string id;
    std::string content;
    TodoStatus status{TodoStatus::Pending};
    Priority priority{Priority::Medium};
    std::optional<std::string> summary;  // Summary of work done (set when completed)
    std::chrono::system_clock::time_point created_at;
};

// Error types for todo operations
enum class TodoError {
    IdEmpty,
    ContentEmpty,
    TooManyItems,
    ItemNotFound,
    DuplicateId,
    MultipleInProgress,
    InvalidTransition,
};

constexpr auto format_error(TodoError err) -> std::string_view {
    switch (err) {
        case TodoError::IdEmpty:             return "Todo item ID is empty";
        case TodoError::ContentEmpty:        return "Todo item content is empty";
        case TodoError::TooManyItems:        return "Maximum number of todo items (10) reached";
        case TodoError::ItemNotFound:        return "Todo item not found";
        case TodoError::DuplicateId:         return "Todo item with this ID already exists";
        case TodoError::MultipleInProgress:  return "Only one item can be in_progress at a time";
        case TodoError::InvalidTransition:   return "Invalid status transition";
        default:                             return "Unknown todo error";
    }
}

// Request to write/update todo list
struct TodoWriteRequest {
    std::vector<TodoItem> items;
    bool merge{false};  // true: merge by id; false: replace all
};

// Result of todo write operation
struct TodoWriteResult {
    size_t total_items{0};
    size_t items_added{0};
    size_t items_updated{0};
    size_t items_removed{0};
};

// TodoWriteTool - manages todo list state
class TodoWriteTool {
public:
    static constexpr std::string_view name = "todo_write";
    static constexpr std::string_view description = "Create and manage a structured task list";
    static constexpr size_t kMaxItems = 10;

    TodoWriteTool() = default;

    // Validate a single todo item
    auto validate_item(const TodoItem& item) const -> std::expected<void, TodoError> {
        if (item.id.empty()) {
            return std::unexpected(TodoError::IdEmpty);
        }
        if (item.content.empty()) {
            return std::unexpected(TodoError::ContentEmpty);
        }
        return {};
    }

    // Validate entire request including constraint checks
    auto validate(const TodoWriteRequest& request) const -> std::expected<void, TodoError> {
        // Check item limit
        if (request.items.size() > kMaxItems) {
            return std::unexpected(TodoError::TooManyItems);
        }

        // Validate individual items
        for (const auto& item : request.items) {
            if (auto result = validate_item(item); !result) {
                return result;
            }
        }

        // Check for duplicate IDs within request
        std::unordered_set<std::string_view> seen_ids;
        for (const auto& item : request.items) {
            if (seen_ids.contains(item.id)) {
                return std::unexpected(TodoError::DuplicateId);
            }
            seen_ids.insert(item.id);
        }

        // Check only one in_progress constraint
        size_t in_progress_count = static_cast<size_t>(std::count_if(request.items.begin(), request.items.end(), [](const auto& item) {
            return item.status == TodoStatus::InProgress;
        }));
        if (in_progress_count > 1) {
            return std::unexpected(TodoError::MultipleInProgress);
        }

        return {};
    }

    // Main entry: write or merge todo items
    auto execute(TodoWriteRequest request) -> std::expected<TodoWriteResult, TodoError> {
        if (auto result = validate(request); !result) {
            return std::unexpected(result.error());
        }

        TodoWriteResult result;

        if (!request.merge) {
            // Replace mode: swap entire list
            result.items_removed = items_.size();
            result.items_added = request.items.size();
            items_.clear();

            for (auto& item : request.items) {
                if (item.created_at == std::chrono::system_clock::time_point{}) {
                    item.created_at = std::chrono::system_clock::now();
                }
                items_.push_back(std::move(item));
            }
        } else {
            // Merge mode: update existing items by id, add new ones
            for (auto& item : request.items) {
                auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& existing) {
                    return existing.id == item.id;
                });

                if (it != items_.end()) {
                    // Update existing item (preserve created_at)
                    auto created = it->created_at;
                    *it = std::move(item);
                    it->created_at = created;
                    result.items_updated++;
                } else {
                    // Add new item
                    if (items_.size() >= kMaxItems) {
                        return std::unexpected(TodoError::TooManyItems);
                    }
                    if (item.created_at == std::chrono::system_clock::time_point{}) {
                        item.created_at = std::chrono::system_clock::now();
                    }
                    items_.push_back(std::move(item));
                    result.items_added++;
                }
            }
        }

        // Final constraint validation on the resulting list
        size_t in_progress = static_cast<size_t>(std::count_if(items_.begin(), items_.end(), [](const auto& item) {
            return item.status == TodoStatus::InProgress;
        }));
        if (in_progress > 1) {
            return std::unexpected(TodoError::MultipleInProgress);
        }

        result.total_items = items_.size();
        return result;
    }

    // Get all current items
    [[nodiscard]] auto get_items() const -> const std::vector<TodoItem>& {
        return items_;
    }

    // Get items sorted for display (in_progress > pending > completed, then by priority)
    [[nodiscard]] auto get_sorted_items() const -> std::vector<TodoItem> {
        auto sorted = items_;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            // Status order: in_progress(0) > pending(1) > completed(2)
            auto status_rank = [](TodoStatus s) -> int {
                switch (s) {
                    case TodoStatus::InProgress: return 0;
                    case TodoStatus::Pending:    return 1;
                    case TodoStatus::Completed:  return 2;
                    default: return 3;
                }
            };
            // Priority order: high(0) > medium(1) > low(2)
            auto priority_rank = [](Priority p) -> int {
                switch (p) {
                    case Priority::High:   return 0;
                    case Priority::Medium: return 1;
                    case Priority::Low:    return 2;
                    default: return 3;
                }
            };

            if (status_rank(a.status) != status_rank(b.status))
                return status_rank(a.status) < status_rank(b.status);
            if (priority_rank(a.priority) != priority_rank(b.priority))
                return priority_rank(a.priority) < priority_rank(b.priority);
            return a.created_at < b.created_at;
        });
        return sorted;
    }

    // Generate JSON schema for LLM tool invocation
    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "todos": {{
        "type": "array",
        "items": {{
          "type": "object",
            "properties": {{
            "id": {{ "type": "string", "description": "Optional stable id; generated when omitted" }},
            "content": {{ "type": "string" }},
            "status": {{ "type": "string", "enum": ["pending", "in_progress", "completed"] }},
            "activeForm": {{ "type": "string", "description": "Present-tense form of the task, accepted for TypeScript parity" }},
            "priority": {{ "type": "string", "enum": ["high", "medium", "low"], "default": "medium" }},
            "summary": {{ "type": "string" }}
          }},
          "required": ["content", "status"]
        }},
        "maxItems": 10,
        "minItems": 0
      }},
      "merge": {{ "type": "boolean", "description": "Merge with existing list by id" }}
    }},
    "required": ["todos"]
  }}
}})", name, description);
    }

private:
    std::vector<TodoItem> items_;
};

namespace detail {

using JsonVal = cc::utils::json::JsonVal;

[[nodiscard]] std::optional<std::string> json_string(JsonVal obj, std::string_view key) {
    auto value = obj.get(key);
    if (!value.valid() || !value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] bool json_bool(JsonVal obj, std::string_view key, bool fallback) {
    auto value = obj.get(key);
    return value.valid() && value.is_bool() ? value.as_bool() : fallback;
}

[[nodiscard]] std::expected<TodoStatus, std::string> parse_status(JsonVal item) {
    auto status = json_string(item, "status");
    if (!status) return std::unexpected("Todo item status is required");
    if (*status == "pending") return TodoStatus::Pending;
    if (*status == "in_progress") return TodoStatus::InProgress;
    if (*status == "completed") return TodoStatus::Completed;
    return std::unexpected("Invalid todo status: " + *status);
}

[[nodiscard]] std::expected<Priority, std::string> parse_priority(JsonVal item) {
    auto priority = json_string(item, "priority");
    if (!priority || priority->empty()) return Priority::Medium;
    if (*priority == "high") return Priority::High;
    if (*priority == "medium") return Priority::Medium;
    if (*priority == "low") return Priority::Low;
    return std::unexpected("Invalid todo priority: " + *priority);
}

[[nodiscard]] std::expected<TodoItem, std::string> parse_item(JsonVal item, std::size_t index) {
    if (!item.valid() || !item.is_obj()) {
        return std::unexpected(std::format("Todo item {} must be an object", index + 1));
    }

    auto content = json_string(item, "content");
    if (!content || content->empty()) {
        return std::unexpected(std::format("Todo item {} content is required", index + 1));
    }

    auto status = parse_status(item);
    if (!status) return std::unexpected(status.error());
    auto priority = parse_priority(item);
    if (!priority) return std::unexpected(priority.error());

    TodoItem parsed{
        .id = json_string(item, "id").value_or(std::format("todo-{}", index + 1)),
        .content = std::move(*content),
        .status = *status,
        .priority = *priority,
        .summary = json_string(item, "summary"),
        .created_at = {},
    };
    return parsed;
}

[[nodiscard]] std::expected<TodoWriteRequest, std::string> parse_request(std::string_view raw_json) {
    auto doc = cc::utils::json::parse(raw_json);
    if (!doc) return std::unexpected(doc.error().format());

    auto root = doc->root();
    if (!root.valid() || !root.is_obj()) {
        return std::unexpected("todo_write input must be a JSON object");
    }

    auto todos = root.get("todos");
    if (!todos.valid()) todos = root.get("items");
    if (!todos.valid() || !todos.is_arr()) {
        return std::unexpected("todo_write requires a todos array");
    }

    TodoWriteRequest request;
    request.merge = json_bool(root, "merge", false);

    std::size_t index = 0;
    std::optional<std::string> error;
    todos.iter([&](JsonVal item) {
        if (error) return;
        auto parsed = parse_item(item, index);
        if (!parsed) {
            error = parsed.error();
            return;
        }
        request.items.push_back(std::move(*parsed));
        ++index;
    });

    if (error) return std::unexpected(*error);

    if (!request.merge && !request.items.empty()) {
        const bool all_done = std::ranges::all_of(request.items, [](const TodoItem& item) {
            return item.status == TodoStatus::Completed;
        });
        if (all_done) request.items.clear();
    }

    return request;
}

} // namespace detail

/// Factory: create TodoWriteTool wrapped as ITool for registry integration
[[nodiscard]] auto make_todo_write_tool() -> std::unique_ptr<cc::core::ITool> {
    struct Adapter final : cc::core::ITool {
        TodoWriteTool tool_;
        cc::core::ToolDefinition def_{
            .name = std::string(TodoWriteTool::name),
            .description = std::string(TodoWriteTool::description),
            .input_schema = cc::core::InputSchema{
                .properties = {
                    cc::core::SchemaProperty{
                        .name = "todos",
                        .type = "array",
                        .description = "Array of todo items with id, content, status, priority",
                        .required = true
                    },
                    cc::core::SchemaProperty{
                        .name = "merge",
                        .type = "boolean",
                        .description = "Merge with existing list by id (true) or replace all (false)",
                        .required = false,
                        .default_value = "false"
                    }
                }
            },
            .permission = cc::core::ToolPermission::ReadOnly,
            .category = "task_management"
        };

        const cc::core::ToolDefinition& definition() const override { return def_; }

        std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
            auto request = detail::parse_request(input.json());
            if (!request) {
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::InvalidInput,
                    request.error()));
            }

            auto result = tool_.execute(std::move(*request));
            if (result) {
                auto msg = std::format("Todo list updated: {} total, {} added, {} updated, {} removed",
                    result->total_items, result->items_added, result->items_updated, result->items_removed);
                return cc::core::ToolResult::success(std::move(msg));
            }
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::ToolExecutionFailed,
                std::string(format_error(result.error()))));
        }

        bool check_permission(const cc::core::ToolInput&) const override {
            return true;
        }
    };
    return std::make_unique<Adapter>();
}

} // namespace cc::tools
