// Implementation unit for cc.query.query_engine — Message/ContentBlock to
// request-JSON serialization (append_message_to_json / content_to_json).
// cc.utils.json also stays imported by the module interface because the
// surviving declarations of these members name JsonMutVal/JsonMutDoc.
module;

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.utils.json;

namespace cc::core {

void QueryEngine::append_message_to_json(const Message& msg,
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

        arr.append(msg_obj);
    }, msg);
}

[[nodiscard]] cc::utils::json::JsonMutVal QueryEngine::content_to_json(
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

} // namespace cc::core
