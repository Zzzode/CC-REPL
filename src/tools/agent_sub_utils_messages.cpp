// Implementation unit for cc.tools.agent.utils — fork-context parsing, the
// four resume message filters, content-replacement application, sidechain
// append, and message text extraction.
module;

module cc.tools.agent.utils;

import std;

import cc.utils.json;
import cc.services.api.client;
import cc.tools.agent_runtime;

namespace cc::tools::agent::utils {

[[nodiscard]] std::string trim_ascii_copy(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] std::string message_content_text(const Message& message) {
    std::string out;
    for (const auto& block : message.content) {
        if (!out.empty()) out += "\n";
        if (block.type == ContentBlockType::Text || block.type == ContentBlockType::ToolResult) {
            out += block.text;
        } else if (block.type == ContentBlockType::ToolUse) {
            out += std::format("[tool_use:{}]", block.tool_name);
        }
    }
    return out;
}

[[nodiscard]] std::vector<Message> fork_context_messages_from_entries(
    const std::vector<std::string>& entries
) {
    std::vector<Message> messages;
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed) continue;
        if (auto message = message_from_json_value(parsed->root())) {
            messages.push_back(std::move(*message));
        }
    }
    return messages;
}

[[nodiscard]] std::vector<std::string> tool_use_ids_in_message(const Message& message) {
    std::vector<std::string> ids;
    if (message.role != "assistant" && message.role != "user") return ids;
    for (const auto& block : message.content) {
        if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
            ids.push_back(block.tool_use_id);
        }
    }
    return ids;
}

[[nodiscard]] std::vector<std::string> tool_result_ids_in_message(const Message& message) {
    std::vector<std::string> ids;
    if (message.role != "assistant" && message.role != "user") return ids;
    for (const auto& block : message.content) {
        if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
            ids.push_back(block.tool_use_id);
        }
    }
    return ids;
}

[[nodiscard]] std::vector<Message> filter_resume_unresolved_tool_use_messages(
    std::vector<Message> messages
) {
    std::unordered_set<std::string> tool_use_ids;
    std::unordered_set<std::string> tool_result_ids;
    for (const auto& message : messages) {
        for (const auto& id : tool_use_ids_in_message(message)) tool_use_ids.insert(id);
        for (const auto& id : tool_result_ids_in_message(message)) tool_result_ids.insert(id);
    }

    std::unordered_set<std::string> unresolved_ids;
    for (const auto& id : tool_use_ids) {
        if (!tool_result_ids.contains(id)) unresolved_ids.insert(id);
    }
    if (unresolved_ids.empty()) return messages;

    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        if (message.role != "assistant") {
            filtered.push_back(std::move(message));
            continue;
        }
        auto ids = tool_use_ids_in_message(message);
        if (ids.empty()) {
            filtered.push_back(std::move(message));
            continue;
        }
        const bool all_unresolved = std::ranges::all_of(ids, [&](const std::string& id) {
            return unresolved_ids.contains(id);
        });
        if (!all_unresolved) filtered.push_back(std::move(message));
    }
    return filtered;
}

[[nodiscard]] bool message_is_thinking_only(const Message& message) {
    if (message.role != "assistant" || message.content.empty()) return false;
    return std::ranges::all_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Thinking ||
            block.type == ContentBlockType::RedactedThinking;
    });
}

[[nodiscard]] std::vector<Message> filter_resume_orphaned_thinking_messages(
    std::vector<Message> messages
) {
    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        if (message_is_thinking_only(message)) continue;
        filtered.push_back(std::move(message));
    }
    return filtered;
}

[[nodiscard]] bool message_is_whitespace_only_assistant(const Message& message) {
    if (message.role != "assistant" || message.content.empty()) return false;
    return std::ranges::all_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Text && trim_ascii_copy(block.text).empty();
    });
}

void append_merged_user_message(std::vector<Message>& messages, Message message) {
    if (!messages.empty() && messages.back().role == "user" && message.role == "user") {
        messages.back().content.insert(
            messages.back().content.end(),
            std::make_move_iterator(message.content.begin()),
            std::make_move_iterator(message.content.end()));
        return;
    }
    messages.push_back(std::move(message));
}

[[nodiscard]] std::vector<Message> filter_resume_whitespace_assistant_messages(
    std::vector<Message> messages
) {
    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    bool changed = false;
    for (auto& message : messages) {
        if (message_is_whitespace_only_assistant(message)) {
            changed = true;
            continue;
        }
        if (changed) {
            append_merged_user_message(filtered, std::move(message));
        } else {
            filtered.push_back(std::move(message));
        }
    }
    return filtered;
}

// Filters out assistant messages that contain `tool_use` blocks for which NO
// matching `tool_result` block exists in the transcript.
//
// Mirrors TS filterIncompleteToolCalls in runAgent.ts. This is stricter than
// filter_resume_unresolved_tool_use_messages (which only drops an assistant
// message when ALL of its tool_uses are unresolved): here a single orphaned
// tool_use is enough to exclude the entire assistant message, because the
// Anthropic API rejects requests where any tool_use is missing its result.
//
// Use this when splicing a parent's conversation history into a sub-agent's
// context (fork / resume paths) to avoid sending malformed message sequences.
[[nodiscard]] std::vector<Message> filter_incomplete_tool_calls(
    std::vector<Message> messages
) {
    // migrated edge case: build the set of tool_use IDs that have results by
    // doing a full forward scan. TS does two passes (first pass collect
    // result IDs, second pass filter messages). We mirror exactly.
    std::unordered_set<std::string> tool_use_ids_with_results;
    for (const auto& message : messages) {
        if (message.role != "user") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
                tool_use_ids_with_results.insert(block.tool_use_id);
            }
        }
    }

    std::vector<Message> filtered;
    filtered.reserve(messages.size());
    for (auto& message : messages) {
        // migrated edge case: non-assistant messages always pass through; the
        // API only rejects malformed assistant→user tool_use/tool_result pairs.
        if (message.role != "assistant") {
            filtered.push_back(std::move(message));
            continue;
        }
        bool has_incomplete_tool_call = false;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
                if (!tool_use_ids_with_results.contains(block.tool_use_id)) {
                    has_incomplete_tool_call = true;
                    break;
                }
            }
        }
        // migrated edge case: exclude the assistant message if ANY tool_use
        // lacks a matching tool_result. TS keeps assistant messages with
        // zero tool_uses, even if they're whitespace-only.
        if (!has_incomplete_tool_call) {
            filtered.push_back(std::move(message));
        }
    }
    return filtered;
}

[[nodiscard]] std::unordered_map<std::string, std::string> resume_content_replacements_from_entries(
    const std::vector<std::string>& entries
) {
    std::unordered_map<std::string, std::string> replacements;
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed || !parsed->root().is_obj()) continue;
        auto root = parsed->root();
        if (json_string_member(root, "type") != "content-replacement") continue;

        auto replacement_entries = root.get("replacements");
        if (!replacement_entries.is_arr()) continue;
        replacement_entries.iter([&](cc::utils::json::JsonVal item) {
            if (!item.is_obj()) return;
            auto kind = json_string_member(item, "kind");
            if (!kind.empty() && kind != "tool-result") return;
            auto id = json_string_member(item, "toolUseId");
            if (id.empty()) id = json_string_member(item, "tool_use_id");
            auto replacement = json_string_member(item, "replacement");
            if (!id.empty()) replacements[std::move(id)] = std::move(replacement);
        });
    }
    return replacements;
}

void apply_resume_content_replacements(
    std::vector<Message>& messages,
    const std::unordered_map<std::string, std::string>& replacements
) {
    if (replacements.empty()) return;
    for (auto& message : messages) {
        if (message.role != "user") continue;
        for (auto& block : message.content) {
            if (block.type != ContentBlockType::ToolResult || block.tool_use_id.empty()) continue;
            if (auto replacement = replacements.find(block.tool_use_id); replacement != replacements.end()) {
                block.text = replacement->second;
            }
        }
    }
}

[[nodiscard]] std::vector<Message> resume_messages_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    auto messages = fork_context_messages_from_entries(entries);
    // migrated edge case: drop assistant messages with any tool_use that
    // never received a tool_result (mirrors TS filterUnresolvedToolUses in
    // resumeAgent). filter_resume_unresolved_tool_use_messages handles the
    // case where an assistant message's tool_uses are *all* unresolved;
    // filter_incomplete_tool_calls is stricter and drops an assistant
    // whenever *any* tool_use lacks a result.
    messages = filter_incomplete_tool_calls(std::move(messages));
    messages = filter_resume_unresolved_tool_use_messages(std::move(messages));
    messages = filter_resume_orphaned_thinking_messages(std::move(messages));
    messages = filter_resume_whitespace_assistant_messages(std::move(messages));
    apply_resume_content_replacements(messages, resume_content_replacements_from_entries(entries));
    return messages;
}

void append_agent_sidechain_message(std::string_view agent_id, const Message& message) {
    cc::tools::agent_runtime::native_agent_store().append_sidechain_message(
        agent_id,
        message.role,
        message_content_sidechain_json(message),
        message_content_text(message));
}

} // namespace cc::tools::agent::utils
