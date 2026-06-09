// Tools root utilities: message tagging and tool-use ID extraction.
// Mirrors src/tools/utils.ts
module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module cc.tools.utils;

import cc.types.message;

export namespace cc::tools {

/// Tags user messages with a sourceToolUseID so they stay transient until the
/// tool resolves. Prevents the "is running" message from being duplicated in the UI.
template <typename MessageT>
inline auto tag_messages_with_tool_use_id(
    std::vector<MessageT> messages,
    std::optional<std::string> tool_use_id
) -> std::vector<MessageT> {
    if (!tool_use_id) return messages;
    for (auto& m : messages) {
        if (m.type == MessageType::User) {
            m.source_tool_use_id = tool_use_id;
        }
    }
    return messages;
}

/// Extracts the tool use ID from a parent assistant message for a given tool name.
[[nodiscard]] inline auto get_tool_use_id_from_parent_message(
    const AssistantMessage& parent_message,
    std::string_view tool_name
) -> std::optional<std::string> {
    for (const auto& block : parent_message.message.content) {
        if (block.type == ContentBlockType::ToolUse && block.name == tool_name) {
            return block.id;
        }
    }
    return std::nullopt;
}

} // namespace cc::tools
