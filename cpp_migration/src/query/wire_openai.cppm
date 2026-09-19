/// @file wire_openai.cppm
/// @brief OpenAI-compatible wire backend: POST {base}/v1/chat/completions.
///
/// Implements the `cc::query::wire::WireBackend` seam so the engine's agent
/// loop can drive any OpenAI-compatible endpoint (llama.cpp server, vLLM,
/// Ollama, OpenRouter, LM Studio, ...) with the same tool execution,
/// permission and UI paths it uses for Anthropic.
///
/// This is a NEW implementation (nothing to port from TS): the project grew
/// on the Anthropic /v1/messages shape, and the OpenAI chat-completions shape
/// differs in ways that are lossy in both directions. Every such loss is
/// called out inline with a "LOSSY:" marker and summarized here:
///
///   * system prompt   — OpenAI has no top-level `system`; it becomes the
///                       FIRST element of the flat `messages` array.
///   * tool results    — Anthropic nests `tool_result` blocks inside a user
///                       message; OpenAI requires ONE message with
///                       role:"tool" per result. A single engine message may
///                       therefore expand into several wire messages.
///   * tool calls      — Anthropic uses `tool_use` content blocks; OpenAI
///                       uses an assistant-level `tool_calls` array whose
///                       `function.arguments` is a JSON *string*.
///   * thinking        — no standard field exists; ThinkingBlocks are
///                       DROPPED on the way out and never produced on the
///                       way in (the engine still handles thinking when the
///                       Anthropic backend asks for it).
///   * documents       — no `document` content part exists; a DocumentBlock
///                       degrades to a text placeholder that names the media
///                       type but does NOT inline the payload.
///   * computer use    — no native `computer_20241022` tool type; the tool
///                       is emitted as an ordinary function tool. The
///                       capability still works, it is just described by a
///                       function schema instead of a vendor tool type.
///   * cache tokens    — `cache_read_tokens` / `cache_creation_tokens` have
///                       no OpenAI equivalent and stay 0.
///   * tool errors     — the tool role has no `is_error` flag; an error
///                       result is prefixed with "[tool_error] " so the model
///                       can still tell failures from successes.
///
/// The backend is intentionally STATELESS (see the WireBackend doc comment):
/// every method derives its output purely from its arguments. The streaming
/// contract below is designed around that constraint.
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module cc.query.wire_openai;

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
using cc::core::TextBlock;
using cc::core::ThinkingBlock;
using cc::core::TokenUsage;
using cc::core::ToolDefinition;
using cc::core::ToolResultBlock;
using cc::core::ToolResultContentItem;
using cc::core::ToolResultMessage;
using cc::core::ToolUseBlock;
using cc::core::ToolUseMessage;
using cc::core::UserMessage;

namespace detail {

namespace json = cc::utils::json;

/// Placeholder text emitted for a DocumentBlock: the OpenAI chat-completions
/// format has no `document` content part (only `text` and `image_url`), so
/// the block degrades to a note. The base64 payload is deliberately NOT
/// inlined — it would be charged as prompt tokens with no way for the model
/// to interpret it.
[[nodiscard]] inline std::string document_placeholder(const DocumentBlock& doc) {
    return std::format(
        "[document: media_type={}, {} base64 chars omitted — the "
        "OpenAI-compatible wire format has no document content part]",
        doc.media_type, doc.data.size());
}

/// Placeholder text emitted for an image nested inside a tool result: the
/// OpenAI tool role carries a plain string, so a data URL would have to be
/// concatenated into that string. We emit a note instead of a multi-megabyte
/// base64 blob, again to avoid paying tokens for bytes the model cannot read
/// in this position. Images in *user* messages are unaffected — those become
/// proper `image_url` parts with a data URL.
[[nodiscard]] inline std::string tool_image_placeholder(
    const ToolResultContentItem& item) {
    return std::format(
        "[image: media_type={}, {} base64 chars omitted — images are not "
        "representable inside an OpenAI tool-role message]",
        item.media_type, item.data.size());
}

/// `data:<media_type>;base64,<payload>` data URL for an image part.
[[nodiscard]] inline std::string image_data_url(std::string_view media_type,
                                                std::string_view data) {
    return std::format("data:{};base64,{}", media_type, data);
}

/// One `{"type":"text","text":...}` content part.
[[nodiscard]] inline json::JsonMutVal text_part(json::JsonMutDoc& doc,
                                                std::string_view text) {
    auto obj = doc.object();
    obj.add("type", doc.string("text"));
    obj.add("text", doc.string(text));
    return obj;
}

/// One `{"type":"image_url","image_url":{"url":...}}` content part.
[[nodiscard]] inline json::JsonMutVal image_part(json::JsonMutDoc& doc,
                                                 std::string_view media_type,
                                                 std::string_view data) {
    auto obj = doc.object();
    obj.add("type", doc.string("image_url"));
    auto url_obj = doc.object();
    url_obj.add("url", doc.string(image_data_url(media_type, data)));
    obj.add("image_url", url_obj);
    return obj;
}

/// Attach a raw JSON document under `key`, falling back to `fallback_json`
/// when the text does not parse. `JsonMutVal::add` must never receive an
/// invalid value, so validity is checked on every path.
inline void add_raw_json(json::JsonMutDoc& doc, json::JsonMutVal& obj,
                         std::string_view key, const std::string& text,
                         std::string_view fallback_json) {
    if (!text.empty()) {
        if (auto parsed = json::parse(text)) {
            obj.add(key, doc.copy_val(parsed->root()));
            return;
        }
        auto raw = doc.raw_json(text);
        if (raw.valid()) {
            obj.add(key, raw);
            return;
        }
    }
    auto fallback = doc.raw_json(fallback_json);
    if (fallback.valid()) obj.add(key, fallback);
}

/// An empty but permissive JSON Schema, used when a tool schema is unusable.
inline constexpr std::string_view kEmptyObjectSchema =
    R"({"type":"object","properties":{}})";

/// Map an OpenAI `finish_reason` onto the engine's stop_reason vocabulary.
/// "tool_calls" -> "tool_use" is what makes the agent loop continue;
/// "length" -> "max_tokens" is what keeps the engine's existing max_tokens
/// escalation branch working.
[[nodiscard]] inline std::string map_finish_reason(std::string_view finish) {
    if (finish == "tool_calls" || finish == "function_call") return "tool_use";
    if (finish == "stop") return "end_turn";
    if (finish == "length") return "max_tokens";
    // Unknown / vendor-specific reasons pass through verbatim so the engine
    // can still log and branch on them.
    return std::string(finish);
}

/// Text for one tool-result content item.
[[nodiscard]] inline std::string tool_result_item_text(
    const ToolResultContentItem& item) {
    if (item.type == "image") return tool_image_placeholder(item);
    return item.text;
}

/// Flatten a ToolResultBlock's content to the single string the OpenAI tool
/// role expects. Mirrors cc::core::tool_result_content_text() but keeps the
/// media type visible in the image placeholder instead of a bare "[Image]".
[[nodiscard]] inline std::string tool_result_text(const ToolResultBlock& trb) {
    if (const auto* plain = std::get_if<std::string>(&trb.content)) {
        return *plain;
    }
    const auto& items = std::get<std::vector<ToolResultContentItem>>(trb.content);
    std::string out;
    for (const auto& item : items) {
        auto piece = tool_result_item_text(item);
        if (piece.empty()) continue;
        if (!out.empty()) out += '\n';
        out += piece;
    }
    return out;
}

/// Flatten a ToolResultMessage's blocks (the engine's tool-result message
/// variant) to the tool role's single string.
[[nodiscard]] inline std::string tool_result_message_text(
    const ToolResultMessage& msg) {
    std::string out;
    for (const auto& block : msg.content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            if (text->text.empty()) continue;
            if (!out.empty()) out += '\n';
            out += text->text;
        } else if (const auto* image = std::get_if<ImageBlock>(&block)) {
            out += std::format(
                "\n[image: media_type={}, {} base64 chars omitted — images are "
                "not representable inside an OpenAI tool-role message]",
                image->media_type, image->data.size());
        } else if (const auto* document = std::get_if<DocumentBlock>(&block)) {
            if (!out.empty()) out += '\n';
            out += document_placeholder(*document);
        }
    }
    return out;
}

/// Build one `{"role":"tool","tool_call_id":...,"content":...}` message from
/// an engine ToolResultBlock.
[[nodiscard]] inline json::JsonMutVal tool_role_message(json::JsonMutDoc& doc,
                                                        const ToolResultBlock& trb) {
    auto msg = doc.object();
    msg.add("role", doc.string("tool"));
    msg.add("tool_call_id", doc.string(trb.tool_use_id.value));
    auto text = tool_result_text(trb);
    // LOSSY: the tool role has no is_error flag; keep the signal in-band so
    // the model can distinguish a failed tool from a successful one.
    if (trb.is_error) text = "[tool_error] " + text;
    msg.add("content", doc.string(text));
    return msg;
}

/// Build one `{"role":"tool","tool_call_id":...,"content":...}` message from
/// an engine ToolResultMessage.
[[nodiscard]] inline json::JsonMutVal tool_role_message(
    json::JsonMutDoc& doc, const ToolResultMessage& msg) {
    auto obj = doc.object();
    obj.add("role", doc.string("tool"));
    obj.add("tool_call_id", doc.string(msg.tool_use_id.value));
    auto text = tool_result_message_text(msg);
    if (msg.is_error) text = "[tool_error] " + text;
    obj.add("content", doc.string(text));
    return obj;
}

/// One `{"id":...,"type":"function","function":{"name":...,"arguments":...}}`
/// entry for an assistant message's tool_calls array. `arguments` is a JSON
/// *string* on the OpenAI wire, not a nested object.
[[nodiscard]] inline json::JsonMutVal tool_call_entry(json::JsonMutDoc& doc,
                                                      std::string_view id,
                                                      std::string_view name,
                                                      std::string_view input_json) {
    auto entry = doc.object();
    entry.add("id", doc.string(id));
    entry.add("type", doc.string("function"));
    auto function = doc.object();
    function.add("name", doc.string(name));
    function.add("arguments", doc.string(input_json.empty() ? "{}" : input_json));
    entry.add("function", function);
    return entry;
}

/// Per-block classification used while serializing one engine message.
struct MessageParts {
    json::JsonMutVal content = {};    ///< array of content parts (may be empty)
    json::JsonMutVal tool_calls = {}; ///< array of tool_calls (may be empty)
    /// Tool results that must leave the current message and become their own
    /// role:"tool" messages, in order.
    std::vector<json::JsonMutVal> tool_messages;
};

/// Serialize the ContentBlocks of one engine message.
///
/// Mapping per block:
///   TextBlock       -> {"type":"text","text":...}
///   ImageBlock      -> {"type":"image_url","image_url":{"url":"data:..."}}
///   DocumentBlock   -> text placeholder (no document part exists)
///   ThinkingBlock   -> DROPPED (no standard OpenAI thinking field)
///   ToolUseBlock    -> `tool_calls` entry when the message is an assistant
///                      turn; a text note otherwise (user turns cannot carry
///                      tool calls on this wire)
///   ToolResultBlock -> its own role:"tool" message (possibly several)
[[nodiscard]] inline MessageParts split_message(
    json::JsonMutDoc& doc, const std::vector<ContentBlock>& content,
    bool assistant_turn) {
    MessageParts parts;
    parts.content = doc.array();
    parts.tool_calls = doc.array();

    for (const auto& block : content) {
        std::visit(
            [&](const auto& b) {
                using T = std::remove_cvref_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    if (b.text.empty()) return;
                    parts.content.append(text_part(doc, b.text));
                } else if constexpr (std::is_same_v<T, ImageBlock>) {
                    parts.content.append(image_part(doc, b.media_type, b.data));
                } else if constexpr (std::is_same_v<T, DocumentBlock>) {
                    parts.content.append(text_part(doc, document_placeholder(b)));
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    if (assistant_turn) {
                        parts.tool_calls.append(
                            tool_call_entry(doc, b.id.value, b.name, b.input_json));
                    } else {
                        // A tool_use inside a user turn is not expressible:
                        // OpenAI only allows tool_calls on assistant messages.
                        parts.content.append(text_part(
                            doc,
                            std::format("[tool_use:{} {}]", b.name, b.input_json)));
                    }
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    // Tool results never stay in the containing message: each
                    // becomes its own role:"tool" message, appended in order
                    // right after this one.
                    parts.tool_messages.push_back(tool_role_message(doc, b));
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    // LOSSY: dropped, not even as text — the thinking text is
                    // not part of the conversation the model should see again,
                    // and no OpenAI-compatible field can round-trip it
                    // (Anthropic needs the signature back; OpenAI has none).
                    return;
                }
            },
            block);
    }
    (void)assistant_turn;
    return parts;
}

/// Append the flat wire messages for one engine Message to `out`.
inline void append_message(json::JsonMutDoc& doc, const Message& msg,
                           json::JsonMutVal& out) {
    std::visit(
        [&](const auto& m) {
            using T = std::remove_cvref_t<decltype(m)>;

            if constexpr (std::is_same_v<T, ToolResultMessage>) {
                // Engine tool-result message -> a single role:"tool" message.
                out.append(tool_role_message(doc, m));
            } else if constexpr (std::is_same_v<T, ToolUseMessage>) {
                // Assistant asked for a tool. The OpenAI wire wants this as an
                // assistant message with a tool_calls array. ToolUseMessage
                // carries no tool_use id of its own, so its (unique) message id
                // doubles as the tool_call id.
                auto msg_obj = doc.object();
                msg_obj.add("role", doc.string("assistant"));
                auto tool_calls = doc.array();
                tool_calls.append(tool_call_entry(
                    doc, m.id.value, m.tool_name, m.tool_input_json));
                msg_obj.add("tool_calls", tool_calls);
                out.append(msg_obj);
            } else if constexpr (std::is_same_v<T, UserMessage>) {
                auto parts = split_message(doc, m.content, /*assistant_turn=*/false);
                // Order matters: the role:"tool" messages go FIRST so they
                // directly follow the assistant message that requested the
                // calls (OpenAI pairs a tool message with the immediately
                // preceding tool_calls; several servers reject a tool message
                // with anything in between). The user message with the turn's
                // remaining parts (text / images / document notes) then
                // follows.
                for (auto& tool_msg : parts.tool_messages) {
                    out.append(tool_msg);
                }
                // A user message whose blocks are ALL tool results has no user
                // part of its own — emitting an empty `content: []` user turn
                // would be both meaningless and rejected by strict servers.
                // This is a common shape in the engine, which nests
                // tool_result blocks inside user messages (Anthropic style).
                if (parts.content.size() > 0) {
                    auto msg_obj = doc.object();
                    msg_obj.add("role", doc.string("user"));
                    // Always the array form: it is the only shape that can
                    // carry images, and it keeps the mapping uniform (tool
                    // results in particular force an array elsewhere).
                    msg_obj.add("content", parts.content);
                    out.append(msg_obj);
                }
            } else if constexpr (std::is_same_v<T, AssistantMessage>) {
                auto parts = split_message(doc, m.content, /*assistant_turn=*/true);
                auto msg_obj = doc.object();
                msg_obj.add("role", doc.string("assistant"));
                if (parts.content.size() > 0) {
                    msg_obj.add("content", parts.content);
                } else if (parts.tool_calls.size() == 0) {
                    // Neither text nor tool calls: emit an empty string rather
                    // than omitting the field, which some strict servers reject.
                    msg_obj.add("content", doc.string(""));
                }
                // With tool calls and no text, `content` is omitted (OpenAI
                // permits an absent/null content there).
                if (parts.tool_calls.size() > 0) {
                    msg_obj.add("tool_calls", parts.tool_calls);
                }
                out.append(msg_obj);
                for (auto& tool_msg : parts.tool_messages) {
                    out.append(tool_msg);
                }
            } else if constexpr (std::is_same_v<T, cc::core::SystemMessage>) {
                // Skipped: the engine already hoists the system prompt into
                // RequestInput::system_prompt, which becomes messages[0].
            }
        },
        msg);
}

} // namespace detail

// =========================================================================
// OpenAiWireBackend
// =========================================================================

/// WireBackend for POST {base}/v1/chat/completions.
///
/// Stateless: no member is mutated by any method, so an instance may be
/// shared between concurrent requests.
class OpenAiWireBackend final : public WireBackend {
public:
    /// @param base_url        server root, e.g. "http://localhost:8080".
    ///                        Must NOT already include "/v1" — the path is
    ///                        appended by prepare(). A trailing '/' is
    ///                        tolerated.
    /// @param extra_headers   caller-supplied headers (typically
    ///                        `Authorization: Bearer <key>`). They are added
    ///                        after the defaults and may override them.
    explicit OpenAiWireBackend(
        std::string base_url,
        std::vector<std::pair<std::string, std::string>> extra_headers = {})
        : base_url_(std::move(base_url)),
          extra_headers_(std::move(extra_headers)) {}

    [[nodiscard]] WireApi api() const noexcept override {
        return WireApi::OpenAi;
    }

    [[nodiscard]] std::expected<PreparedRequest, std::string> prepare(
        const RequestInput& input) const override {
        std::string base = base_url_;
        // Trim trailing slashes so "http://host/" and "http://host" agree.
        const auto last_keep = base.find_last_not_of('/');
        if (last_keep != std::string::npos) {
            base.erase(last_keep + 1);
        } else {
            base.clear();
        }
        if (base.empty()) {
            return std::unexpected(
                "OpenAI wire backend has no base URL configured");
        }

        detail::json::JsonMutDoc doc;
        auto root = doc.object();

        root.add("model", doc.string(input.model));
        root.add("max_tokens",
                 doc.number(static_cast<int64_t>(input.max_tokens)));
        root.add("stream", doc.boolean(input.stream));

        // Optional sampling parameters. top_k has no OpenAI equivalent and is
        // intentionally dropped; most OpenAI-compatible servers reject unknown
        // fields.
        if (input.temperature) {
            root.add("temperature", doc.number(*input.temperature));
        }
        if (input.top_p) {
            root.add("top_p", doc.number(*input.top_p));
        }

        // ---- messages: a FLAT array; no top-level `system` ----
        auto messages = doc.array();
        if (!input.system_prompt.empty()) {
            // OpenAI has no top-level system field: the prompt becomes the
            // first message of the conversation.
            auto system_msg = doc.object();
            system_msg.add("role", doc.string("system"));
            system_msg.add("content", doc.string(input.system_prompt));
            messages.append(system_msg);
        }
        for (const auto& msg : input.messages) {
            detail::append_message(doc, msg, messages);
        }
        root.add("messages", messages);

        // ---- tools: always the ordinary function form ----
        if (!input.tools.empty()) {
            auto tools = doc.array();
            for (const auto& tool : input.tools) {
                // LOSSY: no native computer-use tool type exists here, so
                // `input.native_computer_tool` is intentionally ignored. The
                // computer-use capability still works — the tool is offered
                // as a plain function whose schema describes the actions —
                // it just is not the vendor-specific "computer_20241022"
                // shape the Anthropic backend emits.
                auto tool_obj = doc.object();
                tool_obj.add("type", doc.string("function"));
                auto function = doc.object();
                function.add("name", doc.string(tool.name));
                function.add("description", doc.string(tool.description));

                // Prefer the caller's verbatim schema (e.g. an MCP server's
                // nested schema) when one is registered for this tool name;
                // otherwise fall back to the engine's simplified schema.
                std::string schema_json;
                for (const auto& [name, schema] : input.tool_schemas) {
                    if (name == tool.name) {
                        schema_json = schema;
                        break;
                    }
                }
                if (schema_json.empty()) {
                    schema_json = tool.input_schema.to_json();
                }
                detail::add_raw_json(doc, function, "parameters", schema_json,
                                     detail::kEmptyObjectSchema);

                tool_obj.add("function", function);
                tools.append(tool_obj);
            }
            root.add("tools", tools);
        }

        // LOSSY: extended thinking has no OpenAI-compatible representation.
        // `thinking_enabled` / `thinking_budget_tokens` are deliberately
        // ignored — sending a `thinking` object would be rejected by most
        // endpoints, and there is no field that would make the model emit a
        // ThinkingBlock back. Thinking stays an Anthropic-backend feature.
        //
        // NOTE: endpoints that accept `max_completion_tokens` instead of
        // `max_tokens` (newer OpenAI reasoning models) need a different
        // backend/flag; `max_tokens` is what the wide set of local servers
        // (llama.cpp, vLLM, Ollama, LM Studio) understands.

        doc.set_root(root);

        PreparedRequest prepared;
        prepared.url = base + "/v1/chat/completions";
        prepared.body = doc.to_string();

        prepared.headers.emplace_back("Content-Type", "application/json");
        for (const auto& [name, value] : extra_headers_) {
            // Caller headers win, but never duplicate Content-Type.
            if (name == "Content-Type") continue;
            prepared.headers.emplace_back(name, value);
        }
        return prepared;
    }

    [[nodiscard]] std::expected<ParsedResponse, std::string> parse_response(
        std::string_view body) const override {
        auto parsed = detail::json::parse(body);
        if (!parsed) {
            return std::unexpected(std::format(
                "OpenAI response is not valid JSON: {}",
                parsed.error().format()));
        }
        auto root = parsed->root();
        if (!root.is_obj()) {
            return std::unexpected(
                "OpenAI response is not a JSON object");
        }

        ParsedResponse result;

        // The backend layers `id` doubles as the engine MessageId (there is no
        // separate empty-content block type in the engine types; the id is all
        // the engine needs to correlate the turn).
        result.message.id.value = std::string(root.get("id").as_str());
        const auto model = root.get("model");
        if (model.is_str()) {
            result.message.model = std::string(model.as_str());
        }
        result.message.timestamp = std::chrono::system_clock::now();

        const auto choices = root.get("choices");
        if (!choices.is_arr() || choices.size() == 0) {
            return std::unexpected(
                "OpenAI response contains no choices");
        }
        const auto choice = choices.at(0);

        const auto finish = choice.get("finish_reason");
        if (finish.is_str() && !finish.as_str().empty()) {
            result.message.stop_reason =
                detail::map_finish_reason(finish.as_str());
        }

        const auto message = choice.get("message");
        if (!message.is_obj()) {
            return std::unexpected(
                "OpenAI response choice has no message object");
        }

        // `content` may be a string or (rarely) an array of parts; both are
        // flattened into one TextBlock. A null content with tool calls is
        // normal and yields no text block.
        const auto content = message.get("content");
        if (content.is_str() && !content.as_str().empty()) {
            TextBlock text;
            text.text = std::string(content.as_str());
            result.message.content.push_back(std::move(text));
        } else if (content.is_arr()) {
            std::string joined;
            content.iter([&](detail::json::JsonVal part) {
                if (!part.is_obj()) return;
                if (part.get("type").as_str() != std::string_view("text")) return;
                const auto part_text = part.get("text");
                if (!part_text.is_str()) return;
                joined += std::string(part_text.as_str());
            });
            if (!joined.empty()) {
                TextBlock text;
                text.text = std::move(joined);
                result.message.content.push_back(std::move(text));
            }
        }

        const auto tool_calls = message.get("tool_calls");
        if (tool_calls.is_arr()) {
            tool_calls.iter([&](detail::json::JsonVal call) {
                if (!call.is_obj()) return;
                ToolUseBlock block;
                block.id.value = std::string(call.get("id").as_str());
                const auto function = call.get("function");
                block.name = std::string(function.get("name").as_str());
                // `arguments` is a JSON *string*; the engine wants the raw
                // JSON text. Anything unparseable degrades to "{}" so a
                // malformed call surfaces as a schema/permission error from
                // the tool layer rather than a hard parse failure here.
                const auto arguments = function.get("arguments");
                std::string arguments_json =
                    arguments.is_str() ? std::string(arguments.as_str())
                                       : std::string{};
                if (arguments_json.empty() ||
                    !detail::json::parse(arguments_json)) {
                    arguments_json = "{}";
                }
                block.input_json = std::move(arguments_json);
                result.message.content.push_back(std::move(block));
            });
        }

        const auto usage = root.get("usage");
        if (usage.is_obj()) {
            result.usage.input_tokens = static_cast<std::uint32_t>(
                usage.get("prompt_tokens").as_int());
            result.usage.output_tokens = static_cast<std::uint32_t>(
                usage.get("completion_tokens").as_int());
            // LOSSY: cache_read_tokens / cache_creation_tokens have no OpenAI
            // equivalent and are left at 0.
        }

        return result;
    }

    /// Decode one SSE frame.
    ///
    /// OpenAI streams `data: {json}` frames (event_name is normally empty) and
    /// terminates with `data: [DONE]`.
    ///
    /// STREAMING CONTRACT (the backend is stateless, so StreamDelta carries no
    /// index and no history can be kept):
    ///   * TextDelta        — appended to the current text block. OpenAI emits
    ///                        text before tool calls, so text and tool-call
    ///                        accumulation do not interleave in practice.
    ///   * ToolUseStart     — emitted whenever a delta carries a non-empty
    ///                        `id` or `function.name`. OpenAI sends both only
    ///                        in a tool call's FIRST chunk, so in practice
    ///                        exactly one start is emitted per tool call. If an
    ///                        endpoint re-sends them, the engine sees a
    ///                        repeated start; consumers should treat a start
    ///                        for an already-open call as a no-op.
    ///   * ToolUseInputDelta— emitted for every non-empty `function.arguments`
    ///                        fragment, carrying no id (it appeared only in the
    ///                        start chunk). The engine therefore MUST
    ///                        accumulate in arrival order into the most
    ///                        recently started tool call.
    ///   * BlockStop        — never emitted: the format has no block boundary
    ///                        event. Consumers close open blocks on
    ///                        MessageStop.
    ///   * MessageStop      — emitted for a non-null `finish_reason` and again
    ///                        for `[DONE]`. The [DONE] one carries an empty
    ///                        stop_reason because the backend cannot know
    ///                        whether one was already seen; consumers must
    ///                        treat an empty stop_reason as "unchanged".
    ///
    /// KNOWN LIMITATION: parallel tool calls streamed with interleaved
    /// `index` values cannot be demultiplexed without state. Sequential tool
    /// calls (what llama.cpp / vLLM / Ollama emit) reconstruct correctly; with
    /// interleaved parallel calls the fragments of later calls are appended to
    /// the wrong call. A future stateful variant of the backend would fix this.
    [[nodiscard]] std::vector<StreamDelta> parse_stream_event(
        std::string_view event_name, std::string_view data) const override {
        (void)event_name;  // OpenAI frames carry no event name

        std::vector<StreamDelta> deltas;

        // Trim surrounding whitespace; some servers pad their frames.
        const auto first = data.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            StreamDelta ignored;
            ignored.kind = StreamDelta::Kind::Ignored;
            deltas.push_back(std::move(ignored));
            return deltas;
        }
        const auto last = data.find_last_not_of(" \t\r\n");
        data = data.substr(first, last - first + 1);

        if (data == "[DONE]") {
            StreamDelta stop;
            stop.kind = StreamDelta::Kind::MessageStop;
            // Empty stop_reason: see the contract note above.
            deltas.push_back(std::move(stop));
            return deltas;
        }

        auto parsed = detail::json::parse(data);
        if (!parsed) {
            StreamDelta error;
            error.kind = StreamDelta::Kind::Error;
            error.text = std::format("malformed OpenAI stream frame: {}",
                                     std::string(data));
            deltas.push_back(std::move(error));
            return deltas;
        }
        auto root = parsed->root();

        // Mid-stream failures arrive in-band as {"error":{...}}.
        const auto error_obj = root.get("error");
        if (error_obj.is_obj()) {
            StreamDelta error;
            error.kind = StreamDelta::Kind::Error;
            const auto message = error_obj.get("message");
            error.text = message.is_str()
                             ? std::string(message.as_str())
                             : std::string("OpenAI stream error");
            deltas.push_back(std::move(error));
            return deltas;
        }

        const auto choices = root.get("choices");
        if (choices.is_arr() && choices.size() > 0) {
            const auto choice = choices.at(0);
            const auto delta = choice.get("delta");
            if (delta.is_obj()) {
                const auto content = delta.get("content");
                if (content.is_str() && !content.as_str().empty()) {
                    StreamDelta text;
                    text.kind = StreamDelta::Kind::TextDelta;
                    text.text = std::string(content.as_str());
                    deltas.push_back(std::move(text));
                }

                const auto tool_calls = delta.get("tool_calls");
                if (tool_calls.is_arr()) {
                    tool_calls.iter([&](detail::json::JsonVal call) {
                        if (!call.is_obj()) return;
                        const auto function = call.get("function");
                        const auto id = call.get("id");
                        const auto name = function.get("name");
                        const std::string_view id_sv =
                            id.is_str() ? id.as_str() : std::string_view{};
                        const std::string_view name_sv =
                            name.is_str() ? name.as_str() : std::string_view{};
                        if (!id_sv.empty() || !name_sv.empty()) {
                            StreamDelta start;
                            start.kind = StreamDelta::Kind::ToolUseStart;
                            start.tool_id = std::string(id_sv);
                            start.tool_name = std::string(name_sv);
                            deltas.push_back(std::move(start));
                        }
                        const auto arguments = function.get("arguments");
                        if (arguments.is_str() && !arguments.as_str().empty()) {
                            StreamDelta fragment;
                            fragment.kind =
                                StreamDelta::Kind::ToolUseInputDelta;
                            fragment.partial_json =
                                std::string(arguments.as_str());
                            deltas.push_back(std::move(fragment));
                        }
                    });
                }
            }

            const auto finish = choice.get("finish_reason");
            if (finish.is_str() && !finish.as_str().empty()) {
                StreamDelta stop;
                stop.kind = StreamDelta::Kind::MessageStop;
                stop.stop_reason = detail::map_finish_reason(finish.as_str());
                deltas.push_back(std::move(stop));
            }
        }

        // The final frame (when the endpoint supports
        // `stream_options.include_usage`) has an empty choices array and a
        // usage object.
        const auto usage = root.get("usage");
        if (usage.is_obj()) {
            StreamDelta usage_delta;
            usage_delta.kind = StreamDelta::Kind::Usage;
            usage_delta.usage.input_tokens = static_cast<std::uint32_t>(
                usage.get("prompt_tokens").as_int());
            usage_delta.usage.output_tokens = static_cast<std::uint32_t>(
                usage.get("completion_tokens").as_int());
            deltas.push_back(std::move(usage_delta));
        }

        if (deltas.empty()) {
            StreamDelta ignored;
            ignored.kind = StreamDelta::Kind::Ignored;
            deltas.push_back(std::move(ignored));
        }
        return deltas;
    }

    /// "tool_calls" is OpenAI's "the model wants tools executed"; "tool_use" is
    /// accepted too so a value normalized by the engine keeps working.
    [[nodiscard]] bool stop_reason_is_tool_use(
        std::string_view stop_reason) const override {
        return stop_reason == "tool_use" || stop_reason == "tool_calls";
    }

private:
    std::string base_url_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;
};

} // namespace cc::query::wire
