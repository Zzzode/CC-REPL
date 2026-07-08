// C++23 Message Mappers Module
// Provides format mappers between API/SDK and internal message representations,
// plus system init message construction.
module;

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.message_mappers;

export namespace cc::utils::message_mappers {

// ---------------------------------------------------------------------------
// Common Types
// ---------------------------------------------------------------------------

/// Universally unique identifier type (string-based UUID)
using UUID = std::string;

/// Message type discriminator
enum class MessageType {
    User,
    Assistant,
    System
};

/// System message subtype
enum class SystemSubtype {
    None,
    Init,
    CompactBoundary,
    LocalCommand
};

/// Permission mode for the session
enum class PermissionMode {
    Default,
    Plan,
    AutoApprove
};

/// Source of the API key being used
enum class ApiKeySource {
    Environment,
    Config,
    OAuth,
    Unknown
};

// ---------------------------------------------------------------------------
// Compact Metadata (SDK ↔ Internal conversion)
// ---------------------------------------------------------------------------

/// Preserved segment within a compaction boundary
struct PreservedSegment {
    UUID head_uuid;
    UUID anchor_uuid;
    UUID tail_uuid;
};

/// Internal representation of compact metadata
struct CompactMetadata {
    std::string trigger;
    std::size_t pre_tokens = 0;
    std::optional<PreservedSegment> preserved_segment;
};

/// SDK wire-format representation of compact metadata (snake_case)
struct SDKCompactMetadata {
    std::string trigger;
    std::size_t pre_tokens = 0;
    std::optional<PreservedSegment> preserved_segment;
};

/// Convert internal CompactMetadata to SDK wire format
[[nodiscard]] inline SDKCompactMetadata to_sdk_compact_metadata(
    const CompactMetadata& meta) {
    return {
        .trigger = meta.trigger,
        .pre_tokens = meta.pre_tokens,
        .preserved_segment = meta.preserved_segment
    };
}

/// Convert SDK wire format to internal CompactMetadata
[[nodiscard]] inline CompactMetadata from_sdk_compact_metadata(
    const SDKCompactMetadata& meta) {
    return {
        .trigger = meta.trigger,
        .pre_tokens = meta.pre_tokens,
        .preserved_segment = meta.preserved_segment
    };
}

// ---------------------------------------------------------------------------
// Message Content Blocks
// ---------------------------------------------------------------------------

/// A text content block
struct TextBlock {
    std::string text;
};

/// A tool use content block
struct ToolUseBlock {
    std::string id;
    std::string name;
    std::string input_json;  // JSON-encoded input
};

/// A tool result content item (TS parity: ContentBlockParam for tool_result)
struct ToolResultContentItem {
    std::string type;        ///< "text" or "image"
    std::string text;        ///< text content (for type="text")
    std::string media_type;  ///< e.g. "image/png" (for type="image")
    std::string data;        ///< base64 data (for type="image")
};

/// A tool result content block.
/// TS PARITY: content may be string or array of content items.
struct ToolResultBlock {
    std::string tool_use_id;
    std::variant<std::string, std::vector<ToolResultContentItem>> content;
    bool is_error = false;
};

/// A content block in a message
using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

// ---------------------------------------------------------------------------
// Message Types
// ---------------------------------------------------------------------------

/// An internal user message
struct UserMessage {
    UUID uuid;
    std::vector<ContentBlock> content;
    std::string timestamp;
    bool is_meta = false;
    bool is_visible_in_transcript_only = false;
};

/// An API-level assistant message payload
struct ApiAssistantPayload {
    std::string id;
    std::string model;
    std::vector<ContentBlock> content;
    std::string stop_reason;
};

/// An internal assistant message
struct AssistantMessage {
    UUID uuid;
    ApiAssistantPayload message;
    std::optional<std::string> request_id;
    std::string timestamp;
    std::optional<std::string> error;
};

/// An internal system message
struct SystemMessage {
    UUID uuid;
    std::string content;
    std::string level;  // "info", "warning", "error"
    SystemSubtype subtype = SystemSubtype::None;
    std::optional<CompactMetadata> compact_metadata;
    std::string timestamp;
};

/// Discriminated union of all internal message types
struct Message {
    MessageType type;
    std::variant<UserMessage, AssistantMessage, SystemMessage> data;
};

// ---------------------------------------------------------------------------
// SDK Message Types
// ---------------------------------------------------------------------------

/// SDK user message
struct SDKUserMessage {
    UUID uuid;
    std::vector<ContentBlock> content;
    std::string session_id;
    std::optional<std::string> timestamp;
    bool is_synthetic = false;
};

/// SDK assistant message
struct SDKAssistantMessage {
    UUID uuid;
    ApiAssistantPayload message;
    std::string session_id;
    std::optional<std::string> error;
};

/// SDK system message (for init, compact_boundary, etc.)
struct SDKSystemMessage {
    UUID uuid;
    SystemSubtype subtype;
    std::string session_id;
    std::optional<SDKCompactMetadata> compact_metadata;
};

/// Discriminated union of SDK message types
struct SDKMessage {
    MessageType type;
    std::variant<SDKUserMessage, SDKAssistantMessage, SDKSystemMessage> data;
};

// ---------------------------------------------------------------------------
// Message Conversion: SDK → Internal
// ---------------------------------------------------------------------------

/// Convert a list of SDK messages to internal messages
[[nodiscard]] inline std::vector<Message> to_internal_messages(
    const std::vector<SDKMessage>& sdk_messages) {

    std::vector<Message> result;
    result.reserve(sdk_messages.size());

    for (const auto& sdk_msg : sdk_messages) {
        switch (sdk_msg.type) {
        case MessageType::Assistant: {
            const auto& am = std::get<SDKAssistantMessage>(sdk_msg.data);
            AssistantMessage internal{
                .uuid = am.uuid,
                .message = am.message,
                .request_id = std::nullopt,
                .timestamp = {},  // set to current time in practice
                .error = am.error
            };
            result.push_back({MessageType::Assistant, std::move(internal)});
            break;
        }
        case MessageType::User: {
            const auto& um = std::get<SDKUserMessage>(sdk_msg.data);
            UserMessage internal{
                .uuid = um.uuid,
                .content = um.content,
                .timestamp = um.timestamp.value_or(""),
                .is_meta = um.is_synthetic,
                .is_visible_in_transcript_only = false
            };
            result.push_back({MessageType::User, std::move(internal)});
            break;
        }
        case MessageType::System: {
            const auto& sm = std::get<SDKSystemMessage>(sdk_msg.data);
            if (sm.subtype == SystemSubtype::CompactBoundary && sm.compact_metadata) {
                SystemMessage internal{
                    .uuid = sm.uuid,
                    .content = "Conversation compacted",
                    .level = "info",
                    .subtype = SystemSubtype::CompactBoundary,
                    .compact_metadata = from_sdk_compact_metadata(*sm.compact_metadata),
                    .timestamp = {}
                };
                result.push_back({MessageType::System, std::move(internal)});
            }
            break;
        }
        }
    }
    return result;
}

/// Convert internal messages to SDK messages
[[nodiscard]] inline std::vector<SDKMessage> to_sdk_messages(
    const std::vector<Message>& messages,
    std::string_view session_id) {

    std::vector<SDKMessage> result;
    result.reserve(messages.size());
    std::string sid{session_id};

    for (const auto& msg : messages) {
        switch (msg.type) {
        case MessageType::Assistant: {
            const auto& am = std::get<AssistantMessage>(msg.data);
            SDKAssistantMessage sdk{
                .uuid = am.uuid,
                .message = am.message,
                .session_id = sid,
                .error = am.error
            };
            result.push_back({MessageType::Assistant, std::move(sdk)});
            break;
        }
        case MessageType::User: {
            const auto& um = std::get<UserMessage>(msg.data);
            SDKUserMessage sdk{
                .uuid = um.uuid,
                .content = um.content,
                .session_id = sid,
                .timestamp = um.timestamp,
                .is_synthetic = um.is_meta || um.is_visible_in_transcript_only
            };
            result.push_back({MessageType::User, std::move(sdk)});
            break;
        }
        case MessageType::System: {
            const auto& sm = std::get<SystemMessage>(msg.data);
            if (sm.subtype == SystemSubtype::CompactBoundary && sm.compact_metadata) {
                SDKSystemMessage sdk{
                    .uuid = sm.uuid,
                    .subtype = SystemSubtype::CompactBoundary,
                    .session_id = sid,
                    .compact_metadata = to_sdk_compact_metadata(*sm.compact_metadata)
                };
                result.push_back({MessageType::System, std::move(sdk)});
            }
            break;
        }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Rate Limit Info Mapping
// ---------------------------------------------------------------------------

/// Rate limit status
enum class RateLimitStatus {
    Ok,
    Warning,
    Limited
};

/// Internal rate limit information
struct RateLimitInfo {
    RateLimitStatus status = RateLimitStatus::Ok;
    std::optional<std::string> resets_at;
    std::optional<std::string> rate_limit_type;
    std::optional<double> utilization;
    std::optional<std::string> overage_status;
    std::optional<std::string> overage_resets_at;
    std::optional<std::string> overage_disabled_reason;
    std::optional<bool> is_using_overage;
    std::optional<double> surpassed_threshold;
};

/// SDK-facing rate limit info (stripped of internal-only fields)
struct SDKRateLimitInfo {
    RateLimitStatus status = RateLimitStatus::Ok;
    std::optional<std::string> resets_at;
    std::optional<std::string> rate_limit_type;
    std::optional<double> utilization;
    std::optional<std::string> overage_status;
    std::optional<std::string> overage_resets_at;
    std::optional<std::string> overage_disabled_reason;
    std::optional<bool> is_using_overage;
    std::optional<double> surpassed_threshold;
};

/// Map internal rate limit info to SDK-facing type
[[nodiscard]] inline std::optional<SDKRateLimitInfo> to_sdk_rate_limit_info(
    const std::optional<RateLimitInfo>& limits) {
    if (!limits) return std::nullopt;
    return SDKRateLimitInfo{
        .status = limits->status,
        .resets_at = limits->resets_at,
        .rate_limit_type = limits->rate_limit_type,
        .utilization = limits->utilization,
        .overage_status = limits->overage_status,
        .overage_resets_at = limits->overage_resets_at,
        .overage_disabled_reason = limits->overage_disabled_reason,
        .is_using_overage = limits->is_using_overage,
        .surpassed_threshold = limits->surpassed_threshold
    };
}

// ---------------------------------------------------------------------------
// System Init Message
// ---------------------------------------------------------------------------

/// Tool name compatibility mapping for SDK consumers
[[nodiscard]] inline std::string sdk_compat_tool_name(
    std::string_view name,
    std::string_view agent_tool_name,
    std::string_view legacy_agent_tool_name) {
    if (name == agent_tool_name) return std::string{legacy_agent_tool_name};
    return std::string{name};
}

/// MCP server status information
struct McpServerInfo {
    std::string name;
    std::string status;
};

/// Plugin information
struct PluginInfo {
    std::string name;
    std::string path;
    std::string source;
};

/// Inputs required to build a system/init message
struct SystemInitInputs {
    std::vector<std::string> tool_names;
    std::vector<McpServerInfo> mcp_clients;
    std::string model;
    PermissionMode permission_mode;
    std::vector<std::string> slash_commands;
    std::vector<std::string> agent_types;
    std::vector<std::string> skill_names;
    std::vector<PluginInfo> plugins;
    std::optional<bool> fast_mode;
};

/// The system/init message payload for SDK streams
struct SystemInitPayload {
    std::string cwd;
    std::string session_id;
    std::vector<std::string> tools;
    std::vector<McpServerInfo> mcp_servers;
    std::string model;
    PermissionMode permission_mode;
    std::vector<std::string> slash_commands;
    ApiKeySource api_key_source = ApiKeySource::Unknown;
    std::vector<std::string> betas;
    std::string version;
    std::string output_style;
    std::vector<std::string> agents;
    std::vector<std::string> skills;
    std::vector<PluginInfo> plugins;
    UUID uuid;
    std::optional<bool> fast_mode;
};

/// Build the system/init SDK message carrying session metadata.
/// Called at the start of each query turn (or on bridge connect for REPL RC).
[[nodiscard]] inline SystemInitPayload build_system_init_payload(
    const SystemInitInputs& inputs,
    std::string_view cwd,
    std::string_view session_id,
    std::string_view version,
    std::string_view output_style,
    ApiKeySource api_key_source,
    const std::vector<std::string>& betas,
    const UUID& uuid) {

    return SystemInitPayload{
        .cwd = std::string{cwd},
        .session_id = std::string{session_id},
        .tools = inputs.tool_names,
        .mcp_servers = inputs.mcp_clients,
        .model = inputs.model,
        .permission_mode = inputs.permission_mode,
        .slash_commands = inputs.slash_commands,
        .api_key_source = api_key_source,
        .betas = betas,
        .version = std::string{version},
        .output_style = std::string{output_style},
        .agents = inputs.agent_types,
        .skills = inputs.skill_names,
        .plugins = inputs.plugins,
        .uuid = uuid,
        .fast_mode = inputs.fast_mode
    };
}

// ---------------------------------------------------------------------------
// Local Command Output Conversion
// ---------------------------------------------------------------------------

/// Strip ANSI escape codes from a string
[[nodiscard]] inline std::string strip_ansi(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    bool in_escape = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\033' && i + 1 < input.size() && input[i + 1] == '[') {
            in_escape = true;
            ++i;  // skip '['
            continue;
        }
        if (in_escape) {
            if ((input[i] >= 'A' && input[i] <= 'Z') ||
                (input[i] >= 'a' && input[i] <= 'z')) {
                in_escape = false;
            }
            continue;
        }
        result.push_back(input[i]);
    }
    return result;
}

/// Convert local command output to an SDK assistant message.
/// Strips ANSI codes and unwraps XML wrapper tags.
[[nodiscard]] inline SDKAssistantMessage local_command_output_to_sdk_assistant(
    std::string_view raw_content,
    const UUID& uuid,
    std::string_view session_id) {

    std::string clean = strip_ansi(raw_content);

    // Strip <local-command-stdout>...</local-command-stdout> wrappers
    auto strip_tag = [](std::string& s, std::string_view open, std::string_view close) {
        auto pos = s.find(open);
        if (pos != std::string::npos) {
            auto end_pos = s.find(close, pos + open.size());
            if (end_pos != std::string::npos) {
                std::string inner = s.substr(pos + open.size(),
                                             end_pos - pos - open.size());
                s = s.substr(0, pos) + inner + s.substr(end_pos + close.size());
            }
        }
    };
    strip_tag(clean, "<local-command-stdout>", "</local-command-stdout>");
    strip_tag(clean, "<local-command-stderr>", "</local-command-stderr>");

    // Trim whitespace
    auto start = clean.find_first_not_of(" \t\n\r");
    auto end = clean.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        clean = clean.substr(start, end - start + 1);
    }

    ApiAssistantPayload payload{
        .id = "synthetic",
        .model = "synthetic",
        .content = {TextBlock{.text = std::move(clean)}},
        .stop_reason = "end_turn"
    };

    return SDKAssistantMessage{
        .uuid = uuid,
        .message = std::move(payload),
        .session_id = std::string{session_id},
        .error = std::nullopt
    };
}

} // namespace cc::utils::message_mappers
