/// @file logs.cppm
/// @brief Session log and transcript entry types.
/// Migrated from: src/types/logs.ts
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

export module cc.types.logs;

export namespace cc::types::logs {

/// Serialized message stored in session transcripts
struct SerializedMessage {
    std::string cwd;
    std::string user_type;
    std::optional<std::string> entrypoint;
    std::string session_id;
    std::string timestamp;
    std::string version;
    std::optional<std::string> git_branch;
    std::optional<std::string> slug;
};

/// Session mode discriminator
enum class SessionMode {
    coordinator,
    normal,
};

/// Persisted worktree session state for resume
struct PersistedWorktreeSession {
    std::string original_cwd;
    std::string worktree_path;
    std::string worktree_name;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> original_branch;
    std::optional<std::string> original_head_commit;
    std::string session_id;
    std::optional<std::string> tmux_session_name;
    std::optional<bool> hook_based;
};

/// Per-file attribution state tracking Claude's character contributions
struct FileAttributionState {
    std::string content_hash;      // SHA-256 hash of file content
    int64_t claude_contribution;   // Characters written by Claude
    int64_t mtime;                 // File modification time
};

/// Log option representing a session entry in the log list
struct LogOption {
    std::string date;
    std::optional<std::string> full_path;
    int64_t value;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point modified;
    std::string first_prompt;
    int64_t message_count;
    std::optional<int64_t> file_size;
    bool is_sidechain;
    std::optional<bool> is_lite;
    std::optional<std::string> session_id;
    std::optional<std::string> team_name;
    std::optional<std::string> agent_name;
    std::optional<std::string> agent_color;
    std::optional<std::string> agent_setting;
    std::optional<bool> is_teammate;
    std::optional<std::string> leaf_uuid;
    std::optional<std::string> summary;
    std::optional<std::string> custom_title;
    std::optional<std::string> tag;
    std::optional<std::string> git_branch;
    std::optional<std::string> project_path;
    std::optional<int32_t> pr_number;
    std::optional<std::string> pr_url;
    std::optional<std::string> pr_repository;
    std::optional<SessionMode> mode;
    // nullopt = never entered worktree; has_value with nullopt inner = exited
    std::optional<std::optional<PersistedWorktreeSession>> worktree_session;
};

/// Summary message appended to transcript
struct SummaryMessage {
    static constexpr std::string_view type = "summary";
    std::string leaf_uuid;
    std::string summary;
};

/// User-set custom title for a session
struct CustomTitleMessage {
    static constexpr std::string_view type = "custom-title";
    std::string session_id;
    std::string custom_title;
};

/// AI-generated session title (distinct from user custom title)
struct AiTitleMessage {
    static constexpr std::string_view type = "ai-title";
    std::string session_id;
    std::string ai_title;
};

/// Last prompt message for quick resume display
struct LastPromptMessage {
    static constexpr std::string_view type = "last-prompt";
    std::string session_id;
    std::string last_prompt;
};

/// Periodic fork-generated summary of current agent activity
struct TaskSummaryMessage {
    static constexpr std::string_view type = "task-summary";
    std::string session_id;
    std::string summary;
    std::string timestamp;
};

/// Tag message for session search
struct TagMessage {
    static constexpr std::string_view type = "tag";
    std::string session_id;
    std::string tag;
};

/// Agent name message
struct AgentNameMessage {
    static constexpr std::string_view type = "agent-name";
    std::string session_id;
    std::string agent_name;
};

/// Agent color message
struct AgentColorMessage {
    static constexpr std::string_view type = "agent-color";
    std::string session_id;
    std::string agent_color;
};

/// Agent setting message
struct AgentSettingMessage {
    static constexpr std::string_view type = "agent-setting";
    std::string session_id;
    std::string agent_setting;
};

/// PR link message stored in session transcript
struct PRLinkMessage {
    static constexpr std::string_view type = "pr-link";
    std::string session_id;
    int32_t pr_number;
    std::string pr_url;
    std::string pr_repository;  // e.g., "owner/repo"
    std::string timestamp;      // ISO timestamp when linked
};

/// Session mode entry
struct ModeEntry {
    static constexpr std::string_view type = "mode";
    std::string session_id;
    SessionMode mode;
};

/// Worktree state entry for resume
struct WorktreeStateEntry {
    static constexpr std::string_view type = "worktree-state";
    std::string session_id;
    std::optional<PersistedWorktreeSession> worktree_session; // nullopt = exited
};

/// Transcript message with parent/sidechain info
struct TranscriptMessage {
    SerializedMessage base;
    std::optional<std::string> parent_uuid;            // null for root
    std::optional<std::string> logical_parent_uuid;    // preserves logical parent
    bool is_sidechain;
    std::optional<std::string> git_branch;
    std::optional<std::string> agent_id;
    std::optional<std::string> team_name;
    std::optional<std::string> agent_name;
    std::optional<std::string> agent_color;
    std::optional<std::string> prompt_id;
};

/// Speculation accept message
struct SpeculationAcceptMessage {
    static constexpr std::string_view type = "speculation-accept";
    std::string timestamp;
    int64_t time_saved_ms;
};

/// File history snapshot message
struct FileHistorySnapshotMessage {
    static constexpr std::string_view type = "file-history-snapshot";
    std::string message_id;
    bool is_snapshot_update;
};

/// Attribution snapshot message
struct AttributionSnapshotMessage {
    static constexpr std::string_view type = "attribution-snapshot";
    std::string message_id;
    std::string surface;
    std::unordered_map<std::string, FileAttributionState> file_states;
    std::optional<int64_t> prompt_count;
    std::optional<int64_t> prompt_count_at_last_commit;
    std::optional<int64_t> permission_prompt_count;
    std::optional<int64_t> permission_prompt_count_at_last_commit;
    std::optional<int64_t> escape_count;
    std::optional<int64_t> escape_count_at_last_commit;
};

/// Context collapse commit entry (persisted for replay)
struct ContextCollapseCommitEntry {
    static constexpr std::string_view type = "marble-origami-commit";
    std::string session_id;
    std::string collapse_id;         // 16-digit collapse ID
    std::string summary_uuid;        // The summary placeholder's uuid
    std::string summary_content;     // Full <collapsed> string for placeholder
    std::string summary;             // Plain summary text
    std::string first_archived_uuid;
    std::string last_archived_uuid;
};

/// Staged collapse span info
struct StagedCollapseSpan {
    std::string start_uuid;
    std::string end_uuid;
    std::string summary;
    double risk;
    int64_t staged_at;
};

/// Context collapse snapshot entry (last-wins on restore)
struct ContextCollapseSnapshotEntry {
    static constexpr std::string_view type = "marble-origami-snapshot";
    std::string session_id;
    std::vector<StagedCollapseSpan> staged;
    bool armed;                  // Spawn trigger state
    int64_t last_spawn_tokens;
};

/// Discriminated union of all transcript entry types
using Entry = std::variant<
    TranscriptMessage,
    SummaryMessage,
    CustomTitleMessage,
    AiTitleMessage,
    LastPromptMessage,
    TaskSummaryMessage,
    TagMessage,
    AgentNameMessage,
    AgentColorMessage,
    AgentSettingMessage,
    PRLinkMessage,
    FileHistorySnapshotMessage,
    AttributionSnapshotMessage,
    SpeculationAcceptMessage,
    ModeEntry,
    WorktreeStateEntry,
    ContextCollapseCommitEntry,
    ContextCollapseSnapshotEntry
>;

/// Sort logs by modified date (newest first), ties broken by created date
inline void sort_logs(std::vector<LogOption>& logs) {
    std::ranges::sort(logs, [](const LogOption& a, const LogOption& b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified; // newest first
        }
        return a.created > b.created;
    });
}

} // namespace cc::types::logs
