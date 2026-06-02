// C++23 Message Processing Module
// Message array manipulation, content modifiers, tool use grouping,
// hook summary collapsing, teammate shutdown collapsing
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

export module cc.utils.message_processing;

export namespace cc::utils::message_processing {

// ===========================================================================
// Core Message Types
// ===========================================================================

/// Modifier key enum (keyboard modifiers)
enum class ModifierKey : std::uint8_t {
    Shift,
    Command,
    Control,
    Option
};

/// Check if a specific modifier key is currently pressed (platform-specific).
[[nodiscard]] bool is_modifier_pressed(ModifierKey modifier);

/// Pre-warm the native modifier detection module.
void prewarm_modifiers();

// ---------------------------------------------------------------------------
// Content block types for messages
// ---------------------------------------------------------------------------

struct TextContentBlock {
    std::string text;
};

struct ImageContentBlock {
    std::string media_type;
    std::string source_data;  // base64
};

struct ToolUseContentBlock {
    std::string id;
    std::string name;
    std::string input_json;  // Serialized JSON
};

struct ToolResultContentBlock {
    std::string tool_use_id;
    bool is_error = false;
    std::string content;  // Serialized content
};

using ContentBlock = std::variant<
    TextContentBlock,
    ImageContentBlock,
    ToolUseContentBlock,
    ToolResultContentBlock>;

// ---------------------------------------------------------------------------
// Message structure
// ---------------------------------------------------------------------------

/// Message role
enum class MessageRole : std::uint8_t { User, Assistant };

/// Message type discriminator
enum class MessageType : std::uint8_t {
    User,
    Assistant,
    Attachment,
    System,
    Progress
};

/// System message subtypes
enum class SystemSubtype : std::uint8_t {
    ApiError,
    LocalCommand,
    Informational,
    StopHookSummary,
    AgentsKilled,
    Other
};

/// Hook event type
enum class HookEvent : std::uint8_t {
    PreToolUse,
    PostToolUse,
    Other
};

/// A normalized message with a single content block
struct NormalizedMessage {
    MessageType type = MessageType::User;
    std::string uuid;
    std::string timestamp;
    std::vector<ContentBlock> content;
    std::string message_id;  // API message ID for grouping

    // Metadata flags
    bool is_meta = false;
    bool is_virtual = false;
    bool is_api_error_message = false;

    // System message specifics
    SystemSubtype system_subtype = SystemSubtype::Other;

    // Attachment specifics
    struct AttachmentInfo {
        std::string type;
        HookEvent hook_event = HookEvent::Other;
        std::string tool_use_id;
        std::string hook_name;
        // For teammate shutdown
        std::string task_type;
        std::string status;
    };
    std::optional<AttachmentInfo> attachment;

    // Progress specifics
    struct ProgressInfo {
        std::string type;  // "hook_progress" etc.
        HookEvent hook_event = HookEvent::Other;
        std::string parent_tool_use_id;
    };
    std::optional<ProgressInfo> progress;
};

/// A renderable message (normalized + grouped variants)
struct GroupedToolUseMessage {
    std::string tool_name;
    std::string message_id;
    std::string uuid;
    std::string timestamp;
    std::vector<NormalizedMessage> messages;
    std::vector<NormalizedMessage> results;
};

using RenderableMessage = std::variant<NormalizedMessage, GroupedToolUseMessage>;

// ===========================================================================
// Message Constants
// ===========================================================================

inline constexpr std::string_view INTERRUPT_MESSAGE = "[Request interrupted by user]";
inline constexpr std::string_view INTERRUPT_MESSAGE_FOR_TOOL_USE =
    "[Request interrupted by user for tool use]";
inline constexpr std::string_view CANCEL_MESSAGE =
    "The user doesn't want to take this action right now. "
    "STOP what you are doing and wait for the user to tell you how to proceed.";
inline constexpr std::string_view REJECT_MESSAGE =
    "The user doesn't want to proceed with this tool use. The tool use was rejected "
    "(eg. if it was a file edit, the new_string was NOT written to the file). "
    "STOP what you are doing and wait for the user to tell you how to proceed.";
inline constexpr std::string_view REJECT_MESSAGE_WITH_REASON_PREFIX =
    "The user doesn't want to proceed with this tool use. The tool use was rejected "
    "(eg. if it was a file edit, the new_string was NOT written to the file). "
    "To tell you how to proceed, the user said:\n";
inline constexpr std::string_view NO_RESPONSE_REQUESTED = "No response requested.";
inline constexpr std::string_view SYNTHETIC_TOOL_RESULT_PLACEHOLDER =
    "[Tool result missing due to internal error]";

// ===========================================================================
// Message Utility Functions
// ===========================================================================

/// Derive a short stable message ID (6-char base36 string) from a UUID.
[[nodiscard]] std::string derive_short_message_id(std::string_view uuid);

/// Deterministic UUID derivation from a parent UUID + content block index.
[[nodiscard]] std::string derive_uuid(std::string_view parent_uuid, std::size_t index);

/// Check if a message is a synthetic/system-generated message.
[[nodiscard]] bool is_synthetic_message(const NormalizedMessage& message);

/// Check if a tool result message is a classifier denial.
[[nodiscard]] bool is_classifier_denial(std::string_view content);

/// Build a rejection message for auto mode classifier denials.
[[nodiscard]] std::string build_yolo_rejection_message(std::string_view reason);

/// Build a message for when the auto mode classifier is temporarily unavailable.
[[nodiscard]] std::string build_classifier_unavailable_message(
    std::string_view tool_name,
    std::string_view classifier_model);

/// Build an auto-reject permission denial message.
[[nodiscard]] std::string auto_reject_message(std::string_view tool_name);

/// Append a memory correction hint to a message when auto-memory is enabled.
[[nodiscard]] std::string with_memory_correction_hint(
    std::string_view message,
    bool auto_memory_enabled,
    bool feature_gate_enabled);

/// Check if a message is not empty (has meaningful content).
[[nodiscard]] bool is_not_empty_message(const NormalizedMessage& message);

/// Check if message is a tool use request.
[[nodiscard]] bool is_tool_use_request_message(const NormalizedMessage& message);

/// Check if message is a tool use result.
[[nodiscard]] bool is_tool_use_result_message(const NormalizedMessage& message);

/// Extract the tool_use_id from a message (if any).
[[nodiscard]] std::optional<std::string> get_tool_use_id(const NormalizedMessage& message);

/// Extract tag content from XML-like markup.
[[nodiscard]] std::optional<std::string> extract_tag(
    std::string_view html, std::string_view tag_name);

// ===========================================================================
// Message Normalization
// ===========================================================================

/// Split messages so each content block gets its own NormalizedMessage.
[[nodiscard]] std::vector<NormalizedMessage> normalize_messages(
    std::span<const NormalizedMessage> messages);

/// Reorder messages in the UI so tool results follow their tool use messages.
[[nodiscard]] std::vector<NormalizedMessage> reorder_messages_in_ui(
    std::span<const NormalizedMessage> messages,
    std::span<const NormalizedMessage> synthetic_streaming_tool_use_messages);

/// Reorder attachments for API: bubble up until they hit a tool result or assistant message.
[[nodiscard]] std::vector<NormalizedMessage> reorder_attachments_for_api(
    std::span<const NormalizedMessage> messages);

// ===========================================================================
// Message Lookups (pre-computed O(1) access to message relationships)
// ===========================================================================

/// Pre-computed lookups for efficient access to message relationships.
struct MessageLookups {
    std::unordered_map<std::string, std::unordered_set<std::string>> sibling_tool_use_ids;
    std::unordered_map<std::string, std::vector<NormalizedMessage>> progress_messages_by_tool_use_id;
    std::unordered_map<std::string, std::unordered_map<HookEvent, std::size_t>> in_progress_hook_counts;
    std::unordered_map<std::string, std::unordered_map<HookEvent, std::size_t>> resolved_hook_counts;
    std::unordered_map<std::string, NormalizedMessage> tool_result_by_tool_use_id;
    std::unordered_map<std::string, ToolUseContentBlock> tool_use_by_tool_use_id;
    std::size_t normalized_message_count = 0;
    std::unordered_set<std::string> resolved_tool_use_ids;
    std::unordered_set<std::string> errored_tool_use_ids;
};

/// Build pre-computed lookups for efficient O(1) access to message relationships.
[[nodiscard]] MessageLookups build_message_lookups(
    std::span<const NormalizedMessage> normalized_messages,
    std::span<const NormalizedMessage> raw_messages);

/// Check for unresolved hooks using pre-computed lookup.
[[nodiscard]] bool has_unresolved_hooks_from_lookup(
    std::string_view tool_use_id,
    HookEvent hook_event,
    const MessageLookups& lookups);

// ===========================================================================
// Tool Use Grouping
// ===========================================================================

/// Result of applying tool use grouping.
struct GroupingResult {
    std::vector<RenderableMessage> messages;
};

/// Groups tool uses by message.id (same API response) if the tool supports grouped rendering.
/// Only groups 2+ tools of the same type from the same message.
[[nodiscard]] GroupingResult apply_grouping(
    std::span<const NormalizedMessage> messages,
    const std::unordered_set<std::string>& tools_with_grouping,
    bool verbose);

/// Overload without verbose flag (defaults to false).
[[nodiscard]] GroupingResult apply_grouping(
    std::span<const NormalizedMessage> messages,
    const std::unordered_set<std::string>& tools_with_grouping);

// ===========================================================================
// Hook Summary Collapsing
// ===========================================================================

/// Stop hook info for summary messages.
struct StopHookInfo {
    std::string name;
    std::optional<std::string> output;
};

/// A hook summary system message with collapsible fields.
struct HookSummaryMessage {
    std::string uuid;
    std::string timestamp;
    std::string hook_label;
    std::size_t hook_count = 0;
    std::vector<StopHookInfo> hook_infos;
    std::vector<std::string> hook_errors;
    bool prevented_continuation = false;
    bool has_output = false;
    std::optional<double> total_duration_ms;
};

/// Collapses consecutive hook summary messages with the same hookLabel
/// into a single summary. Parallel tool calls each emit their own hook summary.
[[nodiscard]] std::vector<RenderableMessage> collapse_hook_summaries(
    std::span<const RenderableMessage> messages);

// ===========================================================================
// Teammate Shutdown Collapsing
// ===========================================================================

/// A collapsed batch of teammate shutdown attachments.
struct TeammateShutdownBatch {
    std::string uuid;
    std::string timestamp;
    std::size_t count = 0;
};

/// Collapses consecutive in-process teammate shutdown task_status attachments
/// into a single batch with a count.
[[nodiscard]] std::vector<RenderableMessage> collapse_teammate_shutdowns(
    std::span<const RenderableMessage> messages);

} // namespace cc::utils::message_processing
