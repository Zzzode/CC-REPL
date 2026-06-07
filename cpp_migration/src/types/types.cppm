/// @file types.cppm
/// @brief Core type definitions for the Claude Code CLI engine.
/// Defines message types, content blocks, strong ID types, token usage,
/// stream events, and error types using C++23 features.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <compare>

export module cc.types.types;

export namespace cc::core {

// ============================================================
// Strong type wrapper for domain-specific identifiers
// ============================================================

/// Generic strong type tag to prevent mixing different ID types
template <typename Tag>
struct StrongId {
    std::string value;

    auto operator<=>(const StrongId&) const = default;
    bool operator==(const StrongId&) const = default;

    /// Check if the ID is empty/uninitialized
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }

    /// Format support for std::format
    [[nodiscard]] const std::string& str() const noexcept { return value; }
};

// Tag types for strong IDs
struct ConversationIdTag {};
struct SessionIdTag {};
struct MessageIdTag {};
struct ToolUseIdTag {};

/// Unique conversation identifier
using ConversationId = StrongId<ConversationIdTag>;
/// Unique session identifier
using SessionId = StrongId<SessionIdTag>;
/// Unique message identifier
using MessageId = StrongId<MessageIdTag>;
/// Unique tool-use invocation identifier
using ToolUseId = StrongId<ToolUseIdTag>;

// ============================================================
// Role enum
// ============================================================

/// Participant role in a conversation
enum class Role : std::uint8_t {
    User,
    Assistant,
    System,
    Tool,
};

/// Convert Role to its string representation
[[nodiscard]] constexpr std::string_view role_to_string(Role role) noexcept {
    switch (role) {
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::System:    return "system";
        case Role::Tool:      return "tool";
    }
    return "unknown";
}

// ============================================================
// Content Blocks - variants representing different message content
// ============================================================

/// Plain text content block
struct TextBlock {
    std::string text;
};

/// Represents a tool invocation request from the assistant
struct ToolUseBlock {
    ToolUseId id;
    std::string name;           // Tool name (e.g., "Read", "Write")
    std::string input_json;     // JSON-encoded tool input parameters
};

/// Represents the result of a tool execution
struct ToolResultBlock {
    ToolUseId tool_use_id;
    std::string content;        // Result content (text or JSON)
    bool is_error = false;      // Whether the tool execution failed
};

/// Base64 image content block.
struct ImageBlock {
    std::string media_type;      // e.g. image/png
    std::string data;            // base64 encoded bytes
};

/// Base64 document content block.
struct DocumentBlock {
    std::string media_type;      // e.g. application/pdf
    std::string data;            // base64 encoded bytes
};

/// Extended thinking block from the model
struct ThinkingBlock {
    std::string thinking;       // Internal reasoning text
    std::string signature;      // Cryptographic signature for verification
};

/// Union of all possible content block types
using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock, ImageBlock, DocumentBlock, ThinkingBlock>;

// ============================================================
// Message types
// ============================================================

/// Base message structure containing common fields
struct MessageBase {
    MessageId id;
    std::chrono::system_clock::time_point timestamp;
    std::vector<ContentBlock> content;
};

/// Message sent by the user
struct UserMessage : MessageBase {
    static constexpr Role role = Role::User;
};

/// Response from the assistant (may contain tool use blocks)
struct AssistantMessage : MessageBase {
    static constexpr Role role = Role::Assistant;
    std::optional<std::string> stop_reason;   // "end_turn", "tool_use", "max_tokens"
    std::optional<std::string> model;         // Model used for this response
};

/// System prompt message
struct CompactPreservedSegment {
    std::string head_uuid;
    std::string anchor_uuid;
    std::string tail_uuid;
};

struct CompactMetadata {
    std::string trigger;
    std::uint32_t pre_tokens = 0;
    std::optional<CompactPreservedSegment> preserved_segment;
};

struct SnipMetadata {
    std::vector<std::string> removed_uuids;
};

struct SystemMessage : MessageBase {
    static constexpr Role role = Role::System;
    std::optional<std::string> cache_control; // Cache policy for this prompt
    std::optional<std::string> subtype;       // e.g. compact_boundary
    std::optional<CompactMetadata> compact_metadata;
    std::optional<SnipMetadata> snip_metadata;
};

/// Tool invocation message (assistant requesting tool use)
struct ToolUseMessage : MessageBase {
    static constexpr Role role = Role::Assistant;
    std::string tool_name;
    std::string tool_input_json;
};

/// Tool result message (tool execution output sent back)
struct ToolResultMessage : MessageBase {
    static constexpr Role role = Role::Tool;
    ToolUseId tool_use_id;
    bool is_error = false;
};

/// Union type for any conversation message
using Message = std::variant<
    UserMessage,
    AssistantMessage,
    SystemMessage,
    ToolUseMessage,
    ToolResultMessage
>;

/// Extract the role from any message variant
[[nodiscard]] inline Role get_role(const Message& msg) {
    return std::visit([](const auto& m) { return std::remove_cvref_t<decltype(m)>::role; }, msg);
}

// ============================================================
// Token Usage
// ============================================================

/// Tracks API token consumption for a request
struct TokenUsage {
    std::uint32_t input_tokens = 0;         // Prompt tokens consumed
    std::uint32_t output_tokens = 0;        // Completion tokens generated
    std::uint32_t cache_creation_tokens = 0; // Tokens used for cache creation
    std::uint32_t cache_read_tokens = 0;    // Tokens read from cache

    /// Total tokens consumed in this request
    [[nodiscard]] std::uint32_t total() const noexcept {
        return input_tokens + output_tokens;
    }

    /// Accumulate another usage record into this one
    TokenUsage& operator+=(const TokenUsage& other) noexcept {
        input_tokens += other.input_tokens;
        output_tokens += other.output_tokens;
        cache_creation_tokens += other.cache_creation_tokens;
        cache_read_tokens += other.cache_read_tokens;
        return *this;
    }
};

// ============================================================
// Stream Events - events emitted during streaming responses
// ============================================================

/// Indicates the stream has started
struct StreamStart {
    MessageId message_id;
    std::string model;
};

/// A content block has started (index identifies position)
struct ContentBlockStart {
    std::uint32_t index;
    ContentBlock block;         // Initial block state
};

/// Incremental text delta within a content block
struct ContentBlockDelta {
    std::uint32_t index;
    std::string delta_text;     // Appended text fragment
};

/// A content block has been fully received
struct ContentBlockStop {
    std::uint32_t index;
};

/// The entire message stream is complete
struct StreamEnd {
    std::optional<std::string> stop_reason;
    TokenUsage usage;
};

/// Stream-level error event
struct StreamError {
    std::string error_type;
    std::string message;
};

/// Union of all streaming events
using StreamEvent = std::variant<
    StreamStart,
    ContentBlockStart,
    ContentBlockDelta,
    ContentBlockStop,
    StreamEnd,
    StreamError
>;

// ============================================================
// Error types using std::expected
// ============================================================

/// Error severity levels
enum class ErrorSeverity : std::uint8_t {
    Warning,    // Non-fatal, operation may continue
    Error,      // Operation failed but system is stable
    Fatal,      // Unrecoverable, session must terminate
};

/// Categorized error codes for the engine
enum class ErrorCode : std::uint16_t {
    // Network errors (1xx)
    NetworkTimeout = 100,
    ConnectionFailed = 101,
    SSLError = 102,

    // API errors (2xx)
    AuthenticationFailed = 200,
    RateLimited = 201,
    OverloadedError = 202,
    InvalidRequest = 203,
    ContextWindowExceeded = 204,

    // Tool errors (3xx)
    ToolNotFound = 300,
    ToolExecutionFailed = 301,
    ToolPermissionDenied = 302,
    ToolTimeout = 303,

    // Session errors (4xx)
    SessionNotFound = 400,
    SessionExpired = 401,
    SessionCorrupted = 402,

    // Config errors (5xx)
    ConfigParseError = 500,
    ConfigNotFound = 501,
    ConfigWriteError = 502,

    // General errors (8xx)
    InvalidInput = 800,
    NotFound = 801,
    PermissionDenied = 802,

    // Internal errors (9xx)
    InternalError = 900,
    NotImplemented = 901,
};

/// Structured error type carrying context and diagnostics
struct Error {
    ErrorCode code;
    ErrorSeverity severity = ErrorSeverity::Error;
    std::string message;
    std::optional<std::string> detail;       // Extended diagnostic info
    std::optional<std::string> suggestion;   // Suggested remediation

    /// Create a simple error with message
    [[nodiscard]] static Error make(ErrorCode code, std::string message) {
        return Error{code, ErrorSeverity::Error, std::move(message), std::nullopt, std::nullopt};
    }

    /// Create a fatal error
    [[nodiscard]] static Error fatal(ErrorCode code, std::string message) {
        return Error{code, ErrorSeverity::Fatal, std::move(message), std::nullopt, std::nullopt};
    }

    /// Format error for display
    [[nodiscard]] std::string format() const {
        auto base = std::format("[E{:03d}] {}", static_cast<int>(code), message);
        if (detail) base += std::format("\n  Detail: {}", *detail);
        if (suggestion) base += std::format("\n  Suggestion: {}", *suggestion);
        return base;
    }
};

/// Standard result type used throughout the engine
template <typename T>
using Result = std::expected<T, Error>;

/// Void result for operations that either succeed or fail
using VoidResult = std::expected<void, Error>;

// ============================================================
// Utility concepts
// ============================================================

/// Concept for types that can be serialized to JSON
template <typename T>
concept JsonSerializable = requires(const T& t) {
    { t.to_json() } -> std::convertible_to<std::string>;
};

/// Concept for types that can be deserialized from JSON
template <typename T>
concept JsonDeserializable = requires(std::string_view json) {
    { T::from_json(json) } -> std::same_as<Result<T>>;
};

} // namespace cc::core
