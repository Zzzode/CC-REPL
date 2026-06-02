/// @file conversation_recovery.cppm
/// @brief Conversation crash recovery, cross-project session resume, recovery state persistence
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include <chrono>
#include <unordered_map>

export module cc.utils.conversation_recovery;

export namespace cc::utils::conversation_recovery {

// ---------------------------------------------------------------------------
// Turn interruption detection
// ---------------------------------------------------------------------------

/// Kind of turn interruption detected during deserialization
enum class TurnInterruptionKind : std::uint8_t {
    /// No interruption — conversation ended cleanly
    None,
    /// User submitted a prompt but assistant never responded
    InterruptedPrompt,
};

/// State of turn interruption with optional message payload
struct TurnInterruptionState {
    TurnInterruptionKind kind{TurnInterruptionKind::None};
    /// The interrupted user message (set when kind == InterruptedPrompt)
    std::optional<std::string> interrupted_message;
};

// ---------------------------------------------------------------------------
// Serialized message types (simplified)
// ---------------------------------------------------------------------------

/// Session metadata for restoring agent context
struct SessionMetadata {
    std::optional<std::string> agent_name;
    std::optional<std::string> agent_color;
    std::optional<std::string> agent_setting;
    std::optional<std::string> custom_title;
    std::optional<std::string> tag;
    std::optional<std::string> mode;   // "coordinator" | "normal"
    std::optional<int> pr_number;
    std::optional<std::string> pr_url;
    std::optional<std::string> pr_repository;
    std::optional<std::string> full_path;
};

/// File history snapshot entry
struct FileHistorySnapshot {
    std::string file_path;
    std::string content_hash;
    std::chrono::system_clock::time_point timestamp;
};

/// Result of loading a conversation for resume
struct LoadConversationResult {
    std::vector<std::string> messages;
    TurnInterruptionState turn_interruption_state;
    std::vector<FileHistorySnapshot> file_history_snapshots;
    std::optional<std::string> session_id;
    SessionMetadata metadata;
};

// ---------------------------------------------------------------------------
// Deserialization
// ---------------------------------------------------------------------------

/// Result of deserializing messages with interruption detection
struct DeserializeResult {
    std::vector<std::string> messages;
    TurnInterruptionState turn_interruption_state;
};

/// Deserialize messages from a log file into the format expected by the REPL.
/// Filters unresolved tool uses, orphaned thinking messages, and handles
/// turn interruption detection.
[[nodiscard]] auto deserialize_messages(
    const std::vector<std::string>& serialized_messages) -> DeserializeResult;

// ---------------------------------------------------------------------------
// Conversation loading for resume
// ---------------------------------------------------------------------------

/// Source for loading a conversation
enum class ResumeSource : std::uint8_t {
    /// Load most recent conversation
    MostRecent,
    /// Load by session ID
    ById,
    /// Load from a jsonl file path
    FromFile,
};

/// Parameters for loading a conversation
struct LoadConversationParams {
    ResumeSource source{ResumeSource::MostRecent};
    /// Session ID (when source == ById)
    std::optional<std::string> session_id;
    /// Path to jsonl file (when source == FromFile)
    std::optional<std::filesystem::path> jsonl_path;
};

/// Load a conversation for resume from various sources.
/// Centralized function for loading and deserializing conversations.
[[nodiscard]] auto load_conversation_for_resume(
    const LoadConversationParams& params)
    -> std::expected<LoadConversationResult, std::string>;

/// Load messages from a specific jsonl transcript file path
[[nodiscard]] auto load_messages_from_jsonl_path(
    const std::filesystem::path& path)
    -> std::expected<std::vector<std::string>, std::string>;

// ---------------------------------------------------------------------------
// Skill state recovery
// ---------------------------------------------------------------------------

/// Restores skill state from invoked_skills attachments in messages.
/// Ensures skills are preserved across resume after compaction.
void restore_skill_state_from_messages(
    const std::vector<std::string>& messages);

// ---------------------------------------------------------------------------
// Cross-project resume
// ---------------------------------------------------------------------------

/// Result of cross-project resume check
enum class CrossProjectKind : std::uint8_t {
    /// Same project — normal resume
    SameProject,
    /// Different project but same repo worktree — direct resume OK
    SameRepoWorktree,
    /// Different project entirely — requires cd
    DifferentProject,
};

/// Cross-project resume detection result
struct CrossProjectResumeResult {
    CrossProjectKind kind{CrossProjectKind::SameProject};
    /// The project path for cross-project sessions
    std::optional<std::string> project_path;
    /// Shell command to resume (when kind == DifferentProject)
    std::optional<std::string> command;
};

/// Check if a log is from a different project directory and determine
/// whether it's a related worktree or a completely different project.
[[nodiscard]] auto check_cross_project_resume(
    std::string_view log_project_path,
    std::string_view current_cwd,
    bool show_all_projects,
    const std::vector<std::string>& worktree_paths)
    -> CrossProjectResumeResult;

// ---------------------------------------------------------------------------
// Recovery state persistence
// ---------------------------------------------------------------------------

/// Recovery state stored on disk for crash recovery
struct RecoveryState {
    std::string session_id;
    std::filesystem::path transcript_path;
    std::string project_path;
    std::chrono::system_clock::time_point last_active;
    std::optional<std::string> branch_name;
};

/// Persist current session state for crash recovery
[[nodiscard]] auto persist_recovery_state(const RecoveryState& state)
    -> std::expected<void, std::string>;

/// Load the most recent recovery state (if any)
[[nodiscard]] auto load_recovery_state()
    -> std::expected<std::optional<RecoveryState>, std::string>;

/// Clear recovery state after successful session end
[[nodiscard]] auto clear_recovery_state()
    -> std::expected<void, std::string>;

/// Check resume consistency of loaded messages
[[nodiscard]] auto check_resume_consistency(
    const std::vector<std::string>& messages)
    -> std::expected<void, std::string>;

} // namespace cc::utils::conversation_recovery
