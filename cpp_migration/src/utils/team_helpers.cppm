module;

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <utility>
#include <vector>
#include <fstream>
#include <functional>
#include <initializer_list>

export module cc.utils.team_helpers;

import cc.utils.json;

export namespace cc::utils {

namespace fs = std::filesystem;

/// Process-wide lock serializing ALL read-modify-write operations on teammate
/// inbox files (append write_to_mailbox, mark_all_read, and the
/// permission-sync response remover in swarm_helpers). Without it, a worker
/// blocking on a permission response races the inbox poller's periodic
/// mark-all-read and can resurrect/lose the leader's approval.
/// Cross-process leader↔worker serialization is still file-based (separate
/// processes); this guards the multiple in-process threads that touch one
/// inbox. TS uses proper-lockfile around each mutation.
inline std::mutex& teammate_inbox_mutex() {
    static std::mutex m;
    return m;
}

/// Cross-process advisory lock (flock) for one inbox file. Leader and
/// pane-teammates are SEPARATE processes, so the in-process mutex above does
/// not serialize their concurrent read-modify-write cycles. Each RMW takes an
/// exclusive lock on a sibling "<inbox>.lock" file; flock is released on
/// close (RAII) and automatically if the holder dies. TS uses proper-lockfile
/// around every mailbox mutation.
class ScopedInboxLock {
public:
    explicit ScopedInboxLock(const fs::path& inbox_path) {
#if !defined(_WIN32)
        std::error_code ec;
        fs::create_directories(inbox_path.parent_path(), ec);
        lock_path_ = fs::path{inbox_path} += ".lock";
        fd_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd_ < 0) return;
        // EINTR-safe bounded exclusive lock acquisition.
        constexpr int kMaxAttempts = 200;  // ~10s at 50ms
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (::flock(fd_, LOCK_EX) == 0) {
                locked_ = true;
                return;
            }
            if (errno != EINTR) {
                ::close(fd_);
                fd_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::close(fd_);
        fd_ = -1;
#else
        (void)inbox_path;
#endif
    }

    ScopedInboxLock(const ScopedInboxLock&) = delete;
    ScopedInboxLock& operator=(const ScopedInboxLock&) = delete;

    ~ScopedInboxLock() {
#if !defined(_WIN32)
        if (fd_ >= 0) {
            if (locked_) ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
#endif
    }

    /// True when the exclusive lock is held (and a no-op stand-in on
    /// platforms without flock, where only the in-process mutex applies).
    [[nodiscard]] bool locked() const noexcept {
#if !defined(_WIN32)
        return locked_;
#else
        return true;
#endif
    }

private:
#if !defined(_WIN32)
    int fd_ = -1;
    bool locked_ = false;
    fs::path lock_path_;
#endif
};

/// Generic cross-process advisory lock; ScopedInboxLock is not inbox-specific
/// despite its name — it flocks any path's sibling ".lock" file.
using ScopedFileLock = ScopedInboxLock;

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

// ─── Canonical Team Config File (config.json) ───────────────────────────────
//
// TS-aligned model for <teams_dir>/<sanitized(team)>/config.json, mirroring
// src/utils/swarm/teamHelpers.ts and reconnection.ts. This is the canonical
// writer; team_create keeps its own narrower flat <name>.json shape, which
// coexists on disk under a different file name.

/// One member row in a team config.json (TS TeamFile['members'][number]).
struct TeamMemberRecord {
    std::string agent_id;             // JSON "agentId", e.g. "name@team"
    std::string name;                 // JSON "name"
    std::optional<std::string> color;          // Palette token, e.g. "blue"
    std::optional<std::string> backend;        // BackendType: "tmux"|"iterm2"|"in-process"
    std::string tmux_pane_id;                  // JSON "tmuxPaneId"
    std::optional<std::string> mode;           // PermissionMode string
    std::int64_t joined_at = 0;                // JSON "joinedAt"
    std::string cwd;
    bool plan_mode_required = false;           // JSON "planModeRequired"
    bool is_active = true;                     // JSON "isActive"
    std::optional<std::string> agent_type;     // JSON "agentType"
    std::optional<std::string> model;
    std::optional<std::string> prompt;
    std::optional<std::string> worktree_path;  // JSON "worktreePath"
    std::optional<std::string> session_id;     // JSON "sessionId"
    std::vector<std::string> subscriptions;
};

/// Team config.json root object (TS TeamFile in teamHelpers.ts:64-90).
struct TeamFileRecord {
    std::string name;
    std::optional<std::string> description;
    std::int64_t created_at = 0;               // JSON "createdAt"
    std::string lead_agent_id;                 // JSON "leadAgentId"
    std::optional<std::string> lead_session_id;  // JSON "leadSessionId"
    std::vector<std::string> hidden_pane_ids;  // JSON "hiddenPaneIds"
    std::vector<TeamMemberRecord> members;
};

/// Resolved startup identity for a leader or teammate (reconnection.ts:23-66).
struct InitialTeamContext {
    std::string team_name;
    std::string team_file_path;
    std::string lead_agent_id;
    std::optional<std::string> self_agent_id;  // nullopt => leader
    std::string self_agent_name;
    bool is_leader = false;
};

/// Directory backing a team: <teams_dir>/<sanitized(team)> (TS getTeamDir).
[[nodiscard]] std::string team_dir(std::string_view team_name);

/// Path to a team's canonical config.json (TS getTeamFilePath).
[[nodiscard]] std::string team_file_path(std::string_view team_name);

/// Read and parse a team config.json. Returns nullopt when the file is
/// missing or unreadable (TS readTeamFile maps ENOENT/parse failure to null).
[[nodiscard]] std::optional<TeamFileRecord> read_team_file(std::string_view team_name);

/// Serialize a team record to config.json with 2-space indentation, creating
/// the team directory as needed. Returns false on I/O failure.
bool write_team_file(std::string_view team_name, const TeamFileRecord& record);

/// Find a member row by display name (linear search, TS Array.prototype.find).
[[nodiscard]] std::optional<TeamMemberRecord> find_team_member(
    const TeamFileRecord& file, std::string_view name);

/// Resolve initial leader/teammate identity for an explicit identity triple.
[[nodiscard]] std::optional<InitialTeamContext> compute_initial_team_context(
    std::string_view team_name,
    std::string_view agent_name,
    std::optional<std::string_view> agent_id);

/// Resolve initial identity from the current environment / dynamic context.
/// This is the C++ equivalent of computeInitialTeamContext() in
/// src/utils/swarm/reconnection.ts.
[[nodiscard]] std::optional<InitialTeamContext> compute_initial_team_context_from_env();

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
    static const char* hex = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            case '\b': out += R"(\b)"; break;
            case '\f': out += R"(\f)"; break;
            default:
                // RFC 8259: all other C0 control bytes must be \u-escaped;
                // one bad control char must never corrupt the whole inbox.
                if (ch < 0x20) {
                    out += R"(\u00)";
                    out.push_back(hex[(ch >> 4) & 0x0F]);
                    out.push_back(hex[ch & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
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
    if (const char* value = std::getenv("LOOM_TEAM_RUNTIME_DIR"); value && *value) {
        return fs::path{value};
    }
    if (const char* value = std::getenv("LOOM_TEAMS_DIR"); value && *value) {
        return fs::path{value};
    }
    return fs::current_path() / ".loom" / "teams";
}

[[nodiscard]] inline std::string current_team_name() {
    if (const char* value = std::getenv("LOOM_TEAM_NAME"); value && *value) {
        return value;
    }
    if (const char* value = std::getenv("CLAUDE_CODE_TEAM_NAME"); value && *value) {
        return value;
    }
    return "default";
}

[[nodiscard]] inline std::string current_agent_name() {
    if (const char* value = std::getenv("LOOM_AGENT_NAME"); value && *value) {
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
    return detail::env_string({"LOOM_PARENT_SESSION_ID", "LOOM_PARENT_SESSION_ID"});
}

inline std::optional<std::string> get_team_name() {
    if (active_teammate_context && !active_teammate_context->team_name.empty()) {
        return active_teammate_context->team_name;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->team_name.empty()) {
        return active_dynamic_team_context->team_name;
    }
    return detail::env_string({"LOOM_TEAM_NAME", "LOOM_TEAM_NAME"});
}

inline std::optional<std::string> get_agent_name() {
    if (active_teammate_context && !active_teammate_context->agent_name.empty()) {
        return active_teammate_context->agent_name;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->agent_name.empty()) {
        return active_dynamic_team_context->agent_name;
    }
    return detail::env_string({"LOOM_AGENT_NAME", "LOOM_AGENT_NAME"});
}

inline std::optional<std::string> get_agent_id() {
    if (active_teammate_context && !active_teammate_context->agent_id.empty()) {
        return active_teammate_context->agent_id;
    }
    if (active_dynamic_team_context && !active_dynamic_team_context->agent_id.empty()) {
        return active_dynamic_team_context->agent_id;
    }
    return detail::env_string({"LOOM_AGENT_ID", "LOOM_AGENT_ID"});
}

inline std::optional<std::string> get_teammate_color() {
    if (active_teammate_context && active_teammate_context->color && !active_teammate_context->color->empty()) {
        return active_teammate_context->color;
    }
    if (active_dynamic_team_context && active_dynamic_team_context->color && !active_dynamic_team_context->color->empty()) {
        return active_dynamic_team_context->color;
    }
    return detail::env_string({"LOOM_AGENT_COLOR", "LOOM_AGENT_COLOR"});
}

inline std::optional<std::string> get_agent_type() {
    if (active_teammate_context && active_teammate_context->agent_type && !active_teammate_context->agent_type->empty()) {
        return active_teammate_context->agent_type;
    }
    if (active_dynamic_team_context && active_dynamic_team_context->agent_type && !active_dynamic_team_context->agent_type->empty()) {
        return active_dynamic_team_context->agent_type;
    }
    return detail::env_string({"LOOM_AGENT_TYPE", "LOOM_AGENT_TYPE"});
}

inline bool is_plan_mode_required() {
    if (active_teammate_context) return active_teammate_context->plan_mode_required;
    if (active_dynamic_team_context) return active_dynamic_team_context->plan_mode_required;
    return detail::env_truthy({"LOOM_PLAN_MODE_REQUIRED", "LOOM_PLAN_MODE_REQUIRED"});
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
    std::lock_guard<std::mutex> lock(teammate_inbox_mutex());
    const auto inbox_path = fs::path{get_inbox_path(recipient_name, team_name)};
    ScopedInboxLock flock(inbox_path);
    if (!flock.locked()) {
        return std::unexpected("failed to acquire cross-process inbox lock");
    }
    auto existing = read_inbox(recipient_name, team_name);
    if (!existing) return std::unexpected(existing.error());
    if (message.timestamp.empty()) message.timestamp = detail::timestamp_now();
    message.read = false;
    existing->push_back(std::move(message));
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
    std::lock_guard<std::mutex> lock(teammate_inbox_mutex());
    const auto inbox_path = fs::path{get_inbox_path(agent_name, team_name)};
    ScopedInboxLock flock(inbox_path);
    if (!flock.locked()) {
        return std::unexpected("failed to acquire cross-process inbox lock");
    }
    auto messages = read_inbox(agent_name, team_name);
    if (!messages) return std::unexpected(messages.error());
    for (auto& message : *messages) message.read = true;
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

// ─── Canonical team config.json implementation ──────────────────────────────

inline std::string team_dir(std::string_view team_name) {
    return (detail::teams_dir() /
            detail::sanitize_path_component(team_name, "team")).string();
}

inline std::string team_file_path(std::string_view team_name) {
    return (fs::path{team_dir(team_name)} / "config.json").string();
}

namespace detail {

/// Parse one members[] row from config.json (camelCase keys from teamHelpers.ts).
[[nodiscard]] inline std::optional<TeamMemberRecord> parse_team_member(
    const cc::utils::json::JsonVal& value
) {
    if (!value.is_obj()) return std::nullopt;
    const auto agent_id = value.get("agentId");
    const auto name = value.get("name");
    if (!agent_id.is_str() || !name.is_str()) return std::nullopt;

    TeamMemberRecord member;
    member.agent_id = std::string(agent_id.as_str());
    member.name = std::string(name.as_str());
    member.color = json_optional_string(value, "color");
    member.backend = json_optional_string(value, "backendType");
    member.tmux_pane_id = value.get_string("tmuxPaneId");
    member.mode = json_optional_string(value, "mode");
    member.joined_at = value.get_int("joinedAt");
    member.cwd = value.get_string("cwd");
    const auto plan = value.get("planModeRequired");
    member.plan_mode_required = plan.is_bool() && plan.as_bool();
    const auto active = value.get("isActive");
    member.is_active = !active.is_bool() || active.as_bool();
    member.agent_type = json_optional_string(value, "agentType");
    member.model = json_optional_string(value, "model");
    member.prompt = json_optional_string(value, "prompt");
    member.worktree_path = json_optional_string(value, "worktreePath");
    member.session_id = json_optional_string(value, "sessionId");
    if (const auto subs = value.get("subscriptions"); subs.is_arr()) {
        subs.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) member.subscriptions.emplace_back(item.as_str());
        });
    }
    return member;
}

/// Emit an optional JSON string field, skipping absent values.
inline void append_optional_json_string(
    std::string& out, std::string_view key, const std::optional<std::string>& value
) {
    if (!value) return;
    out += ",\n      \"";
    out += key;
    out += R"(": ")";
    out += mailbox_json_escape(*value);
    out += '"';
}

} // namespace detail

inline std::optional<TeamFileRecord> read_team_file(std::string_view team_name) {
    const auto path = fs::path{team_file_path(team_name)};
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed) return std::nullopt;
    const auto root = parsed->root();
    if (!root.is_obj()) return std::nullopt;

    const auto lead = root.get("leadAgentId");
    const auto name = root.get("name");
    if (!lead.is_str() || !name.is_str()) return std::nullopt;

    TeamFileRecord file;
    file.name = std::string(name.as_str());
    file.description = detail::json_optional_string(root, "description");
    file.created_at = root.get_int("createdAt");
    file.lead_agent_id = std::string(lead.as_str());
    file.lead_session_id = detail::json_optional_string(root, "leadSessionId");
    if (const auto hidden = root.get("hiddenPaneIds"); hidden.is_arr()) {
        hidden.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) file.hidden_pane_ids.emplace_back(item.as_str());
        });
    }
    if (const auto members = root.get("members"); members.is_arr()) {
        members.iter([&](cc::utils::json::JsonVal item) {
            if (auto member = detail::parse_team_member(item)) {
                file.members.push_back(std::move(*member));
            }
        });
    }
    return file;
}

inline bool write_team_file(std::string_view team_name, const TeamFileRecord& record) {
    std::error_code ec;
    fs::create_directories(team_dir(team_name), ec);
    if (ec) return false;

    std::string out;
    out += "{\n";
    out += "  \"name\": \"" + detail::mailbox_json_escape(record.name) + "\",\n";
    if (record.description) {
        out += "  \"description\": \"" +
               detail::mailbox_json_escape(*record.description) + "\",\n";
    }
    out += "  \"createdAt\": " + std::to_string(record.created_at) + ",\n";
    out += "  \"leadAgentId\": \"" +
           detail::mailbox_json_escape(record.lead_agent_id) + '"';
    if (record.lead_session_id) {
        out += ",\n  \"leadSessionId\": \"" +
               detail::mailbox_json_escape(*record.lead_session_id) + '"';
    }
    out += ",\n  \"hiddenPaneIds\": [";
    for (std::size_t i = 0; i < record.hidden_pane_ids.size(); ++i) {
        if (i != 0) out += ", ";
        out += '"' + detail::mailbox_json_escape(record.hidden_pane_ids[i]) + '"';
    }
    out += "],\n";
    out += "  \"members\": [";
    if (record.members.empty()) {
        out += "]\n";
    } else {
        out += '\n';
        for (std::size_t i = 0; i < record.members.size(); ++i) {
            const auto& m = record.members[i];
            out += "    {\n";
            out += "      \"agentId\": \"" +
                   detail::mailbox_json_escape(m.agent_id) + "\",\n";
            out += "      \"name\": \"" +
                   detail::mailbox_json_escape(m.name) + "\"";
            detail::append_optional_json_string(out, "agentType", m.agent_type);
            detail::append_optional_json_string(out, "model", m.model);
            detail::append_optional_json_string(out, "prompt", m.prompt);
            detail::append_optional_json_string(out, "color", m.color);
            if (m.plan_mode_required) {
                out += ",\n      \"planModeRequired\": true";
            }
            out += ",\n      \"joinedAt\": " + std::to_string(m.joined_at);
            out += ",\n      \"tmuxPaneId\": \"" +
                   detail::mailbox_json_escape(m.tmux_pane_id) + '"';
            out += ",\n      \"cwd\": \"" +
                   detail::mailbox_json_escape(m.cwd) + '"';
            detail::append_optional_json_string(out, "worktreePath", m.worktree_path);
            detail::append_optional_json_string(out, "sessionId", m.session_id);
            detail::append_optional_json_string(out, "backendType", m.backend);
            if (!m.is_active) {
                out += ",\n      \"isActive\": false";
            }
            detail::append_optional_json_string(out, "mode", m.mode);
            out += ",\n      \"subscriptions\": [";
            for (std::size_t s = 0; s < m.subscriptions.size(); ++s) {
                if (s != 0) out += ", ";
                out += '"' + detail::mailbox_json_escape(m.subscriptions[s]) + '"';
            }
            out += "]\n";
            out += (i + 1 == record.members.size()) ? "    }\n" : "    },\n";
        }
        out += "  ]\n";
    }
    out += "}\n";

    std::ofstream file(fs::path{team_file_path(team_name)}, std::ios::trunc);
    if (!file) return false;
    file << out;
    return file.good();
}

inline std::optional<TeamMemberRecord> find_team_member(
    const TeamFileRecord& file, std::string_view name
) {
    for (const auto& member : file.members) {
        if (member.name == name) return member;
    }
    return std::nullopt;
}

inline std::optional<InitialTeamContext> compute_initial_team_context(
    std::string_view team_name,
    std::string_view agent_name,
    std::optional<std::string_view> agent_id
) {
    if (team_name.empty() || agent_name.empty()) return std::nullopt;

    const auto file = read_team_file(team_name);
    if (!file) return std::nullopt;  // Matches TS returning undefined on read failure.

    InitialTeamContext context;
    context.team_name = std::string(team_name);
    context.team_file_path = team_file_path(team_name);
    context.lead_agent_id = file->lead_agent_id;
    // reconnection.ts:51: isLeader = !agentId.
    context.is_leader = !agent_id || agent_id->empty();
    context.self_agent_id = (!context.is_leader)
        ? std::optional<std::string>{std::string(*agent_id)}
        : std::nullopt;
    context.self_agent_name = std::string(agent_name);
    return context;
}

inline std::optional<InitialTeamContext> compute_initial_team_context_from_env() {
    const auto team_name = get_team_name();
    if (!team_name || team_name->empty()) return std::nullopt;
    const auto agent_name = get_agent_name();
    if (!agent_name || agent_name->empty()) return std::nullopt;
    const auto agent_id = get_agent_id();
    return compute_initial_team_context(
        *team_name,
        *agent_name,
        agent_id ? std::optional<std::string_view>{*agent_id} : std::nullopt);
}

} // namespace cc::utils
