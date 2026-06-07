/// @file sdk_message_adapter.cppm
/// @brief Converts SDK-format messages from the CCR backend WebSocket into
///        internal REPL message types for rendering and processing.
module;

#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <optional>
#include <chrono>
#include <functional>
#include <stdexcept>

export module cc.remote.sdk_message_adapter;

export import cc.bridge.messages;

export namespace cc::remote {

// ============================================================
// Internal message types (REPL rendering format)
// ============================================================

/// Plain text content block
struct InternalTextBlock {
    std::string text;
};

/// Tool invocation block
struct InternalToolUseBlock {
    std::string id;
    std::string name;
    std::string input_json;
};

/// Tool result returned from execution
struct InternalToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

/// Image block (base64-encoded)
struct InternalImageBlock {
    std::string media_type;
    std::string data;
};

/// Discriminated union over all content block shapes
using InternalContentBlock = std::variant<
    InternalTextBlock,
    InternalToolUseBlock,
    InternalToolResultBlock,
    InternalImageBlock
>;

/// Internal message used by the REPL for rendering
struct InternalMessage {
    std::string role;   // "user", "assistant", "system"
    std::vector<InternalContentBlock> content;
    std::optional<std::string> uuid;
    std::string type;   // "user", "assistant", "system", "result"
    std::optional<std::string> subtype;
    bool is_virtual = false;
    std::string timestamp;
};

// ============================================================
// SDK message types (from CCR WebSocket)
// ============================================================

/// SDK content block received from the backend
struct SdkContentBlock {
    std::string type;  // "text", "tool_use", "tool_result", "image"
    std::string text;
    std::string id;
    std::string name;
    std::string input_json;
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
    std::string media_type;
    std::string data;
};

/// Assistant message from the remote agent
struct SdkAssistantMessage {
    std::string session_id;
    std::string uuid;
    std::vector<SdkContentBlock> content;
    std::optional<std::string> error;
    std::string parent_tool_use_id;
};

/// User message (may contain tool_result blocks)
struct SdkUserMessage {
    std::string session_id;
    std::optional<std::string> uuid;
    std::vector<SdkContentBlock> content;
    std::optional<std::string> tool_use_result;
    bool is_synthetic = false;
    std::string parent_tool_use_id;
    std::optional<std::string> timestamp;
};

/// System message (init, status, hook_response, etc.)
struct SdkSystemMessage {
    std::string session_id;
    std::string uuid;
    std::string subtype;
    std::string message;
    std::optional<std::string> model;
    std::optional<std::string> status;
};

/// Result message (success or error) at session end
struct SdkResultMessage {
    std::string session_id;
    std::string uuid;
    std::string subtype;   // "success" or error variant
    bool is_error = false;
    std::string result;
    std::vector<std::string> errors;
    int duration_ms = 0;
    int num_turns = 0;
    double total_cost_usd = 0.0;
};

/// Streaming partial assistant message
struct SdkStreamEventMessage {
    std::string session_id;
    std::string uuid;
    std::string event_json;
    std::string parent_tool_use_id;
};

/// Tool progress notification
struct SdkToolProgressMessage {
    std::string session_id;
    std::string uuid;
    std::string tool_use_id;
    std::string tool_name;
    int elapsed_time_seconds = 0;
    std::string parent_tool_use_id;
};

/// Compact boundary marker after conversation compaction
struct SdkCompactBoundaryMessage {
    std::string session_id;
    std::string uuid;
    std::string trigger;   // "manual" or "auto"
    int pre_tokens = 0;
};

/// Tagged union of all SDK message shapes from the WebSocket
using SdkMessage = std::variant<
    SdkAssistantMessage,
    SdkUserMessage,
    SdkSystemMessage,
    SdkResultMessage,
    SdkStreamEventMessage,
    SdkToolProgressMessage,
    SdkCompactBoundaryMessage
>;

// ============================================================
// Conversion result
// ============================================================

/// Opaque stream event payload
struct StreamEvent {
    std::string event_json;
};

/// Result of converting an SDK message: either a concrete internal
/// message, a stream event, or a signal to ignore the message.
struct ConvertedMessage {
    enum class Tag { Message, StreamEvent, Ignored };
    Tag tag = Tag::Ignored;

    std::optional<InternalMessage> message;
    std::optional<StreamEvent> stream_event;
};

/// Options that control which user messages are converted
struct ConvertOptions {
    bool convert_tool_results = false;
    bool convert_user_text_messages = false;
};

// ============================================================
// Content-block conversion
// ============================================================

/// Convert a single SDK content block to internal representation.
inline InternalContentBlock convert_sdk_content_block(const SdkContentBlock& block) {
    if (block.type == "text") {
        return InternalTextBlock{.text = block.text};
    }
    if (block.type == "tool_use") {
        return InternalToolUseBlock{
            .id = block.id,
            .name = block.name,
            .input_json = block.input_json
        };
    }
    if (block.type == "tool_result") {
        return InternalToolResultBlock{
            .tool_use_id = block.tool_use_id,
            .content = block.content,
            .is_error = block.is_error
        };
    }
    if (block.type == "image") {
        return InternalImageBlock{
            .media_type = block.media_type,
            .data = block.data
        };
    }
    // Unknown block types fall back to a text block with the raw content
    return InternalTextBlock{.text = block.content};
}

/// Convert a vector of SDK content blocks to internal content blocks.
inline std::vector<InternalContentBlock> convert_sdk_content_blocks(
    const std::vector<SdkContentBlock>& blocks
) {
    std::vector<InternalContentBlock> result;
    result.reserve(blocks.size());
    for (const auto& block : blocks) {
        result.push_back(convert_sdk_content_block(block));
    }
    return result;
}

// ============================================================
// Timestamp helper
// ============================================================

/// Produce an ISO-8601 timestamp for the current moment.
inline std::string make_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_val = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_val));
    return std::string(buf);
}

// ============================================================
// Individual type converters
// ============================================================

/// Convert an SdkAssistantMessage to an internal message.
inline InternalMessage sdk_assistant_to_internal(const SdkAssistantMessage& msg) {
    InternalMessage out;
    out.role = "assistant";
    out.type = "assistant";
    out.uuid = msg.uuid;
    out.timestamp = make_timestamp();
    out.content = convert_sdk_content_blocks(msg.content);
    if (msg.error) {
        out.subtype = *msg.error;
    }
    return out;
}

/// Convert an SdkUserMessage to an internal message.
inline InternalMessage sdk_user_to_internal(const SdkUserMessage& msg) {
    InternalMessage out;
    out.role = "user";
    out.type = "user";
    out.uuid = msg.uuid;
    out.timestamp = msg.timestamp.value_or(make_timestamp());
    out.content = convert_sdk_content_blocks(msg.content);
    out.is_virtual = msg.is_synthetic;
    return out;
}

/// Convert an SdkSystemMessage to an internal message.
inline InternalMessage sdk_system_to_internal(const SdkSystemMessage& msg) {
    InternalMessage out;
    out.role = "system";
    out.type = "system";
    out.uuid = msg.uuid;
    out.subtype = msg.subtype;
    out.timestamp = make_timestamp();

    if (msg.subtype == "init" && msg.model) {
        out.content.push_back(
            InternalTextBlock{.text = "Remote session initialized (model: " + *msg.model + ")"}
        );
    } else if (msg.subtype == "status") {
        std::string text = (msg.status && !msg.status->empty())
            ? (*msg.status == "compacting"
                ? std::string("Compacting conversation...")
                : "Status: " + *msg.status)
            : msg.message;
        out.content.push_back(InternalTextBlock{.text = std::move(text)});
    } else {
        out.content.push_back(InternalTextBlock{.text = msg.message});
    }
    return out;
}

/// Convert an SdkResultMessage to an internal message.
inline InternalMessage sdk_result_to_internal(const SdkResultMessage& msg) {
    InternalMessage out;
    out.role = "system";
    out.type = "result";
    out.uuid = msg.uuid;
    out.subtype = msg.subtype;
    out.timestamp = make_timestamp();

    if (msg.is_error) {
        std::string text;
        for (size_t i = 0; i < msg.errors.size(); ++i) {
            if (i > 0) text += ", ";
            text += msg.errors[i];
        }
        if (text.empty()) {
            text = "Unknown error";
        }
        out.content.push_back(InternalTextBlock{.text = std::move(text)});
    } else {
        out.content.push_back(
            InternalTextBlock{.text = "Session completed successfully"}
        );
    }
    return out;
}

/// Convert an SdkToolProgressMessage to an internal message.
inline InternalMessage sdk_tool_progress_to_internal(const SdkToolProgressMessage& msg) {
    InternalMessage out;
    out.role = "system";
    out.type = "system";
    out.subtype = "informational";
    out.uuid = msg.uuid;
    out.timestamp = make_timestamp();
    out.content.push_back(
        InternalTextBlock{.text = "Tool " + msg.tool_name + " running for "
                                + std::to_string(msg.elapsed_time_seconds) + "s..."}
    );
    return out;
}

/// Convert an SdkCompactBoundaryMessage to an internal message.
inline InternalMessage sdk_compact_boundary_to_internal(const SdkCompactBoundaryMessage& msg) {
    InternalMessage out;
    out.role = "system";
    out.type = "system";
    out.subtype = "compact_boundary";
    out.uuid = msg.uuid;
    out.timestamp = make_timestamp();
    out.content.push_back(
        InternalTextBlock{.text = "Conversation compacted"}
    );
    return out;
}

// ============================================================
// Top-level dispatcher
// ============================================================

/// Convert any SdkMessage to an internal ConvertedMessage.
/// Returns {Ignored} for message types that should not be displayed.
inline ConvertedMessage sdk_to_internal(
    const SdkMessage& msg,
    const ConvertOptions& opts = {}
) {
    ConvertedMessage result;

    if (std::holds_alternative<SdkAssistantMessage>(msg)) {
        const auto& m = std::get<SdkAssistantMessage>(msg);
        result.tag = ConvertedMessage::Tag::Message;
        result.message = sdk_assistant_to_internal(m);
        return result;
    }

    if (std::holds_alternative<SdkUserMessage>(msg)) {
        const auto& m = std::get<SdkUserMessage>(msg);

        // Check for tool_result blocks in content
        bool has_tool_result = false;
        for (const auto& block : m.content) {
            if (block.type == "tool_result") {
                has_tool_result = true;
                break;
            }
        }

        if (opts.convert_tool_results && has_tool_result) {
            result.tag = ConvertedMessage::Tag::Message;
            result.message = sdk_user_to_internal(m);
            return result;
        }

        if (opts.convert_user_text_messages && !has_tool_result) {
            result.tag = ConvertedMessage::Tag::Message;
            result.message = sdk_user_to_internal(m);
            return result;
        }

        // User-typed messages are already added locally by the REPL.
        // In CCR mode all user messages are ignored.
        result.tag = ConvertedMessage::Tag::Ignored;
        return result;
    }

    if (std::holds_alternative<SdkSystemMessage>(msg)) {
        const auto& m = std::get<SdkSystemMessage>(msg);

        if (m.subtype == "init" || m.subtype == "status" || m.subtype == "compact_boundary") {
            result.tag = ConvertedMessage::Tag::Message;
            result.message = sdk_system_to_internal(m);
            return result;
        }

        // hook_response and other subtypes are ignored
        result.tag = ConvertedMessage::Tag::Ignored;
        return result;
    }

    if (std::holds_alternative<SdkResultMessage>(msg)) {
        const auto& m = std::get<SdkResultMessage>(msg);

        // Only show result messages for errors. Success results are noise
        // in multi-turn sessions (isLoading=false is sufficient signal).
        if (m.subtype != "success") {
            result.tag = ConvertedMessage::Tag::Message;
            result.message = sdk_result_to_internal(m);
            return result;
        }

        result.tag = ConvertedMessage::Tag::Ignored;
        return result;
    }

    if (std::holds_alternative<SdkStreamEventMessage>(msg)) {
        const auto& m = std::get<SdkStreamEventMessage>(msg);
        result.tag = ConvertedMessage::Tag::StreamEvent;
        result.stream_event = StreamEvent{.event_json = m.event_json};
        return result;
    }

    if (std::holds_alternative<SdkToolProgressMessage>(msg)) {
        const auto& m = std::get<SdkToolProgressMessage>(msg);
        result.tag = ConvertedMessage::Tag::Message;
        result.message = sdk_tool_progress_to_internal(m);
        return result;
    }

    if (std::holds_alternative<SdkCompactBoundaryMessage>(msg)) {
        const auto& m = std::get<SdkCompactBoundaryMessage>(msg);
        result.tag = ConvertedMessage::Tag::Message;
        result.message = sdk_compact_boundary_to_internal(m);
        return result;
    }

    // Unknown variant — gracefully ignore
    result.tag = ConvertedMessage::Tag::Ignored;
    return result;
}

// ============================================================
// Utility predicates
// ============================================================

/// Check if an SDK message indicates the session has ended.
inline bool is_session_end_message(const SdkMessage& msg) {
    return std::holds_alternative<SdkResultMessage>(msg);
}

/// Check if a result message indicates success.
inline bool is_success_result(const SdkResultMessage& msg) {
    return msg.subtype == "success";
}

/// Extract the result text from a successful result message.
inline std::optional<std::string> get_result_text(const SdkResultMessage& msg) {
    if (msg.subtype == "success") {
        return msg.result;
    }
    return std::nullopt;
}

/// Check if a user message contains tool_result blocks.
inline bool has_tool_result_blocks(const SdkUserMessage& msg) {
    for (const auto& block : msg.content) {
        if (block.type == "tool_result") return true;
    }
    return false;
}

/// Parse a raw JSON string into an SdkMessage.
/// Performs minimal structural parsing to determine the message type
/// and populate the corresponding variant. Returns std::nullopt if
/// the JSON cannot be parsed or the type is unrecognized.
///
/// This is a lightweight parser that extracts the "type" field to
/// dispatch to the correct variant. Full JSON parsing would use
/// a proper JSON library (nlohmann/json or similar) in production.
inline std::optional<SdkMessage> parse_sdk_message(const std::string& json) {
    // Extract the "type" field value from JSON
    auto find_type = [](const std::string& s) -> std::optional<std::string> {
        const std::string needle = "\"type\"";
        auto pos = s.find(needle);
        if (pos == std::string::npos) return std::nullopt;

        // Find the colon after "type"
        auto colon = s.find(':', pos + needle.size());
        if (colon == std::string::npos) return std::nullopt;

        // Find the opening quote of the value
        auto open_q = s.find('"', colon + 1);
        if (open_q == std::string::npos) return std::nullopt;

        // Find the closing quote
        auto close_q = s.find('"', open_q + 1);
        if (close_q == std::string::npos) return std::nullopt;

        return s.substr(open_q + 1, close_q - open_q - 1);
    };

    auto extract_string = [](const std::string& s, const std::string& key) -> std::optional<std::string> {
        const std::string needle = "\"" + key + "\"";
        auto pos = s.find(needle);
        if (pos == std::string::npos) return std::nullopt;
        auto colon = s.find(':', pos + needle.size());
        if (colon == std::string::npos) return std::nullopt;
        auto open_q = s.find('"', colon + 1);
        if (open_q == std::string::npos) return std::nullopt;
        auto close_q = s.find('"', open_q + 1);
        if (close_q == std::string::npos) return std::nullopt;
        return s.substr(open_q + 1, close_q - open_q - 1);
    };

    auto maybe_type = find_type(json);
    if (!maybe_type) return std::nullopt;
    const auto& type_val = *maybe_type;

    // Extract session_id and uuid (common fields)
    auto session_id = extract_string(json, "session_id").value_or("");
    auto uuid = extract_string(json, "uuid").value_or("");

    if (type_val == "assistant") {
        SdkAssistantMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.parent_tool_use_id = extract_string(json, "parent_tool_use_id").value_or("null");
        m.error = extract_string(json, "error");
        return SdkMessage{std::move(m)};
    }

    if (type_val == "user") {
        SdkUserMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.parent_tool_use_id = extract_string(json, "parent_tool_use_id").value_or("null");
        m.is_synthetic = json.find("\"isSynthetic\":true") != std::string::npos;
        m.timestamp = extract_string(json, "timestamp");
        return SdkMessage{std::move(m)};
    }

    if (type_val == "system") {
        SdkSystemMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.subtype = extract_string(json, "subtype").value_or("");
        m.message = extract_string(json, "message").value_or("");
        m.model = extract_string(json, "model");
        m.status = extract_string(json, "status");
        return SdkMessage{std::move(m)};
    }

    if (type_val == "result") {
        SdkResultMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.subtype = extract_string(json, "subtype").value_or("");
        m.is_error = (m.subtype != "success");
        m.result = extract_string(json, "result").value_or("");
        return SdkMessage{std::move(m)};
    }

    if (type_val == "stream_event") {
        SdkStreamEventMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.parent_tool_use_id = extract_string(json, "parent_tool_use_id").value_or("null");
        m.event_json = json;
        return SdkMessage{std::move(m)};
    }

    if (type_val == "tool_progress") {
        SdkToolProgressMessage m;
        m.session_id = session_id;
        m.uuid = uuid;
        m.tool_use_id = extract_string(json, "tool_use_id").value_or("");
        m.tool_name = extract_string(json, "tool_name").value_or("");
        m.parent_tool_use_id = extract_string(json, "parent_tool_use_id").value_or("null");
        return SdkMessage{std::move(m)};
    }

    // Unknown type — return nullopt
    return std::nullopt;
}

} // namespace cc::remote
