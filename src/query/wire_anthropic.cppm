/// @file wire_anthropic.cppm
/// @brief Anthropic /v1/messages wire backend: the first implementation of
///        the cc::query::wire::WireBackend seam.
///
/// This is a pure extraction of the serialization/parsing QueryEngine used to
/// do inline (build_request_body / append_message_to_json / content_to_json /
/// parse_api_response / parse_content_block / add_beta_headers /
/// api_messages_endpoint and the SSE handling in stream_single_api_call).
/// Field values, field order, defaults and edge cases are byte-identical to the
/// engine's; nothing was "improved" on the way out, so existing request dumps
/// and golden tests stay valid.
///
/// What the backend intentionally does NOT own (the engine still does):
///   * which host to hit and which API version to claim -- constructor
///     parameters here, since RequestInput does not carry a base URL;
///   * credentials -- the engine computes them and passes them verbatim in
///     `extra_headers` (see anthropic_credential_header);
///   * which tools are sent at all (deny-rule filtering, enabled-tool
///     filtering, dynamic/MCP merging and dedup happen before this point), and
///     which MCP tools have a verbatim schema (RequestInput::tool_schemas);
///   * the context-management edits, task budget and response schema, which
///     come from env-dependent / mutable engine state. They ride in
///     AnthropicWireOptions because RequestInput has no field for them.
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module cc.query.wire_anthropic;

import cc.types.types;
import cc.tools.tool;
import cc.utils.json;
import cc.query.wire_protocol;

export namespace cc::query::wire {

using cc::core::AssistantMessage;
using cc::core::ContentBlock;
using cc::core::DocumentBlock;
using cc::core::ImageBlock;
using cc::core::Message;
using cc::core::SystemMessage;
using cc::core::TextBlock;
using cc::core::ThinkingBlock;
using cc::core::TokenUsage;
using cc::core::ToolDefinition;
using cc::core::ToolResultMessage;
using cc::core::ToolUseBlock;
using cc::core::UserMessage;

// =========================================================================
// Request-side helper types the engine used to hold in QueryOptions
// =========================================================================

/// One API-side context edit (context_management.edits[]).
///
/// Structurally identical to cc::services::compact::ContextEditStrategy; it is
/// spelled out here so this module does not have to import the compaction
/// service. The engine computes the edits (they depend on env / thinking mode)
/// and hands them over.
struct ContextEdit {
    std::string type;
    std::optional<std::uint32_t> trigger_input_tokens;
    std::optional<std::uint32_t> clear_at_least_input_tokens;
    bool has_thinking_keep{false};
    bool keep_all_thinking{false};
    std::optional<std::uint32_t> keep_thinking_turns;
    std::optional<std::uint32_t> keep_tool_uses;
    std::vector<std::string> clear_tool_inputs;
    std::vector<std::string> exclude_tools;
};

/// API-side output_config.task_budget (the task-budgets beta).
/// Mirrors QueryOptions::TaskBudget (and cc::services::api::TaskBudget).
struct TaskBudget {
    std::uint32_t total{0};
    std::optional<std::uint32_t> remaining;
};

/// API-side output_config.format.json_schema (structured outputs).
/// Mirrors QueryOptions::ResponseSchema.
struct ResponseSchema {
    std::string name;        ///< json_schema.name
    std::string schema_json; ///< JSON schema payload, as a string
};

/// Everything the engine must hand over that RequestInput does not carry.
///
/// NOTE: the project builds with -Wmissing-designated-field-initializers, so a
/// caller that wants only some of these should assign fields one by one rather
/// than use a partial designated initializer.
struct AnthropicWireOptions {
    std::string base_url{"https://api.anthropic.com"};
    std::string api_version{"2023-06-01"};
    /// Header pairs emitted verbatim, right after `anthropic-version`. The
    /// engine passes its credential header (Authorization / x-api-key) and
    /// User-Agent here.
    std::vector<std::pair<std::string, std::string>> extra_headers;
    /// context_management.edits[]; a non-empty list also turns on the
    /// context-management beta header. The engine only ever produced a config
    /// with at least one edit, so "non-empty" == "engine had a value".
    std::vector<ContextEdit> context_edits;
    /// output_config.task_budget; set also turns on the task-budgets beta.
    std::optional<TaskBudget> task_budget;
    /// output_config.format.json_schema.
    std::optional<ResponseSchema> response_schema;
};

// =========================================================================
// Endpoint resolution
// =========================================================================

struct ApiMessagesEndpoint {
    std::string client_base_url;  ///< scheme + authority only (httplib client host)
    std::string path;             ///< "/v1/messages" (or "<prefix>/messages")
};

/// Split a configured base URL into the part httplib::Client wants and the
/// request path, appending the /v1/messages (or <prefix>/messages) suffix.
/// Rejects URLs with no scheme or an empty host. Moved verbatim from the
/// engine, including the trailing-slash and "/v1" handling.
[[nodiscard]] inline std::expected<ApiMessagesEndpoint, std::string> api_messages_endpoint(
    std::string_view base_url
) {
    const auto scheme_end = base_url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected("API base URL must include http:// or https://");
    }

    const auto authority_start = scheme_end + 3;
    const auto path_start = base_url.find('/', authority_start);
    std::string client_base_url = path_start == std::string_view::npos
        ? std::string(base_url)
        : std::string(base_url.substr(0, path_start));
    std::string path_prefix = path_start == std::string_view::npos
        ? std::string{}
        : std::string(base_url.substr(path_start));

    while (client_base_url.size() > scheme_end + 3 && client_base_url.ends_with('/')) {
        client_base_url.pop_back();
    }
    while (!path_prefix.empty() && path_prefix.ends_with('/')) {
        path_prefix.pop_back();
    }
    if (path_prefix == "/") path_prefix.clear();

    if (client_base_url.size() <= scheme_end + 3) {
        return std::unexpected("API base URL host cannot be empty");
    }

    std::string path = path_prefix;
    path += path.ends_with("/v1") ? "/messages" : "/v1/messages";
    return ApiMessagesEndpoint{
        .client_base_url = std::move(client_base_url),
        .path = std::move(path),
    };
}

/// Credential header exactly as the engine computes it: a non-empty bearer
/// token wins and is sent as `Authorization: Bearer <token>`, otherwise the
/// plain key goes out as `x-api-key`. Callers put the result into
/// AnthropicWireOptions::extra_headers.
[[nodiscard]] inline std::pair<std::string, std::string> anthropic_credential_header(
    std::string_view api_key,
    std::string_view auth_token
) {
    if (!auth_token.empty()) {
        return {"Authorization", std::string("Bearer ") + std::string(auth_token)};
    }
    return {"x-api-key", std::string(api_key)};
}

// =========================================================================
// The backend
// =========================================================================

/// Anthropic /v1/messages backend: POST {base}/v1/messages, `event:`-typed SSE
/// stream on the way back. Stateless with respect to a request; the only state
/// it holds is deployment configuration (URL, version, static headers) and the
/// optional output_config / context_management inputs.
class AnthropicWireBackend : public WireBackend {
public:
    /// Convenience form for the common case: just the deployment coordinates
    /// plus the header pairs the caller computes itself.
    explicit AnthropicWireBackend(
        std::string base_url,
        std::string api_version = "2023-06-01",
        std::vector<std::pair<std::string, std::string>> extra_headers = {}
    ) {
        options_.base_url = std::move(base_url);
        options_.api_version = std::move(api_version);
        options_.extra_headers = std::move(extra_headers);
    }

    /// Full configuration (context management / task budget / response schema).
    explicit AnthropicWireBackend(AnthropicWireOptions options)
        : options_(std::move(options)) {}

    [[nodiscard]] WireApi api() const noexcept override {
        return WireApi::Anthropic;
    }

    /// URL + serialized body + headers for one non-streaming request.
    [[nodiscard]] std::expected<PreparedRequest, std::string>
    prepare(const RequestInput& input) const override {
        auto endpoint = api_messages_endpoint(options_.base_url);
        if (!endpoint) {
            return std::unexpected(endpoint.error());
        }

        PreparedRequest out;
        // PreparedRequest::url is absolute: the engine splits it again with
        // api_messages_endpoint() when it wants the httplib client host.
        out.url = endpoint->client_base_url + endpoint->path;
        out.body = build_request_body(input);

        // Header order mirrors the engine's: content type, version, auth,
        // User-Agent (last two from extra_headers), then the beta headers.
        out.headers.emplace_back("Content-Type", "application/json");
        out.headers.emplace_back("anthropic-version", options_.api_version);
        for (const auto& header : options_.extra_headers) {
            out.headers.push_back(header);
        }
        if (options_.task_budget) {
            out.headers.emplace_back("anthropic-beta", "task-budgets-2026-03-13");
        }
        if (!options_.context_edits.empty()) {
            out.headers.emplace_back("anthropic-beta", "context-management-2025-06-27");
        }
        return out;
    }

    /// Parse a complete (non-streaming) /v1/messages response body.
    [[nodiscard]] std::expected<ParsedResponse, std::string>
    parse_response(std::string_view body) const override {
        auto doc_result = cc::utils::json::parse(body);
        if (!doc_result) {
            return std::unexpected("Failed to parse API response");
        }

        auto root = doc_result->root();

        ParsedResponse result;

        // Build assistant message
        result.message.id.value = std::string(root.get("id").as_str());
        result.message.model = std::string(root.get("model").as_str());
        result.message.timestamp = std::chrono::system_clock::now();

        // Parse stop reason
        auto stop_reason = root.get("stop_reason");
        if (stop_reason.valid() && !stop_reason.is_null()) {
            result.message.stop_reason = std::string(stop_reason.as_str());
        }

        // Parse content
        auto content = root.get("content");
        if (content.valid() && content.is_arr()) {
            content.iter([&](cc::utils::json::JsonVal block) {
                parse_content_block(block, result.message.content);
            });
        }

        // Parse usage
        auto usage = root.get("usage");
        if (usage.valid() && usage.is_obj()) {
            result.usage.input_tokens = static_cast<std::uint32_t>(usage.get("input_tokens").as_int());
            result.usage.output_tokens = static_cast<std::uint32_t>(usage.get("output_tokens").as_int());
            auto cache_creation = usage.get("cache_creation_input_tokens");
            if (cache_creation.valid()) {
                result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
            }
            auto cache_read = usage.get("cache_read_input_tokens");
            if (cache_read.valid()) {
                result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
            }
        }

        return result;
    }

    /// Decode one SSE frame into zero or more neutral deltas.
    ///
    /// Mapping notes (all deliberate, to keep the engine's behaviour):
    ///   * message_start carries the message id/model, but StreamDelta has no
    ///     field for them, so only the usage fields travel through here.
    ///   * content_block_start has no dedicated StreamDelta kind: a text /
    ///     thinking block start is reported as an EMPTY TextDelta /
    ///     ThinkingDelta so a consumer that opens a block on its first delta
    ///     still reproduces the engine's (possibly empty) block, and a tool_use
    ///     start as ToolUseStart carrying the id and name.
    ///   * message_delta reports its usage object as a Usage delta and its
    ///     stop_reason as a MessageStop delta; message_stop is the stream's
    ///     actual end (stop_reason was already delivered, as in the engine,
    ///     which keeps it on the result until StreamEnd).
    ///   * unknown event names and unparseable frames yield nothing, exactly
    ///     like the engine's silent fall-through.
    [[nodiscard]] std::vector<StreamDelta> parse_stream_event(
        std::string_view event_name,
        std::string_view data
    ) const override {
        std::vector<StreamDelta> out;
        if (data.empty() || data == "[DONE]") return out;

        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) return out;
        auto root = doc_result->root();

        if (event_name == "message_start") {
            auto msg = root.get("message");
            if (msg.valid()) {
                StreamDelta delta;
                delta.kind = StreamDelta::Kind::Usage;
                auto usage = msg.get("usage");
                if (usage.valid() && usage.is_obj()) {
                    auto input = usage.get("input_tokens");
                    if (input.valid()) {
                        delta.usage.input_tokens = static_cast<std::uint32_t>(input.as_int());
                    }
                    auto cache_creation = usage.get("cache_creation_input_tokens");
                    if (cache_creation.valid()) {
                        delta.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                    }
                    auto cache_read = usage.get("cache_read_input_tokens");
                    if (cache_read.valid()) {
                        delta.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                    }
                }
                out.push_back(std::move(delta));
            }
        } else if (event_name == "content_block_start") {
            auto cb = root.get("content_block");
            // The engine defaults to a TextBlock when the block object is
            // missing or of an unknown type, so the fallback here is a text
            // block start as well.
            std::string_view type = "text";
            StreamDelta delta;
            if (cb.valid()) {
                auto type_val = cb.get("type");
                if (type_val.valid()) type = type_val.as_str();
            }
            if (type == "tool_use") {
                delta.kind = StreamDelta::Kind::ToolUseStart;
                auto id = cb.get("id");
                if (id.valid()) delta.tool_id = std::string(id.as_str());
                auto name = cb.get("name");
                if (name.valid()) delta.tool_name = std::string(name.as_str());
            } else if (type == "thinking") {
                delta.kind = StreamDelta::Kind::ThinkingDelta;
            } else {
                delta.kind = StreamDelta::Kind::TextDelta;
            }
            out.push_back(std::move(delta));
        } else if (event_name == "content_block_delta") {
            auto delta_val = root.get("delta");
            if (!delta_val.valid()) return out;
            auto type = delta_val.get("type");
            if (!type.valid()) return out;

            const auto type_str = type.as_str();
            StreamDelta delta;
            if (type_str == "text_delta") {
                auto text = delta_val.get("text");
                if (!text.valid()) return out;
                delta.kind = StreamDelta::Kind::TextDelta;
                delta.text = std::string(text.as_str());
            } else if (type_str == "input_json_delta") {
                auto pj = delta_val.get("partial_json");
                if (!pj.valid()) return out;
                delta.kind = StreamDelta::Kind::ToolUseInputDelta;
                delta.partial_json = std::string(pj.as_str());
            } else if (type_str == "thinking_delta") {
                auto thinking = delta_val.get("thinking");
                if (!thinking.valid()) return out;
                delta.kind = StreamDelta::Kind::ThinkingDelta;
                delta.text = std::string(thinking.as_str());
            } else {
                // A delta type the engine does not read (e.g. signature_delta).
                delta.kind = StreamDelta::Kind::Ignored;
            }
            out.push_back(std::move(delta));
        } else if (event_name == "content_block_stop") {
            // Finalizing the block (tool input defaults to "{}" when no
            // input_json_delta arrived, thinking/text from the accumulated
            // deltas) is the consumer's job now: it owns the accumulation.
            StreamDelta delta;
            delta.kind = StreamDelta::Kind::BlockStop;
            out.push_back(std::move(delta));
        } else if (event_name == "message_delta") {
            auto delta_val = root.get("delta");
            if (delta_val.valid()) {
                auto sr = delta_val.get("stop_reason");
                if (sr.valid() && !sr.is_null()) {
                    StreamDelta delta;
                    delta.kind = StreamDelta::Kind::MessageStop;
                    delta.stop_reason = std::string(sr.as_str());
                    out.push_back(std::move(delta));
                }
            }
            auto usage = root.get("usage");
            if (usage.valid()) {
                StreamDelta delta;
                delta.kind = StreamDelta::Kind::Usage;
                auto ot = usage.get("output_tokens");
                if (ot.valid()) {
                    delta.usage.output_tokens = static_cast<std::uint32_t>(ot.as_int());
                }
                auto cache_creation = usage.get("cache_creation_input_tokens");
                if (cache_creation.valid()) {
                    delta.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                }
                auto cache_read = usage.get("cache_read_input_tokens");
                if (cache_read.valid()) {
                    delta.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                }
                out.push_back(std::move(delta));
            }
        } else if (event_name == "message_stop") {
            StreamDelta delta;
            delta.kind = StreamDelta::Kind::MessageStop;
            out.push_back(std::move(delta));
        } else if (event_name == "error") {
            auto err = root.get("error");
            if (err.valid()) {
                std::string type_text = "stream_error";
                std::string message_text = "Streaming API error";
                auto type = err.get("type");
                if (type.valid()) type_text = std::string(type.as_str());
                auto msg = err.get("message");
                if (msg.valid()) message_text = std::string(msg.as_str());
                // StreamDelta::Error has a single text field, so it carries the
                // message the engine surfaced (error_message); the error type
                // is only the fallback when the message is empty.
                StreamDelta delta;
                delta.kind = StreamDelta::Kind::Error;
                delta.text = message_text.empty() ? type_text : message_text;
                out.push_back(std::move(delta));
            }
        } else if (event_name == "ping") {
            StreamDelta delta;
            delta.kind = StreamDelta::Kind::Ignored;
            out.push_back(std::move(delta));
        }
        // Anything else: silently ignored, as in the engine.

        return out;
    }

    [[nodiscard]] bool stop_reason_is_tool_use(
        std::string_view stop_reason) const override {
        return stop_reason == "tool_use";
    }

    /// Serialize one request body. Field order and values match the engine's
    /// build_request_body() exactly.
    [[nodiscard]] std::string build_request_body(const RequestInput& input) const {
        cc::utils::json::JsonMutDoc doc;
        auto root = doc.object();

        root.add("model", doc.string(input.model));
        root.add("max_tokens", doc.number(static_cast<int64_t>(input.max_tokens)));
        root.add("stream", doc.boolean(input.stream));

        auto messages_arr = doc.array();
        for (const auto& msg : input.messages) {
            append_message_to_json(msg, messages_arr, doc);
        }

        // System prompt as a top-level field (Anthropic API format); omitted
        // when empty. The engine hoists it here from the SystemMessages as it
        // walks the conversation, so the engine hands it over pre-extracted.
        if (!input.system_prompt.empty()) {
            root.add("system", doc.string(input.system_prompt));
        }

        root.add("messages", messages_arr);

        // Tools, in the order the engine assembled them (deny/enable filtering,
        // dedup and MCP merging already happened there).
        {
            auto tools_arr = doc.array();
            for (const auto& tool : input.tools) {
                auto tool_obj = doc.object();
                // Native Anthropic computer-use tool, identified by the
                // registry's internal name "computer_use" and emitted under
                // the Anthropic wire name "computer" with display geometry
                // and no input_schema. Only when the caller asked for the
                // native shape (RequestInput::native_computer_tool); otherwise
                // it is an ordinary function tool.
                // TS REF: Anthropic computer_20241022 tool spec.
                const bool is_native_computer =
                    input.native_computer_tool && tool.name == "computer_use";
                tool_obj.add("name",
                             doc.string(is_native_computer
                                            ? std::string{"computer"}
                                            : tool.name));
                if (is_native_computer) {
                    tool_obj.add("type", doc.string("computer_20241022"));
                    tool_obj.add("display_width_px",
                        doc.number(input.computer_display_width));
                    tool_obj.add("display_height_px",
                        doc.number(input.computer_display_height));
                    tool_obj.add("display_number",
                        doc.number(input.computer_display_number));
                } else {
                    tool_obj.add("type", doc.string("function"));
                    tool_obj.add("description", doc.string(tool.description));
                    // MCP-merged tools carry an empty simplified schema; the
                    // server's verbatim (possibly nested) JSON schema was
                    // handed over per request. Emit it verbatim whenever the
                    // name matches so the model sees the real parameter shape.
                    // (The engine additionally gated this on the tool's
                    // "mcp:<server>" category; RequestInput carries no
                    // category, so the match is by name alone.)
                    std::string schema_json = tool.input_schema.to_json();
                    for (const auto& [schema_name, verbatim] : input.tool_schemas) {
                        if (schema_name == tool.name) {
                            schema_json = verbatim;
                            break;
                        }
                    }
                    auto schema_doc = cc::utils::json::parse(schema_json);
                    if (schema_doc) {
                        tool_obj.add("input_schema", doc.copy_val(schema_doc->root()));
                    } else {
                        tool_obj.add("input_schema", doc.raw_json(schema_json));
                    }
                }
                tools_arr.append(tool_obj);
            }

            // The engine ran its filtering while assembling this list and only
            // emitted the key when at least one tool survived; an empty list
            // therefore means "no tools", i.e. no "tools" key at all.
            if (!input.tools.empty()) {
                root.add("tools", tools_arr);
            }
        }

        // Extended thinking configuration
        if (input.thinking_enabled) {
            auto thinking_obj = doc.object();
            if (!input.thinking_budget_tokens) {
                // Unset budget == adaptive (model picks).
                thinking_obj.add("type", doc.string("adaptive"));
            } else {
                thinking_obj.add("type", doc.string("enabled"));
                thinking_obj.add("budget_tokens",
                    doc.number(static_cast<int64_t>(*input.thinking_budget_tokens)));
            }
            root.add("thinking", thinking_obj);
        }

        // Optional sampling parameters. Emitted only when set, matching the
        // engine's original behaviour (an unset parameter produced no field).
        if (input.temperature) {
            root.add("temperature", doc.number(*input.temperature));
        }
        if (input.top_p) {
            root.add("top_p", doc.number(*input.top_p));
        }
        if (input.top_k) {
            root.add("top_k", doc.number(static_cast<int64_t>(*input.top_k)));
        }

        if (auto context_management = options_.context_edits; !context_management.empty()) {
            add_context_management_to_json(root, doc, context_management);
        }
        add_output_config_to_json(root, doc);

        doc.set_root(root);
        return doc.to_string();
    }

private:
    /// Append a Message variant to the JSON array. System messages have no wire
    /// representation (their text was hoisted into `system` by the engine).
    void append_message_to_json(const Message& msg,
                                cc::utils::json::JsonMutVal& arr,
                                cc::utils::json::JsonMutDoc& doc) const {
        std::visit([&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            auto msg_obj = doc.object();

            if constexpr (std::is_same_v<T, UserMessage>) {
                msg_obj.add("role", doc.string("user"));
                msg_obj.add("content", content_to_json(m.content, doc));
            } else if constexpr (std::is_same_v<T, AssistantMessage>) {
                msg_obj.add("role", doc.string("assistant"));
                msg_obj.add("content", content_to_json(m.content, doc));
            } else if constexpr (std::is_same_v<T, SystemMessage>) {
                // System messages are handled separately
                return;
            } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
                msg_obj.add("role", doc.string("user"));
                auto result_obj = doc.object();
                result_obj.add("type", doc.string("tool_result"));
                result_obj.add("tool_use_id", doc.string(m.tool_use_id.value));
                result_obj.add("is_error", doc.boolean(m.is_error));

                std::string content_str;
                bool has_rich_content = false;
                for (const auto& block : m.content) {
                    if (const auto* text = std::get_if<TextBlock>(&block)) {
                        content_str += text->text;
                    } else if (const auto* image = std::get_if<ImageBlock>(&block)) {
                        (void)image;
                        has_rich_content = true;
                    }
                }
                if (has_rich_content) {
                    auto rich_content = doc.array();
                    if (!content_str.empty()) {
                        auto text_obj = doc.object();
                        text_obj.add("type", doc.string("text"));
                        text_obj.add("text", doc.string(content_str));
                        rich_content.append(text_obj);
                    }
                    for (const auto& block : m.content) {
                        if (const auto* image = std::get_if<ImageBlock>(&block)) {
                            auto image_obj = doc.object();
                            image_obj.add("type", doc.string("image"));
                            auto source = doc.object();
                            source.add("type", doc.string("base64"));
                            source.add("media_type", doc.string(image->media_type));
                            source.add("data", doc.string(image->data));
                            image_obj.add("source", source);
                            rich_content.append(image_obj);
                        }
                    }
                    result_obj.add("content", rich_content);
                } else {
                    result_obj.add("content", doc.string(content_str));
                }
                auto content_arr = doc.array();
                content_arr.append(result_obj);
                for (const auto& block : m.content) {
                    if (const auto* document = std::get_if<DocumentBlock>(&block)) {
                        auto document_obj = doc.object();
                        document_obj.add("type", doc.string("document"));
                        auto source = doc.object();
                        source.add("type", doc.string("base64"));
                        source.add("media_type", doc.string(document->media_type));
                        source.add("data", doc.string(document->data));
                        document_obj.add("source", source);
                        content_arr.append(document_obj);
                    }
                }
                msg_obj.add("content", content_arr);
            }
            // NOTE (preserved from the engine): ToolUseMessage is a
            // conversation-log shape, not an API message shape, and is
            // therefore never serialized here.

            arr.append(msg_obj);
        }, msg);
    }

    /// Convert content blocks to JSON. A lone text block collapses to a plain
    /// string, matching what the API accepts and what the engine sent.
    [[nodiscard]] cc::utils::json::JsonMutVal content_to_json(
        const std::vector<ContentBlock>& content,
        cc::utils::json::JsonMutDoc& doc) const {
        if (content.size() == 1) {
            if (const auto* text = std::get_if<TextBlock>(&content[0])) {
                return doc.string(text->text);
            }
        }

        auto arr = doc.array();
        for (const auto& block : content) {
            std::visit([&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("text"));
                    obj.add("text", doc.string(b.text));
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("tool_use"));
                    obj.add("id", doc.string(b.id.value));
                    obj.add("name", doc.string(b.name));
                    // Input must be a JSON object, not a string
                    auto input_doc = cc::utils::json::parse(b.input_json);
                    if (input_doc) {
                        obj.add("input", doc.copy_val(input_doc->root()));
                    } else {
                        obj.add("input", doc.raw_json(b.input_json));
                    }
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("thinking"));
                    obj.add("thinking", doc.string(b.thinking));
                    obj.add("signature", doc.string(b.signature));
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, ImageBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("image"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(b.media_type));
                    source.add("data", doc.string(b.data));
                    obj.add("source", source);
                    arr.append(obj);
                } else if constexpr (std::is_same_v<T, DocumentBlock>) {
                    auto obj = doc.object();
                    obj.add("type", doc.string("document"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(b.media_type));
                    source.add("data", doc.string(b.data));
                    obj.add("source", source);
                    arr.append(obj);
                }
            }, block);
        }
        return arr;
    }

    /// Parse a single content block from JSON. Types the engine does not read
    /// (tool_result, image, ...) are skipped, as before.
    void parse_content_block(cc::utils::json::JsonVal block,
                            std::vector<ContentBlock>& content) const {
        auto type = block.get("type").as_str();

        if (type == "text") {
            TextBlock tb;
            tb.text = std::string(block.get("text").as_str());
            content.push_back(std::move(tb));
        } else if (type == "tool_use") {
            ToolUseBlock tub;
            tub.id.value = std::string(block.get("id").as_str());
            tub.name = std::string(block.get("name").as_str());
            auto input = block.get("input");
            tub.input_json = input.valid() ? cc::utils::json::to_string(input) : "{}";
            content.push_back(std::move(tub));
        } else if (type == "thinking") {
            ThinkingBlock tb;
            tb.thinking = std::string(block.get("thinking").as_str());
            auto signature = block.get("signature");
            if (signature.valid()) {
                tb.signature = std::string(signature.as_str());
            }
            content.push_back(std::move(tb));
        }
    }

    void add_input_tokens_object(
        cc::utils::json::JsonMutVal& parent,
        cc::utils::json::JsonMutDoc& doc,
        std::string_view key,
        std::uint32_t value
    ) const {
        auto obj = doc.object();
        obj.add("type", doc.string("input_tokens"));
        obj.add("value", doc.number(static_cast<int64_t>(value)));
        parent.add(key, obj);
    }

    void add_string_array(
        cc::utils::json::JsonMutVal& parent,
        cc::utils::json::JsonMutDoc& doc,
        std::string_view key,
        const std::vector<std::string>& values
    ) const {
        auto arr = doc.array();
        for (const auto& value : values) {
            arr.append(doc.string(value));
        }
        parent.add(key, arr);
    }

    void add_context_management_to_json(
        cc::utils::json::JsonMutVal& root,
        cc::utils::json::JsonMutDoc& doc,
        const std::vector<ContextEdit>& edits_config
    ) const {
        auto context_management = doc.object();
        auto edits = doc.array();

        for (const auto& edit : edits_config) {
            auto obj = doc.object();
            obj.add("type", doc.string(edit.type));

            if (edit.trigger_input_tokens) {
                add_input_tokens_object(obj, doc, "trigger", *edit.trigger_input_tokens);
            }
            if (edit.clear_at_least_input_tokens) {
                add_input_tokens_object(obj, doc, "clear_at_least", *edit.clear_at_least_input_tokens);
            }
            if (edit.has_thinking_keep) {
                if (edit.keep_all_thinking) {
                    obj.add("keep", doc.string("all"));
                } else if (edit.keep_thinking_turns) {
                    auto keep = doc.object();
                    keep.add("type", doc.string("thinking_turns"));
                    keep.add("value", doc.number(static_cast<int64_t>(*edit.keep_thinking_turns)));
                    obj.add("keep", keep);
                }
            } else if (edit.keep_tool_uses) {
                auto keep = doc.object();
                keep.add("type", doc.string("tool_uses"));
                keep.add("value", doc.number(static_cast<int64_t>(*edit.keep_tool_uses)));
                obj.add("keep", keep);
            }
            if (!edit.clear_tool_inputs.empty()) {
                add_string_array(obj, doc, "clear_tool_inputs", edit.clear_tool_inputs);
            }
            if (!edit.exclude_tools.empty()) {
                add_string_array(obj, doc, "exclude_tools", edit.exclude_tools);
            }

            edits.append(obj);
        }

        context_management.add("edits", edits);
        root.add("context_management", context_management);
    }

    void add_output_config_to_json(
        cc::utils::json::JsonMutVal& root,
        cc::utils::json::JsonMutDoc& doc
    ) const {
        const bool has_budget = options_.task_budget.has_value();
        const bool has_schema = options_.response_schema.has_value();
        if (!has_budget && !has_schema) return;

        auto output_config = doc.object();
        if (has_budget) {
            auto task_budget = doc.object();
            task_budget.add("type", doc.string("tokens"));
            task_budget.add("total", doc.number(static_cast<int64_t>(options_.task_budget->total)));
            if (options_.task_budget->remaining) {
                task_budget.add(
                    "remaining",
                    doc.number(static_cast<int64_t>(*options_.task_budget->remaining)));
            }
            output_config.add("task_budget", task_budget);
        }
        if (has_schema) {
            // Structured output: force the model to return JSON conforming to
            // the supplied JSON schema (Anthropic output_config.format.json_schema).
            auto format = doc.object();
            format.add("type", doc.string("json_schema"));
            auto schema_obj = doc.object();
            schema_obj.add("name", doc.string(options_.response_schema->name));
            auto schema_val = doc.raw_json(options_.response_schema->schema_json);
            if (schema_val.raw()) {
                schema_obj.add("schema", schema_val);
            }
            format.add("json_schema", schema_obj);
            output_config.add("format", format);
        }
        root.add("output_config", output_config);
    }

    AnthropicWireOptions options_;
};

} // namespace cc::query::wire
