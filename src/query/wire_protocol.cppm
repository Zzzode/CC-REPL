module;

#include <cstdint>

/// @file wire_protocol.cppm
/// @brief Backend wire-protocol seam: the interface a model API backend
///        implements so QueryEngine can talk to it without knowing the
///        vendor's JSON shape.
///
/// Two backends are targeted (owner decision 2026-09-19):
///   1. Anthropic-compatible  — the /v1/messages shape this project grew on.
///   2. OpenAI-compatible     — /v1/chat/completions, so any OpenAI-compatible
///                              endpoint (local llama.cpp, vLLM, Ollama,
///                              OpenRouter, ...) works with the same agent loop.
///
/// The engine owns the agent loop, tool execution, permissions and UI; a
/// backend owns ONLY the wire format:
///   * how a request body is serialized,
///   * what HTTP headers/URL it needs,
///   * how a response body (and a stream of SSE events) is parsed back into
///     the engine's vendor-neutral ContentBlock types.
///
/// Design note: the interface is intentionally narrow and value-oriented
/// (strings in, engine types out) rather than exposing vendor JSON types,
/// so a backend can be implemented and tested without any engine internals.

export module cc.query.wire_protocol;

import std;

import cc.types.types;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::query::wire {

using cc::core::AssistantMessage;
using cc::core::ContentBlock;
using cc::core::Message;
using cc::core::TokenUsage;
using cc::core::ToolDefinition;

// =========================================================================
// Backend identity
// =========================================================================

/// Which wire format a backend speaks.
enum class WireApi {
    Anthropic,  ///< POST {base}/v1/messages, SSE event: content_block_delta ...
    OpenAi,     ///< POST {base}/v1/chat/completions, SSE data: {choices:[...]}
};

[[nodiscard]] constexpr std::string_view wire_api_name(WireApi api) noexcept {
    switch (api) {
        case WireApi::Anthropic: return "anthropic";
        case WireApi::OpenAi:    return "openai";
    }
    return "unknown";
}

/// Resolve the wire API from a model-endpoint provider name. Bedrock/Vertex/
/// Foundry are Anthropic-shaped proxies today (they speak /v1/messages under
/// a different URL scheme), so they map to the Anthropic wire format; a
/// genuinely different vendor would add a case here.
/// Kept as a string-taking overload so this module does not need to import
/// the config module (which would add a dependency cycle risk).
[[nodiscard]] inline std::optional<WireApi> wire_api_from_provider(
    std::string_view provider_name) {
    if (provider_name == "anthropic" || provider_name == "bedrock" ||
        provider_name == "vertex" || provider_name == "foundry") {
        return WireApi::Anthropic;
    }
    if (provider_name == "openai_compat" || provider_name == "openai" ||
        provider_name == "compatible") {
        return WireApi::OpenAi;
    }
    return std::nullopt;
}

/// Resolve the wire API from a user-facing string (config / env).
/// Accepts: "anthropic", "openai", "openai-compatible", "openai_compat",
/// "compatible". Anything else yields nullopt so callers can fall back.
[[nodiscard]] inline std::optional<WireApi> wire_api_from_string(
    std::string_view name) {
    if (name == "anthropic" || name == "messages") return WireApi::Anthropic;
    if (name == "openai" || name == "openai-compatible" ||
        name == "openai_compat" || name == "compatible" ||
        name == "chat-completions") {
        return WireApi::OpenAi;
    }
    return std::nullopt;
}

// =========================================================================
// Request-side inputs
// =========================================================================

/// Everything a backend needs to serialize one request. Vendor-neutral: the
/// engine fills this in and hands it over.
struct RequestInput {
    std::string model;                       ///< model id as the user configured it
    std::uint32_t max_tokens = 4096;
    bool stream = false;
    std::string system_prompt;               ///< empty => omit
    std::vector<Message> messages;           ///< conversation, system excluded
    std::vector<ToolDefinition> tools;       ///< already deny/enable-filtered
    /// Verbatim MCP input schemas keyed by tool name; when present for a tool
    /// the backend must prefer it over ToolDefinition::input_schema.
    std::vector<std::pair<std::string, std::string>> tool_schemas;
    /// Extended thinking. `budget_tokens` unset means "adaptive"/backend-chosen.
    bool thinking_enabled = false;
    std::optional<std::uint32_t> thinking_budget_tokens;
    /// Emit the native computer-use tool shape when the backend has one.
    /// Anthropic => {"type":"computer_20241022",...}; OpenAI => ordinary
    /// function tool using the schema (the capability still works).
    bool native_computer_tool = false;
    int64_t computer_display_width = 1024;
    int64_t computer_display_height = 768;
    int64_t computer_display_number = 0;
    /// Optional sampling parameters. Unset => the backend omits the field
    /// entirely (which is what the engine did: it only emitted these when the
    /// config carried a value).
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<std::uint32_t> top_k;
};

/// A serialized request: what to POST and with which headers.
struct PreparedRequest {
    std::string url;                       ///< absolute
    std::string body;                      ///< JSON
    std::vector<std::pair<std::string, std::string>> headers;
};

// =========================================================================
// Response-side outputs
// =========================================================================

/// Parsed assistant turn, vendor-neutral.
struct ParsedResponse {
    AssistantMessage message;
    TokenUsage usage;
};

/// One decoded streaming event, reduced to what the engine acts on.
/// Backends translate their own event vocabulary into these.
struct StreamDelta {
    enum class Kind {
        TextDelta,        ///< append `text` to the current text block
        ThinkingDelta,    ///< append `text` to the current thinking block
        ToolUseStart,     ///< a tool call began: `tool_id`, `tool_name`
        ToolUseInputDelta,///< append `partial_json` to the open tool call
        BlockStop,        ///< current content block finished
        MessageStop,      ///< turn finished; `stop_reason` is set
        Usage,            ///< `usage` fields updated
        Error,            ///< `text` is the error message
        Ignored,          ///< event was well-formed but carries nothing we use
    };
    Kind kind = Kind::Ignored;
    std::string text;
    std::string tool_id;
    std::string tool_name;
    std::string partial_json;
    std::string stop_reason;
    TokenUsage usage;
};

// =========================================================================
// The backend interface
// =========================================================================

/// A model API backend. Implementations must be cheap to copy and stateless
/// with respect to a single request (the engine holds all conversation state).
class WireBackend {
public:
    virtual ~WireBackend() = default;

    [[nodiscard]] virtual WireApi api() const noexcept = 0;

    /// Serialize a request (URL + body + headers).
    [[nodiscard]] virtual std::expected<PreparedRequest, std::string>
    prepare(const RequestInput& input) const = 0;

    /// Parse a complete (non-streaming) response body.
    [[nodiscard]] virtual std::expected<ParsedResponse, std::string>
    parse_response(std::string_view body) const = 0;

    /// Decode ONE SSE frame into zero or more deltas. Backends receive the
    /// raw `event:` name (may be empty) and the raw `data:` payload.
    /// Returning several deltas is normal (e.g. a tool-call start that also
    /// carries the tool name).
    [[nodiscard]] virtual std::vector<StreamDelta> parse_stream_event(
        std::string_view event_name, std::string_view data) const = 0;

    /// Whether a stop_reason value means the model wants tools executed.
    /// Anthropic: "tool_use"; OpenAI: "tool_calls".
    [[nodiscard]] virtual bool stop_reason_is_tool_use(
        std::string_view stop_reason) const = 0;
};

} // namespace cc::query::wire
