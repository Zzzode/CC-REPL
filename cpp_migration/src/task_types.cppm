// Canonical task data model and validation helpers.
// NOTE on deduplication: several hook-local Task types exist with narrower
// shapes (hooks/task_hooks.cppm uses a 3-state TaskStatus / 3-level priority,
// hooks/tasks_v2.cppm uses a v2-specific pending/completed/cancelled enum,
// tasks/types.cppm holds task-engine state variants, and commands/tasks_cmd.cppm
// serialises user-facing task-records).  Those files will migrate toward this
// module as their canonical source of truth.
module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <yyjson.h>

export module cc.task_types;

import cc.utils.json;

export namespace cc::tasks {

// ---------------------------------------------------------------------------
// Enums + core types
// ---------------------------------------------------------------------------

enum class TaskStatus : uint8_t {
    Open,
    InProgress,
    Done,
    Blocked,
    Cancelled,
    Archived,
};

enum class TaskPriority : uint8_t {
    Low,
    Normal,
    High,
    Critical,
};

enum class TaskSortKey {
    CreatedAtAsc,
    CreatedAtDesc,
    PriorityDescStatusAsc,
    TitleAsc,
};

using TaskId = std::string;

struct Task {
    TaskId id;
    std::string title;
    std::string description;
    TaskStatus status = TaskStatus::Open;
    TaskPriority priority = TaskPriority::Normal;
    std::string assignee_agent_id;
    std::string parent_id;
    std::vector<TaskId> subtask_ids;
    int64_t created_at_ms = 0;
    int64_t updated_at_ms = 0;
    int64_t due_at_ms = 0;
    std::string external_ref;
    std::unordered_map<std::string, std::string> metadata;
};

struct TaskCreateParams {
    std::string title;
    std::string description;
    TaskPriority priority = TaskPriority::Normal;
    std::string assignee;
    std::string parent;
    std::string external_ref;
    int64_t due_at_ms = 0;
    std::unordered_map<std::string, std::string> metadata;
};

struct TaskUpdateParams {
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> assignee;
    std::optional<TaskStatus> status;
    std::optional<TaskPriority> priority;
    std::vector<std::string> add_subtask;
    std::vector<std::string> remove_subtask;
    std::unordered_map<std::string, std::string> metadata_set;
    std::vector<std::string> metadata_unset;
};

// ---------------------------------------------------------------------------
// Ser/de helpers
// ---------------------------------------------------------------------------

namespace detail {

inline const char* status_str(TaskStatus s) {
    switch (s) {
        case TaskStatus::Open: return "open";
        case TaskStatus::InProgress: return "in_progress";
        case TaskStatus::Done: return "done";
        case TaskStatus::Blocked: return "blocked";
        case TaskStatus::Cancelled: return "cancelled";
        case TaskStatus::Archived: return "archived";
    }
    return "open";
}

inline TaskStatus status_from(std::string_view s) {
    if (s == "open") return TaskStatus::Open;
    if (s == "in_progress") return TaskStatus::InProgress;
    if (s == "done") return TaskStatus::Done;
    if (s == "blocked") return TaskStatus::Blocked;
    if (s == "cancelled") return TaskStatus::Cancelled;
    if (s == "archived") return TaskStatus::Archived;
    return TaskStatus::Open;
}

inline const char* priority_str(TaskPriority p) {
    switch (p) {
        case TaskPriority::Low: return "low";
        case TaskPriority::Normal: return "normal";
        case TaskPriority::High: return "high";
        case TaskPriority::Critical: return "critical";
    }
    return "normal";
}

inline TaskPriority priority_from(std::string_view s) {
    if (s == "low") return TaskPriority::Low;
    if (s == "normal") return TaskPriority::Normal;
    if (s == "high") return TaskPriority::High;
    if (s == "critical") return TaskPriority::Critical;
    return TaskPriority::Normal;
}

} // namespace detail

inline std::string task_to_json(const Task& t) {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();

    root.add("id", doc.string(t.id));
    root.add("title", doc.string(t.title));
    root.add("description", doc.string(t.description));
    root.add("status", doc.string(detail::status_str(t.status)));
    root.add("priority", doc.string(detail::priority_str(t.priority)));
    if (!t.assignee_agent_id.empty()) root.add("assignee_agent_id", doc.string(t.assignee_agent_id));
    if (!t.parent_id.empty()) root.add("parent_id", doc.string(t.parent_id));

    // subtasks
    {
        auto arr = doc.array();
        for (const auto& s : t.subtask_ids) arr.append(doc.string(s));
        root.add("subtask_ids", arr);
    }

    root.add("created_at_ms", doc.number(t.created_at_ms));
    root.add("updated_at_ms", doc.number(t.updated_at_ms));
    if (t.due_at_ms != 0) root.add("due_at_ms", doc.number(t.due_at_ms));
    if (!t.external_ref.empty()) root.add("external_ref", doc.string(t.external_ref));

    // metadata
    {
        auto obj = doc.object();
        for (const auto& [k, v] : t.metadata) obj.add(k, doc.string(v));
        root.add("metadata", obj);
    }

    doc.set_root(root);
    return doc.to_string();
}

inline std::expected<Task, std::string> task_from_json(std::string_view s) {
    auto parsed = cc::utils::json::parse(s);
    if (!parsed) return std::unexpected("invalid task JSON");
    auto v = parsed->root();
    if (!v.is_obj()) return std::unexpected("task JSON root is not object");

    Task t;
    t.id = std::string(v.get_string("id"));
    t.title = std::string(v.get_string("title"));
    t.description = std::string(v.get_string("description"));
    t.status = detail::status_from(v.get_string("status"));
    t.priority = detail::priority_from(v.get_string("priority"));
    t.assignee_agent_id = std::string(v.get_string("assignee_agent_id"));
    t.parent_id = std::string(v.get_string("parent_id"));

    auto subs = v.get("subtask_ids");
    if (subs.is_arr()) {
        subs.iter([&](cc::utils::json::JsonVal e) {
            if (e.is_str()) t.subtask_ids.emplace_back(e.as_str());
        });
    }
    t.created_at_ms = v.get_int("created_at_ms");
    t.updated_at_ms = v.get_int("updated_at_ms");
    t.due_at_ms = v.get_int("due_at_ms");
    t.external_ref = std::string(v.get_string("external_ref"));

    auto meta = v.get("metadata");
    if (meta.is_obj()) {
        meta.iter_obj([&](cc::utils::json::JsonVal k, cc::utils::json::JsonVal val) {
            if (k.is_str() && val.is_str()) {
                t.metadata.emplace(std::string(k.as_str()), std::string(val.as_str()));
            }
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Graph helper
// ---------------------------------------------------------------------------

inline bool is_descendant_of(const std::unordered_map<TaskId, Task>& index,
                             const TaskId& parent,
                             const TaskId& child) {
    if (parent == child) return false;
    std::unordered_set<std::string> seen;
    std::string cur = child;
    while (!cur.empty()) {
        auto it = index.find(cur);
        if (it == index.end()) return false;
        if (!seen.insert(cur).second) return false; // cycle guard
        if (it->second.parent_id == parent) return true;
        cur = it->second.parent_id;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

inline std::optional<std::string> validate_task_create(const TaskCreateParams& p) {
    // Trim title for emptiness check.
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t start = 0;
    while (start < p.title.size() && is_ws(static_cast<unsigned char>(p.title[start]))) ++start;
    size_t end = p.title.size();
    while (end > start && is_ws(static_cast<unsigned char>(p.title[end - 1]))) --end;
    if (start == end) return "task title must not be empty";
    if ((end - start) > 1024) return "task title too long (max 1024 chars)";
    if (p.description.size() > 1024 * 1024) return "task description too large";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Id generation
// ---------------------------------------------------------------------------

inline std::string generate_task_id() {
    constexpr char hex[] = "0123456789abcdef";
    constexpr size_t kLen = 24;

    // Prefer arc4random_buf where available.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    unsigned char buf[kLen / 2];
    arc4random_buf(buf, sizeof(buf));
#else
    // Fallback: mt19937 seeded from std::random_device.
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    unsigned char buf[kLen / 2];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<unsigned char>(dist(rng));
#endif

    std::string out;
    out.reserve(kLen);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        out.push_back(hex[(buf[i] >> 4) & 0x0F]);
        out.push_back(hex[buf[i] & 0x0F]);
    }
    return out;
}

} // namespace cc::tasks
