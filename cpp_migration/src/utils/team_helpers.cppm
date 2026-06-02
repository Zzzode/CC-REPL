module;

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.team_helpers;

export namespace cc::utils {

// ─── Teammate Context ────────────────────────────────────────────────────────

/// Runtime context for in-process teammates (stored via thread_local)
struct TeammateContext {
    std::string agent_id;           // Full agent ID, e.g., "researcher@my-team"
    std::string agent_name;         // Display name, e.g., "researcher"
    std::string team_name;          // Team name this teammate belongs to
    std::optional<std::string> color;  // UI color assigned
    bool plan_mode_required = false;   // Whether teammate must enter plan mode first
    std::string parent_session_id;     // Leader's session ID
    bool is_in_process = true;         // Always true for in-process teammates
};

/// Get the current in-process teammate context, if running as one.
/// Returns nullptr if not running within an in-process teammate context.
const TeammateContext* get_teammate_context();

/// Check if current execution is within an in-process teammate
bool is_in_process_teammate();

/// Run a function with teammate context set (scoped context)
template<typename F>
decltype(auto) run_with_teammate_context(const TeammateContext& ctx, F&& fn) {
    struct ContextGuard {
        const TeammateContext& ctx;
        ContextGuard(const TeammateContext& c);
        ~ContextGuard();
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
    std::optional<std::string> color;
    bool plan_mode_required = false;
    std::optional<std::string> parent_session_id;
};

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

} // namespace cc::utils
