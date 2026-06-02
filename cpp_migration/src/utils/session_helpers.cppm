// C++23 Session Helpers Module
// Session title generation/formatting, file access hooks per session,
// session ingress authentication
module;

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.session_helpers;

export namespace cc::utils::session_helpers {

// ===========================================================================
// Session Title Generation
// ===========================================================================

/// Maximum conversation text length used for title generation input.
inline constexpr std::size_t MAX_CONVERSATION_TEXT = 1000;

/// Minimal message representation for title extraction.
struct TitleMessage {
    enum class Type : std::uint8_t { User, Assistant, Other };
    Type type = Type::Other;
    bool is_meta = false;

    /// Message origin kind: "human", "tool", "system", etc.
    std::string origin_kind;

    /// Content as a flat string (pre-extracted text blocks joined).
    std::string text_content;
};

/// Flatten a message array into a single text string for title generation input.
/// Skips meta/non-human messages. Tail-slices to the last MAX_CONVERSATION_TEXT
/// chars so recent context wins when the conversation is long.
[[nodiscard]] std::string extract_conversation_text(
    std::span<const TitleMessage> messages);

/// Backend function type for querying a lightweight model (Haiku) for title generation.
/// Takes a system prompt and user prompt, returns the model's response text.
using TitleQueryBackend = std::function<
    std::expected<std::string, std::string>(
        std::string_view system_prompt,
        std::string_view user_prompt)>;

/// Generate a sentence-case session title from a description or first message.
/// Returns std::nullopt on error or if the model returns an unparseable response.
///
/// The title is 3-7 words, sentence case, capturing the main topic/goal.
[[nodiscard]] std::expected<std::string, std::string> generate_session_title(
    std::string_view description,
    const TitleQueryBackend& backend);

// ===========================================================================
// Session File Access Hooks
// ===========================================================================

/// Detected session file type categories.
enum class SessionFileType : std::uint8_t {
    SessionMemory,
    SessionTranscript
};

/// Memory scope categories for telemetry.
enum class MemoryScope : std::uint8_t {
    User,
    Project,
    Session
};

/// Tool names tracked by session file access hooks.
inline constexpr std::string_view FILE_READ_TOOL = "Read";
inline constexpr std::string_view FILE_EDIT_TOOL = "Edit";
inline constexpr std::string_view FILE_WRITE_TOOL = "Write";
inline constexpr std::string_view GREP_TOOL = "Grep";
inline constexpr std::string_view GLOB_TOOL = "Glob";

/// Hook event types for file access tracking.
enum class FileAccessHookEvent : std::uint8_t {
    PreToolUse,
    PostToolUse
};

/// Input to the file access hook callback.
struct FileAccessHookInput {
    FileAccessHookEvent hook_event = FileAccessHookEvent::PostToolUse;
    std::string tool_name;
    std::string file_path;    // Extracted from tool input
    std::string pattern;      // For glob/grep tools
    std::string path_arg;     // path argument for grep/glob
};

/// Result from file access analysis — what was detected.
struct FileAccessResult {
    std::optional<SessionFileType> session_file_type;
    bool is_auto_mem_file = false;
    bool is_team_mem_file = false;
    std::optional<MemoryScope> memory_scope;
};

/// Callback for logging analytics events from file access hooks.
using FileAccessEventLogger = std::function<void(
    std::string_view event_name,
    const std::map<std::string, std::string>& properties)>;

/// Detect if a file path corresponds to a session memory or transcript file.
[[nodiscard]] std::optional<SessionFileType> detect_session_file_type(
    std::string_view file_path);

/// Detect session file type from a glob/search pattern.
[[nodiscard]] std::optional<SessionFileType> detect_session_pattern_type(
    std::string_view pattern);

/// Check if a path is an auto-memory file.
[[nodiscard]] bool is_auto_mem_file(std::string_view file_path);

/// Determine the memory scope for a given path.
[[nodiscard]] std::optional<MemoryScope> memory_scope_for_path(
    std::string_view file_path);

/// Check if a tool use constitutes a memory file access.
/// Detects session memory (via Read/Grep/Glob) and memdir access (via Read/Edit/Write).
[[nodiscard]] bool is_memory_file_access(
    std::string_view tool_name,
    std::string_view file_path);

/// Analyze a tool use for session file access and return what was detected.
[[nodiscard]] FileAccessResult analyze_file_access(
    const FileAccessHookInput& input);

/// Handle a PostToolUse event for session file access tracking.
/// Logs appropriate analytics events based on what file type was accessed.
void handle_session_file_access(
    const FileAccessHookInput& input,
    const FileAccessEventLogger& logger,
    std::optional<std::string_view> subagent_name = std::nullopt);

/// Hook matcher entry: which tool names trigger the hook.
struct HookMatcher {
    std::string tool_name;
};

/// Register session file access tracking hooks.
/// Returns the list of matchers for the PostToolUse hook.
[[nodiscard]] std::vector<HookMatcher> get_session_file_access_hook_matchers();

// ===========================================================================
// Session Ingress Authentication
// ===========================================================================

/// Authentication token source priority:
/// 1. Environment variable (CLAUDE_CODE_SESSION_ACCESS_TOKEN)
/// 2. File descriptor (CLAUDE_CODE_WEBSOCKET_AUTH_FILE_DESCRIPTOR)
/// 3. Well-known file path

/// Configuration for session ingress auth token resolution.
struct IngressAuthConfig {
    /// CLAUDE_CODE_SESSION_ACCESS_TOKEN env var value
    std::optional<std::string> env_token;

    /// CLAUDE_CODE_WEBSOCKET_AUTH_FILE_DESCRIPTOR env var value
    std::optional<std::string> fd_env;

    /// CLAUDE_SESSION_INGRESS_TOKEN_FILE env var override
    std::optional<std::filesystem::path> token_file_override;

    /// Default well-known token file path
    std::filesystem::path default_token_path;

    /// CLAUDE_CODE_ORGANIZATION_UUID env var value
    std::optional<std::string> organization_uuid;

    /// Platform identifier ("darwin", "linux", "freebsd")
    std::string platform;
};

/// Resolved auth headers to attach to requests.
struct AuthHeaders {
    std::map<std::string, std::string> headers;
};

/// Get session ingress authentication token.
///
/// Priority order:
///  1. Environment variable (CLAUDE_CODE_SESSION_ACCESS_TOKEN)
///  2. File descriptor (legacy path)
///  3. Well-known file
[[nodiscard]] std::optional<std::string> get_session_ingress_auth_token(
    const IngressAuthConfig& config);

/// Overload using a filesystem reader callback for testability.
using FileReader = std::function<std::expected<std::string, std::string>(
    const std::filesystem::path& path)>;

[[nodiscard]] std::optional<std::string> get_session_ingress_auth_token(
    const IngressAuthConfig& config,
    const FileReader& read_file);

/// Build auth headers for the current session token.
/// Session keys (sk-ant-sid) use Cookie auth + X-Organization-Uuid;
/// JWTs use Bearer auth.
[[nodiscard]] AuthHeaders get_session_ingress_auth_headers(
    const IngressAuthConfig& config);

/// Overload taking an explicit token.
[[nodiscard]] AuthHeaders get_session_ingress_auth_headers_from_token(
    std::string_view token,
    std::optional<std::string_view> organization_uuid = std::nullopt);

/// Update the session ingress auth token in-process.
/// Used by the REPL bridge to inject a fresh token after reconnection.
void update_session_ingress_auth_token(
    std::string& env_token_storage,
    std::string_view new_token);

/// Read a token from a well-known file path, returning nullopt on failure.
[[nodiscard]] std::optional<std::string> read_token_from_well_known_file(
    const std::filesystem::path& path);

/// Overload with a custom file reader for testing.
[[nodiscard]] std::optional<std::string> read_token_from_well_known_file(
    const std::filesystem::path& path,
    const FileReader& read_file);

} // namespace cc::utils::session_helpers
