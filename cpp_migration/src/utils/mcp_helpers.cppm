// C++23 MCP Helpers Module
// MCP instructions delta computation, output storage/retrieval, validation helpers,
// and WebSocket transport for MCP protocol
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.mcp_helpers;

import cc.utils.json;

export namespace cc::utils::mcp_helpers {

using cc::utils::json::JsonVal;

// ===========================================================================
// MCP Instructions Delta
// ===========================================================================

/// Represents a change in announced MCP server instructions.
/// Used for stateless-scan reconstruction of announced servers.
struct McpInstructionsDelta {
    std::vector<std::string> added_names;   // Server names being added
    std::vector<std::string> added_blocks;  // Rendered "## {name}\n{instructions}" blocks
    std::vector<std::string> removed_names; // Server names being removed
};

/// Client-authored instruction block to announce when a server connects,
/// in addition to (or instead of) the server's own InitializeResult.instructions.
struct ClientSideInstruction {
    std::string server_name;
    std::string block;
};

/// Minimal representation of an MCP server connection for delta computation
struct McpServerConnection {
    enum class Type { Connected, Disconnected, Connecting };
    Type type = Type::Disconnected;
    std::string name;
    std::optional<std::string> instructions;
};

/// Represents a message with an attachment for delta scanning
struct McpMessage {
    enum class Type { User, Assistant, Attachment, System, Progress };
    Type type = Type::User;

    // Only relevant when type == Attachment
    struct AttachmentData {
        std::string type;  // "mcp_instructions_delta" etc.
        std::vector<std::string> added_names;
        std::vector<std::string> removed_names;
    };
    std::optional<AttachmentData> attachment;
};

/// Check if MCP instructions delta mode is enabled.
/// Env override: CLAUDE_CODE_MCP_INSTR_DELTA=true/false
[[nodiscard]] bool is_mcp_instructions_delta_enabled(
    std::string_view env_override,
    std::string_view user_type,
    bool feature_gate_value);

/// Compute the delta between currently connected MCP servers with instructions
/// and what's already been announced in the conversation.
/// Returns std::nullopt if nothing changed.
[[nodiscard]] std::optional<McpInstructionsDelta> get_mcp_instructions_delta(
    std::span<const McpServerConnection> mcp_clients,
    std::span<const McpMessage> messages,
    std::span<const ClientSideInstruction> client_side_instructions);

// ===========================================================================
// MCP Output Storage
// ===========================================================================

/// MCP result type categories
enum class McpResultType {
    ToolResult,
    StructuredContent,
    ContentArray
};

/// Generates a format description string based on the MCP result type and schema.
[[nodiscard]] std::string get_format_description(
    McpResultType type,
    std::optional<std::string_view> schema = std::nullopt);

/// Generates instruction text for reading from a saved large-output file.
[[nodiscard]] std::string get_large_output_instructions(
    std::string_view raw_output_path,
    std::size_t content_length,
    std::string_view format_description);

/// Overload with optional max read length for Bash output context.
[[nodiscard]] std::string get_large_output_instructions(
    std::string_view raw_output_path,
    std::size_t content_length,
    std::string_view format_description,
    std::size_t max_read_length);

/// Map a MIME type to a file extension.
/// Known types get their proper extension; unknown types get "bin".
[[nodiscard]] std::string extension_for_mime_type(std::string_view mime_type);

/// Heuristic for whether a content-type header indicates binary content
/// that should be saved to disk rather than put into the model context.
[[nodiscard]] bool is_binary_content_type(std::string_view content_type);

/// Result of persisting binary content
struct PersistBinaryResult {
    std::string filepath;
    std::size_t size = 0;
    std::string ext;
};

/// Write raw binary bytes to the tool-results directory with a mime-derived extension.
[[nodiscard]] std::expected<PersistBinaryResult, std::string> persist_binary_content(
    std::span<const std::uint8_t> bytes,
    std::string_view mime_type,
    std::string_view persist_id,
    const std::filesystem::path& results_dir);

/// Build a short message telling Claude where binary content was saved.
[[nodiscard]] std::string get_binary_blob_saved_message(
    std::string_view filepath,
    std::string_view mime_type,
    std::size_t size,
    std::string_view source_description);

// ===========================================================================
// MCP Content Validation / Truncation
// ===========================================================================

/// Token count threshold factor for size-based heuristic
inline constexpr double MCP_TOKEN_COUNT_THRESHOLD_FACTOR = 0.5;

/// Estimated token count for a single image block
inline constexpr std::size_t IMAGE_TOKEN_ESTIMATE = 1600;

/// Default maximum MCP output tokens
inline constexpr std::size_t DEFAULT_MAX_MCP_OUTPUT_TOKENS = 25000;

/// Content block variant for MCP tool results
struct TextBlock {
    std::string text;
};

struct ImageBlock {
    std::string media_type;
    std::string data;  // base64
};

using ContentBlock = std::variant<TextBlock, ImageBlock>;

/// MCP tool result: can be a plain string, a sequence of content blocks, or empty
using McpToolResult = std::variant<std::monostate, std::string, std::vector<ContentBlock>>;

/// Resolve the MCP output token cap from env/feature-flag/default.
[[nodiscard]] std::size_t get_max_mcp_output_tokens(
    std::optional<std::string_view> env_value = std::nullopt,
    std::optional<std::size_t> feature_override = std::nullopt);

/// Estimate the content size in tokens for an MCP tool result.
[[nodiscard]] std::size_t get_content_size_estimate(const McpToolResult& content);

/// Truncate an MCP tool result to fit within the token budget.
[[nodiscard]] McpToolResult truncate_mcp_content(
    const McpToolResult& content,
    std::size_t max_tokens);

/// Truncate only if needed (heuristic check first).
[[nodiscard]] McpToolResult truncate_mcp_content_if_needed(
    const McpToolResult& content,
    std::size_t max_tokens);

// ===========================================================================
// MCP WebSocket Transport
// ===========================================================================

/// WebSocket ready states
enum class WebSocketState : std::uint8_t {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3
};

/// JSON-RPC message (opaque JSON value)
using JsonRpcMessage = JsonVal;

/// Callbacks for the WebSocket transport layer
struct TransportCallbacks {
    std::function<void()> on_close;
    std::function<void(std::string_view error)> on_error;
    std::function<void(const JsonRpcMessage&)> on_message;
};

/// Abstract WebSocket interface — implementations wrap platform-specific sockets
struct IWebSocket {
    virtual ~IWebSocket() = default;
    [[nodiscard]] virtual WebSocketState ready_state() const = 0;
    virtual void close() = 0;
    virtual std::expected<void, std::string> send(std::string_view data) = 0;
};

/// MCP WebSocket Transport implementing the MCP Transport interface.
/// Manages lifecycle and message parsing for a JSON-RPC over WebSocket connection.
class WebSocketTransport {
public:
    explicit WebSocketTransport(std::unique_ptr<IWebSocket> ws);
    ~WebSocketTransport();

    // Non-copyable, moveable
    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;
    WebSocketTransport(WebSocketTransport&&) noexcept;
    WebSocketTransport& operator=(WebSocketTransport&&) noexcept;

    /// Set transport event callbacks
    void set_callbacks(TransportCallbacks callbacks);

    /// Start listening for messages on the WebSocket.
    /// Can only be called once per transport instance.
    [[nodiscard]] std::expected<void, std::string> start();

    /// Close the WebSocket connection.
    void close();

    /// Send a JSON-RPC message over the WebSocket connection.
    [[nodiscard]] std::expected<void, std::string> send(const JsonRpcMessage& message);

    /// Check if the transport has been started.
    [[nodiscard]] bool is_started() const noexcept { return started_; }

private:
    std::unique_ptr<IWebSocket> ws_;
    bool started_ = false;
    TransportCallbacks callbacks_;

    void handle_error(std::string_view error);
    void handle_close();
    void handle_message(std::string_view raw_data);
};

} // namespace cc::utils::mcp_helpers
