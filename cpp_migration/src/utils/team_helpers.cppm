module;

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.team_helpers;

import cc.utils.json;

export namespace cc::utils {

namespace fs = std::filesystem;

// ─── Teammate Context ────────────────────────────────────────────────────────

/// Runtime context for in-process teammates (stored via thread_local)
struct TeammateContext {
    std::string agent_id;           // Full agent ID, e.g., "researcher@my-team"
    std::string agent_name;         // Display name, e.g., "researcher"
    std::string team_name;          // Team name this teammate belongs to
    std::optional<std::string> agent_type; // Agent definition type for teammate prompt/context
    std::optional<std::string> color;  // UI color assigned
    bool plan_mode_required = false;   // Whether teammate must enter plan mode first
    std::string parent_session_id;     // Leader's session ID
    bool is_in_process = true;         // Always true for in-process teammates
};

inline thread_local std::optional<TeammateContext> active_teammate_context;

/// Get the current in-process teammate context, if running as one.
/// Returns nullptr if not running within an in-process teammate context.
const TeammateContext* get_teammate_context();

/// Check if current execution is within an in-process teammate
bool is_in_process_teammate();

/// Run a function with teammate context set (scoped context)
template<typename F>
decltype(auto) run_with_teammate_context(const TeammateContext& ctx, F&& fn) {
    struct ContextGuard {
        std::optional<TeammateContext> previous;

        explicit ContextGuard(const TeammateContext& c)
            : previous(active_teammate_context) {
            active_teammate_context = c;
        }

        ~ContextGuard() {
            active_teammate_context = std::move(previous);
        }
    };
    ContextGuard guard(ctx);
    return std::forward<F>(fn)();
}

/// Create a TeammateContext from spawn configuration
TeammateContext create_teammate_context(
    std::string_view agent_id,
    std::string_view agent_name,
    std::string_view team_name,
    std::string_view parent_session_id,
    bool plan_mode_required,
    std::optional<std::string_view> color);

// ─── Dynamic Team Context (runtime joining) ──────────────────────────────────

/// Dynamic team context for tmux-based teammates joining at runtime
struct DynamicTeamContext {
    std::string agent_id;
    std::string agent_name;
    std::string team_name;
    std::optional<std::string> agent_type;
    std::optional<std::string> color;
    bool plan_mode_required = false;
    std::optional<std::string> parent_session_id;
};

inline thread_local std::optional<DynamicTeamContext> active_dynamic_team_context;

/// Set the dynamic team context (called when joining a team at runtime)
void set_dynamic_team_context(DynamicTeamContext context);

/// Clear the dynamic team context (called when leaving a team)
void clear_dynamic_team_context();

/// Get the current dynamic team context (for inspection/debugging)
const DynamicTeamContext* get_dynamic_team_context();

/// Returns the parent session ID (checks in-process ctx first, then dynamic)
std::optional<std::string> get_parent_session_id();

// ─── Teammate Identity Resolution ───────────────────────────────────────────

/// Get the team name for the current teammate.
/// Priority: AsyncLocalStorage > dynamicTeamContext > env vars
std::optional<std::string> get_team_name();

/// Get the agent name for the current teammate
std::optional<std::string> get_agent_name();

/// Get the agent ID for the current teammate
std::optional<std::string> get_agent_id();

/// Get the assigned color for the current teammate
std::optional<std::string> get_teammate_color();

/// Get the agent definition type for the current teammate
std::optional<std::string> get_agent_type();

/// Check whether this teammate must enter plan mode before implementation
bool is_plan_mode_required();

// ─── Team Discovery ─────────────────────────────────────────────────────────

/// Pane backend type used for teammate hosting
enum class PaneBackendType {
    tmux,
    iterm2,
    in_process
};

/// Summary of a team's state
struct TeamSummary {
    std::string name;
    int member_count = 0;
    int running_count = 0;
    int idle_count = 0;
};

/// Status of an individual teammate
struct TeammateStatus {
    std::string name;
    std::string agent_id;
    std::optional<std::string> agent_type;
    std::optional<std::string> model;
    std::optional<std::string> prompt;
    std::string status;   // "running", "idle", "unknown"
    std::optional<std::string> color;
    std::optional<std::string> idle_since;     // ISO timestamp
    std::string tmux_pane_id;
    std::string cwd;
    std::optional<std::string> worktree_path;
    bool is_hidden = false;
    std::optional<PaneBackendType> backend_type;
    std::optional<std::string> mode;   // Current permission mode
};

/// Get detailed teammate statuses for a team
std::vector<TeammateStatus> get_teammate_statuses(std::string_view team_name);

// ─── Team Memory Operations ─────────────────────────────────────────────────

/// Check if a file path is a team memory file
bool is_team_mem_file(std::string_view path);

/// Check if a search tool use targets team memory files
bool is_team_memory_search(std::string_view path);

/// Check if a Write or Edit tool use targets a team memory file
bool is_team_memory_write_or_edit(std::string_view tool_name, std::string_view file_path);

/// Memory operation counts for summary generation
struct TeamMemoryCounts {
    int team_memory_read_count = 0;
    int team_memory_search_count = 0;
    int team_memory_write_count = 0;
};

/// Append team memory summary parts to a parts vector
void append_team_memory_summary_parts(
    const TeamMemoryCounts& counts,
    bool is_active,
    std::vector<std::string>& parts);

// ─── Teammate Mailbox ────────────────────────────────────────────────────────

/// A message in a teammate's mailbox
struct TeammateMessage {
    std::string from;
    std::string text;
    std::string timestamp;   // ISO format
    bool read = false;
    std::optional<std::string> color;     // Sender's assigned color
    std::optional<std::string> summary;   // 5-10 word preview
};

/// Get the filesystem path to a teammate's inbox file
std::string get_inbox_path(std::string_view agent_name, std::optional<std::string_view> team_name);

/// Overload without team_name (uses current team)
std::string get_inbox_path(std::string_view agent_name);

/// Read all messages from a teammate's inbox
std::expected<std::vector<TeammateMessage>, std::string> read_inbox(
    std::string_view agent_name,
    std::optional<std::string_view> team_name);

/// Overload without team_name
std::expected<std::vector<TeammateMessage>, std::string> read_inbox(
    std::string_view agent_name);

/// Send a message to a teammate's inbox
std::expected<void, std::string> send_message(
    std::string_view to_agent_name,
    std::string_view text,
    std::optional<std::string_view> summary);

/// Overload without summary
std::expected<void, std::string> send_message(
    std::string_view to_agent_name,
    std::string_view text);

/// Mark all messages in an inbox as read
std::expected<void, std::string> mark_all_read(
    std::string_view agent_name,
    std::optional<std::string_view> team_name);

/// Overload without team_name
std::expected<void, std::string> mark_all_read(std::string_view agent_name);

/// Get the count of unread messages in an inbox
std::expected<size_t, std::string> unread_count(
    std::string_view agent_name,
    std::optional<std::string_view> team_name);

/// Overload without team_name
std::expected<size_t, std::string> unread_count(std::string_view agent_name);

namespace detail {

[[nodiscard]] inline std::optional<std::string> env_string(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (const char* value = std::getenv(name); value && *value) {
            return std::string(value);
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool env_truthy(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        const char* value = std::getenv(name);
        if (!value || !*value) continue;
        std::string normalized(value);
        std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return normalized != "0" && normalized != "false" &&
            normalized != "no" && normalized != "off";
    }
    return false;
}

[[nodiscard]] inline std::string mailbox_json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline std::string sanitize_path_component(std::string_view value, std::string_view fallback) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? std::string(fallback) : out;
}

[[nodiscard]] inline fs::path teams_dir() {
    if (const char* value = std::getenv("CC_REPL_TEAM_RUNTIME_DIR"); value && *value) {
        return fs::path{value};
    }
    if (const char* value = std::getenv("CLAUDE_CODE_TEAMS_DIR"); value && *value) {
        return fs::path{value};
    }
    return fs::current_path() / ".claude" / "teams";
}

[[nodiscard]] inline std::string current_team_name() {
    if (const char* value = std::getenv("CC_REPL_TEAM_NAME"); value && *value) {
        return value;
    }
    if (const char* value = std::getenv("CLAUDE_CODE_TEAM_NAME"); value && *value) {
        return value;
    }
    return "default";
}

[[nodiscard]] inline std::string current_agent_name() {
    if (const char* value = std::getenv("CC_REPL_AGENT_NAME"); value && *value) {
        return value;
    }
    if (const char* value = std::getenv("CLAUDE_CODE_AGENT_NAME"); value && *value) {
        return value;
    }
    return "team-lead";
}

[[nodiscard]] inline std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis);
}

[[nodiscard]] inline std::optional<std::string> json_optional_string(
    cc::utils::json::JsonVal object,
    std::string_view key
) {
    auto value = object.get(key);
    if (!value.valid() || !value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline bool write_messages(const fs::path& inbox_path, const std::vector<TeammateMessage>& messages) {
    std::error_code ec;
    fs::create_directories(inbox_path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(inbox_path, std::ios::trunc);
    if (!out) return false;
    out << '[';
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        if (i != 0) out << ',';
        out << R"({"from":")" << mailbox_json_escape(message.from)
            << R"(","text":")" << mailbox_json_escape(message.text)
            << R"(","timestamp":")" << mailbox_json_escape(message.timestamp)
            << R"(","read":)" << (message.read ? "true" : "false");
        if (message.color) {
            out << R"(,"color":")" << mailbox_json_escape(*message.color) << '"';
        }
        if (message.summary) {
            out << R"(,"summary":")" << mailbox_json_escape(*message.summary) << '"';
        }
        out << '}';
    }
    out << ']';
    return out.good();
}

} // namespace detail

inline const TeammateContext* get_teammate_context() {
    return active_teammate_context ? &*active_teammate_context : nullptr;
}

inline bool is_in_process_teammate() {
    return active_teammate_context.has_value();
}

inline TeammateContext create_teammate_context(
    std::string_view agent_id,
    std::string_view agent_name,
    std::string_view team_name,
    std::string_view parent_session_id,
    bool plan_mode_required,
    std::optional<std::string_view> color
) {
    return TeammateContext{
        .agent_id = std::string(agent_id),
        .agent_name = std::string(agent_name),
        .team_name = std::string(team_name),
        .agent_type = std::nullopt,
        .color = color ? std::optional<std::string>{std::string(*color)} : std::nullopt,
        .plan_mode_required = plan_mode_required,
        .parent_session_id = std::string(parent_session_id),
        .is_in_process = true,
    };
}

inline void set_dynamic_team_context(DynamicTeamContext context) {
    active_dynamic_team_context = std::move(context);
}

inline void clear_dynamic_team_context() {
    active_dynamic_team_context.reset();
}

inline const DynamicTeamContext* get_dynamic_team_context() {
    return active_dynamic_team_context ? &*active_dynamic_team_context : nullptr;
}

inline std::optional<std::string> get_parent_session_id() {
    if (active_teammate_context && !active_teammate_context->parent_session_id.empty()) {
        return active_teammate_context->parent_session_id;
    }
    if (active_dynamic_team_context && active_dynamic_team_context->parent_session_id &&
        !active_dynamic_team_context->parent_session_id->empty()) {
        return active_dynamic_team_context->parent_session_id;
    }
    return detail::env_string({"CC_REPL_PARENT_SESSION_ID", "CLAUDE_CODE_PARENT_SESSION_ID"});
}

inline std::optional<std::string> get_team_name() {
    if (active_teammate_context && !active_teammate_context->team_name.empty()) {
        return active_teammate_context->team_name;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->team_name.empty()) {
        return active_dynamic_team_context->team_name;
    }
    return detail::env_string({"CC_REPL_TEAM_NAME", "CLAUDE_CODE_TEAM_NAME"});
}

inline std::optional<std::string> get_agent_name() {
    if (active_teammate_context && !active_teammate_context->agent_name.empty()) {
        return active_teammate_context->agent_name;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->agent_name.empty()) {
        return active_dynamic_team_context->agent_name;
    }
    return detail::env_string({"CC_REPL_AGENT_NAME", "CLAUDE_CODE_AGENT_NAME"});
}

inline std::optional<std::string> get_agent_id() {
    if (active_teammate_context && !active_teammate_context->agent_id.empty()) {
        return active_teammate_context->agent_id;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->agent_id.empty()) {
        return active_dynamic_team_context->agent_id;
    }
    return detail::env_string({"CC_REPL_AGENT_ID", "CLAUDE_CODE_AGENT_ID"});
}

inline std::optional<std::string> get_teammate_color() {
    if (active_teammate_context && active_teammate_context->color && !active_teammate_context->color->empty()) {
        return active_teammate_context->color;
    }
    if (active_dynamic_team_context && active_dynamic_team_context->color && !active_dynamic_team_context->color->empty()) {
        return active_dynamic_team_context->color;
    }
    return detail::env_string({"CC_REPL_AGENT_COLOR", "CLAUDE_CODE_AGENT_COLOR"});
}

inline std::optional<std::string> get_agent_type() {
    if (active_teammate_context && active_teammate_context->agent_type && !active_teammate_context->agent_type->empty()) {
        return active_teammate_context->agent_type;
    }
    if (active_dynamic_team_context && active_dynamic_team_context->agent_type && !active_dynamic_team_context->agent_type->empty()) {
        return active_dynamic_team_context->agent_type;
    }
    return detail::env_string({"CC_REPL_AGENT_TYPE", "CLAUDE_CODE_AGENT_TYPE"});
}

inline bool is_plan_mode_required() {
    if (active_teammate_context) return active_teammate_context->plan_mode_required;
    if (active_dynamic_team_context) return active_dynamic_team_context->plan_mode_required;
    return detail::env_truthy({"CC_REPL_PLAN_MODE_REQUIRED", "CLAUDE_CODE_PLAN_MODE_REQUIRED"});
}

inline std::string get_inbox_path(std::string_view agent_name, std::optional<std::string_view> team_name) {
    const auto team = team_name && !team_name->empty()
        ? std::string(*team_name)
        : detail::current_team_name();
    return (detail::teams_dir() /
        detail::sanitize_path_component(team, "default") /
        "inboxes" /
        (detail::sanitize_path_component(agent_name, "agent") + ".json")).string();
}

inline std::string get_inbox_path(std::string_view agent_name) {
    return get_inbox_path(agent_name, std::nullopt);
}

inline std::expected<std::vector<TeammateMessage>, std::string> read_inbox(
    std::string_view agent_name,
    std::optional<std::string_view> team_name
) {
    const auto inbox_path = fs::path{get_inbox_path(agent_name, team_name)};
    std::error_code ec;
    if (!fs::exists(inbox_path, ec)) return std::vector<TeammateMessage>{};

    auto parsed = cc::utils::json::parse_file(inbox_path);
    if (!parsed) return std::unexpected(parsed.error().format());
    auto root = parsed->root();
    if (!root.is_arr()) return std::unexpected("teammate inbox must be a JSON array");

    std::vector<TeammateMessage> messages;
    messages.reserve(root.size());
    root.iter([&](cc::utils::json::JsonVal item) {
        if (!item.is_obj()) return;
        auto from = item.get("from");
        auto text = item.get("text");
        auto timestamp = item.get("timestamp");
        if (!from.is_str() || !text.is_str()) return;
        TeammateMessage message{
            .from = std::string(from.as_str()),
            .text = std::string(text.as_str()),
            .timestamp = timestamp.is_str() ? std::string(timestamp.as_str()) : std::string{},
            .read = item.get("read").is_bool() ? item.get("read").as_bool() : false,
            .color = detail::json_optional_string(item, "color"),
            .summary = detail::json_optional_string(item, "summary"),
        };
        messages.push_back(std::move(message));
    });
    return messages;
}

inline std::expected<std::vector<TeammateMessage>, std::string> read_inbox(std::string_view agent_name) {
    return read_inbox(agent_name, std::nullopt);
}

inline std::expected<void, std::string> write_to_mailbox(
    std::string_view recipient_name,
    TeammateMessage message,
    std::optional<std::string_view> team_name
) {
    auto existing = read_inbox(recipient_name, team_name);
    if (!existing) return std::unexpected(existing.error());
    if (message.timestamp.empty()) message.timestamp = detail::timestamp_now();
    message.read = false;
    existing->push_back(std::move(message));
    const auto inbox_path = fs::path{get_inbox_path(recipient_name, team_name)};
    if (!detail::write_messages(inbox_path, *existing)) {
        return std::unexpected("failed to write teammate inbox");
    }
    return {};
}

inline std::expected<void, std::string> send_message(
    std::string_view to_agent_name,
    std::string_view text,
    std::optional<std::string_view> summary
) {
    return write_to_mailbox(
        to_agent_name,
        TeammateMessage{
            .from = detail::current_agent_name(),
            .text = std::string(text),
            .timestamp = detail::timestamp_now(),
            .read = false,
            .color = std::nullopt,
            .summary = summary ? std::optional<std::string>{std::string(*summary)} : std::nullopt,
        },
        std::nullopt);
}

inline std::expected<void, std::string> send_message(std::string_view to_agent_name, std::string_view text) {
    return send_message(to_agent_name, text, std::nullopt);
}

inline std::expected<void, std::string> mark_all_read(
    std::string_view agent_name,
    std::optional<std::string_view> team_name
) {
    auto messages = read_inbox(agent_name, team_name);
    if (!messages) return std::unexpected(messages.error());
    for (auto& message : *messages) message.read = true;
    const auto inbox_path = fs::path{get_inbox_path(agent_name, team_name)};
    if (!detail::write_messages(inbox_path, *messages)) {
        return std::unexpected("failed to update teammate inbox");
    }
    return {};
}

inline std::expected<void, std::string> mark_all_read(std::string_view agent_name) {
    return mark_all_read(agent_name, std::nullopt);
}

inline std::expected<size_t, std::string> unread_count(
    std::string_view agent_name,
    std::optional<std::string_view> team_name
) {
    auto messages = read_inbox(agent_name, team_name);
    if (!messages) return std::unexpected(messages.error());
    return static_cast<size_t>(std::ranges::count_if(*messages, [](const auto& message) {
        return !message.read;
    }));
}

inline std::expected<size_t, std::string> unread_count(std::string_view agent_name) {
    return unread_count(agent_name, std::nullopt);
}

} // namespace cc::utils
