// Implementation unit for cc.query.query_engine — conversation compaction
// and tool-result budgeting: time-based microcompaction, the large-output
// disk-spill budget, snip-boundary replay, compaction summaries, token
// estimation, and the env/lowercase static helpers. The std::visit lambdas
// over ContentBlock/Message variants move verbatim inside their enclosing
// non-template functions.
module;

#include <cstdlib>

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.tools.agent_runtime;
// Genuinely used for cc::utils::PERSISTED_OUTPUT_TAG / ..._CLOSING_TAG /
// TOOL_RESULT_CLEARED_MESSAGE: namespace-scope constexpr string_views the
// dead-import heuristic does not harvest.
import cc.utils.tool_helpers;  // arch-check: keep-import
import cc.utils.env_utils;

namespace cc::core {

[[nodiscard]] std::string QueryEngine::lowercase_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

[[nodiscard]] bool QueryEngine::tool_result_already_replaced(std::string_view text) {
    return text.starts_with(cc::utils::PERSISTED_OUTPUT_TAG);
}

[[nodiscard]] bool QueryEngine::env_truthy_any(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (cc::utils::is_env_truthy(std::getenv(name))) return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t QueryEngine::env_uint_or_default(
    std::initializer_list<const char*> names,
    std::uint32_t fallback) {
    for (const char* name : names) {
        const char* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0') continue;
        char* end = nullptr;
        const auto value = std::strtoul(raw, &end, 10);
        if (end == raw || value == 0) continue;
        return static_cast<std::uint32_t>(
            std::min<unsigned long>(value, std::numeric_limits<std::uint32_t>::max()));
    }
    return fallback;
}

[[nodiscard]] bool QueryEngine::time_based_microcompact_enabled() {
    return env_truthy_any({
        "LOOM_TIME_BASED_MICROCOMPACT",
        "LOOM_TIME_BASED_MICROCOMPACT",
    });
}

[[nodiscard]] std::uint32_t QueryEngine::time_based_microcompact_gap_minutes() {
    return env_uint_or_default({
        "LOOM_TIME_BASED_MICROCOMPACT_GAP_MINUTES",
        "LOOM_TIME_BASED_MICROCOMPACT_GAP_MINUTES",
    }, 60);
}

[[nodiscard]] std::uint32_t QueryEngine::time_based_microcompact_keep_recent() {
    return std::max<std::uint32_t>(1, env_uint_or_default({
        "LOOM_TIME_BASED_MICROCOMPACT_KEEP_RECENT",
        "LOOM_TIME_BASED_MICROCOMPACT_KEEP_RECENT",
    }, 5));
}

[[nodiscard]] bool QueryEngine::is_time_based_microcompact_tool(std::string_view tool_name) {
    const auto name = lowercase_ascii(tool_name);
    static const std::unordered_set<std::string> compactable = {
        "bash",
        "powershell",
        "glob",
        "grep",
        "read",
        "webfetch",
        "websearch",
        "edit",
        "write",
    };
    return compactable.contains(name);
}

[[nodiscard]] std::unordered_set<std::string> QueryEngine::unbounded_tool_result_budget_names() const {
    std::unordered_set<std::string> names;
    if (!tool_registry_) return names;
    for (const auto& definition : tool_registry_->get_visible_definitions()) {
        if (definition.max_result_size_unbounded) {
            names.insert(lowercase_ascii(definition.name));
        }
    }
    return names;
}

[[nodiscard]] std::unordered_map<std::string, std::string> QueryEngine::tool_name_by_tool_use_id(
    const std::vector<Message>& messages) {
    std::unordered_map<std::string, std::string> names;
    for (const auto& message : messages) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            for (const auto& block : assistant->content) {
                if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                    names[tool_use->id.value] = tool_use->name;
                }
            }
        } else if (const auto* tool_use_message = std::get_if<ToolUseMessage>(&message)) {
            for (const auto& block : tool_use_message->content) {
                if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                    names[tool_use->id.value] = tool_use->name;
                }
            }
        }
    }
    return names;
}

[[nodiscard]] std::optional<std::string> QueryEngine::tool_result_plain_text_content(
    const ToolResultMessage& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* text_block = std::get_if<TextBlock>(&block)) {
            text += text_block->text;
        } else {
            return std::nullopt;
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] std::optional<std::string> QueryEngine::tool_result_text_for_budget(
    const ToolResultMessage& message) {
    auto text = tool_result_plain_text_content(message);
    if (!text || tool_result_already_replaced(*text)) {
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] std::vector<std::vector<QueryEngine::QueryToolResultBudgetCandidate>>
QueryEngine::collect_tool_result_budget_candidates_by_message(
    const std::vector<Message>& messages,
    const std::unordered_map<std::string, std::string>& tool_names) {
    std::vector<std::vector<QueryToolResultBudgetCandidate>> groups;
    std::vector<QueryToolResultBudgetCandidate> current;
    auto flush = [&] {
        if (!current.empty()) groups.push_back(std::exchange(current, {}));
    };

    for (std::size_t message_index = 0; message_index < messages.size(); ++message_index) {
        const auto& message = messages[message_index];
        if (std::holds_alternative<AssistantMessage>(message) ||
            std::holds_alternative<ToolUseMessage>(message)) {
            flush();
            continue;
        }
        const auto* tool_result = std::get_if<ToolResultMessage>(&message);
        if (!tool_result || tool_result->tool_use_id.value.empty()) continue;
        auto text = tool_result_text_for_budget(*tool_result);
        if (!text) continue;
        std::string tool_name;
        if (auto name = tool_names.find(tool_result->tool_use_id.value); name != tool_names.end()) {
            tool_name = name->second;
        }
        current.push_back(QueryToolResultBudgetCandidate{
            .message_index = message_index,
            .tool_use_id = tool_result->tool_use_id.value,
            .tool_name = std::move(tool_name),
            .size = text->size(),
        });
    }
    flush();
    return groups;
}

[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
QueryEngine::last_assistant_timestamp(const std::vector<Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&*it)) {
            return assistant->timestamp;
        }
        if (const auto* tool_use = std::get_if<ToolUseMessage>(&*it)) {
            return tool_use->timestamp;
        }
    }
    return std::nullopt;
}

void QueryEngine::apply_time_based_microcompact() {
    if (!time_based_microcompact_enabled()) return;

    std::lock_guard lock(conversation_mutex_);
    const auto last_assistant = last_assistant_timestamp(conversation_);
    if (!last_assistant) return;

    const auto now = std::chrono::system_clock::now();
    if (*last_assistant > now) return;
    const auto gap = std::chrono::duration_cast<std::chrono::minutes>(now - *last_assistant);
    if (gap < std::chrono::minutes{time_based_microcompact_gap_minutes()}) return;

    std::vector<std::string> compactable_ids;
    for (const auto& message : conversation_) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            for (const auto& block : assistant->content) {
                const auto* tool_use = std::get_if<ToolUseBlock>(&block);
                if (tool_use && is_time_based_microcompact_tool(tool_use->name)) {
                    compactable_ids.push_back(tool_use->id.value);
                }
            }
        } else if (const auto* tool_use_message = std::get_if<ToolUseMessage>(&message)) {
            for (const auto& block : tool_use_message->content) {
                const auto* tool_use = std::get_if<ToolUseBlock>(&block);
                if (tool_use && is_time_based_microcompact_tool(tool_use->name)) {
                    compactable_ids.push_back(tool_use->id.value);
                }
            }
        }
    }

    const auto keep_recent = static_cast<std::size_t>(time_based_microcompact_keep_recent());
    if (compactable_ids.size() <= keep_recent) return;

    std::unordered_set<std::string> keep_ids;
    const auto keep_start = compactable_ids.size() - keep_recent;
    for (std::size_t i = keep_start; i < compactable_ids.size(); ++i) {
        keep_ids.insert(compactable_ids[i]);
    }

    std::unordered_set<std::string> clear_ids;
    for (const auto& id : compactable_ids) {
        if (!keep_ids.contains(id)) clear_ids.insert(id);
    }
    if (clear_ids.empty()) return;

    bool changed = false;
    for (auto& message : conversation_) {
        auto* result = std::get_if<ToolResultMessage>(&message);
        if (!result || !clear_ids.contains(result->tool_use_id.value)) continue;
        auto existing_text = tool_result_plain_text_content(*result);
        if (existing_text && *existing_text == cc::utils::TOOL_RESULT_CLEARED_MESSAGE) {
            continue;
        }
        result->content.clear();
        result->content.push_back(TextBlock{std::string(cc::utils::TOOL_RESULT_CLEARED_MESSAGE)});
        changed = true;
    }

    if (changed) {
        rebuild_content_replacement_state_locked();
    }
}

[[nodiscard]] std::filesystem::path QueryEngine::query_tool_result_path(std::string_view tool_use_id) const {
    return cc::tools::agent_runtime::runtime_state_dir() /
        "tool-results" /
        (cc::tools::agent_runtime::safe_agent_filename(session_id_.str()) + "-" +
            cc::tools::agent_runtime::safe_agent_filename(tool_use_id) + ".txt");
}

[[nodiscard]] std::optional<std::string> QueryEngine::build_query_tool_result_replacement(
    const QueryToolResultBudgetCandidate& candidate,
    std::string_view content) const {
    auto path = query_tool_result_path(candidate.tool_use_id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return std::nullopt;
    if (!std::filesystem::exists(path, ec)) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return std::nullopt;
        out << content;
        if (!out.good()) return std::nullopt;
    }

    constexpr std::size_t preview_size = 2'000;
    const auto preview_len = std::min(preview_size, content.size());
    std::string replacement;
    replacement.reserve(preview_len + path.string().size() + 192);
    replacement += cc::utils::PERSISTED_OUTPUT_TAG;
    replacement += "\n";
    replacement += std::format(
        "Output too large ({} bytes). Full output saved to: {}\n\n",
        content.size(),
        path.string());
    replacement += std::format("Preview (first {} bytes):\n", preview_len);
    replacement += content.substr(0, preview_len);
    replacement += content.size() > preview_len ? "\n...\n" : "\n";
    replacement += cc::utils::PERSISTED_OUTPUT_CLOSING_TAG;
    return replacement;
}

void QueryEngine::replace_tool_result_message_locked(std::size_t message_index, std::string replacement) {
    auto* tool_result = std::get_if<ToolResultMessage>(&conversation_[message_index]);
    if (!tool_result) return;
    tool_result->content.clear();
    tool_result->content.push_back(TextBlock{std::move(replacement)});
}

void QueryEngine::rebuild_content_replacement_state_locked() {
    content_replacement_seen_ids_.clear();
    content_replacements_.clear();
    for (const auto& message : conversation_) {
        const auto* tool_result = std::get_if<ToolResultMessage>(&message);
        if (!tool_result || tool_result->tool_use_id.value.empty()) continue;
        auto text = tool_result_plain_text_content(*tool_result);
        if (!text) continue;
        content_replacement_seen_ids_.insert(tool_result->tool_use_id.value);
        if (tool_result_already_replaced(*text)) {
            content_replacements_[tool_result->tool_use_id.value] = std::move(*text);
        }
    }
}

void QueryEngine::apply_tool_result_budget() {
    std::lock_guard lock(conversation_mutex_);
    if (conversation_.empty()) return;

    static constexpr std::size_t max_tool_results_per_message_chars = 200'000;
    const auto tool_names = tool_name_by_tool_use_id(conversation_);
    const auto skip_tool_names = unbounded_tool_result_budget_names();

    for (const auto& candidates : collect_tool_result_budget_candidates_by_message(conversation_, tool_names)) {
        std::vector<QueryToolResultBudgetCandidate> fresh;
        std::size_t frozen_size = 0;
        std::size_t fresh_size = 0;

        for (const auto& candidate : candidates) {
            if (auto replacement = content_replacements_.find(candidate.tool_use_id);
                replacement != content_replacements_.end()) {
                replace_tool_result_message_locked(candidate.message_index, replacement->second);
                content_replacement_seen_ids_.insert(candidate.tool_use_id);
            } else if (content_replacement_seen_ids_.contains(candidate.tool_use_id)) {
                frozen_size += candidate.size;
            } else if (!candidate.tool_name.empty() &&
                       skip_tool_names.contains(lowercase_ascii(candidate.tool_name))) {
                content_replacement_seen_ids_.insert(candidate.tool_use_id);
            } else {
                fresh.push_back(candidate);
                fresh_size += candidate.size;
            }
        }

        if (fresh.empty()) continue;
        std::vector<QueryToolResultBudgetCandidate> selected;
        if (frozen_size + fresh_size > max_tool_results_per_message_chars) {
            std::ranges::sort(fresh, {}, &QueryToolResultBudgetCandidate::size);
            std::ranges::reverse(fresh);
            auto remaining = frozen_size + fresh_size;
            for (const auto& candidate : fresh) {
                if (remaining <= max_tool_results_per_message_chars) break;
                selected.push_back(candidate);
                remaining -= candidate.size;
            }
        }

        std::unordered_set<std::string> selected_ids;
        for (const auto& candidate : selected) {
            selected_ids.insert(candidate.tool_use_id);
        }
        for (const auto& candidate : fresh) {
            if (!selected_ids.contains(candidate.tool_use_id)) {
                content_replacement_seen_ids_.insert(candidate.tool_use_id);
            }
        }

        for (const auto& candidate : selected) {
            auto* tool_result = std::get_if<ToolResultMessage>(&conversation_[candidate.message_index]);
            if (!tool_result) continue;
            auto text = tool_result_text_for_budget(*tool_result);
            content_replacement_seen_ids_.insert(candidate.tool_use_id);
            if (!text) continue;
            auto replacement = build_query_tool_result_replacement(candidate, *text);
            if (!replacement) continue;
            replace_tool_result_message_locked(candidate.message_index, *replacement);
            content_replacements_[candidate.tool_use_id] = std::move(*replacement);
        }
    }
}

[[nodiscard]] std::string QueryEngine::truncate_for_compaction(std::string text,
                                                               std::size_t limit) {
    if (text.size() <= limit) return text;
    text.resize(limit);
    text += "...";
    return text;
}

[[nodiscard]] std::string QueryEngine::content_block_compaction_text(const ContentBlock& block) {
    return std::visit([](const auto& b) -> std::string {
        using T = std::remove_cvref_t<decltype(b)>;
        if constexpr (std::same_as<T, TextBlock>) {
            return b.text;
        } else if constexpr (std::same_as<T, ToolUseBlock>) {
            return std::format("[tool_use:{} {}]", b.name, b.input_json);
        } else if constexpr (std::same_as<T, ToolResultBlock>) {
            return std::format("[tool_result:{} error={}] {}",
                b.tool_use_id.value,
                b.is_error ? "true" : "false",
                tool_result_content_text(b));
        } else if constexpr (std::same_as<T, ImageBlock>) {
            return std::format("[image:{} {} bytes]", b.media_type, b.data.size());
        } else if constexpr (std::same_as<T, DocumentBlock>) {
            return std::format("[document:{} {} bytes]", b.media_type, b.data.size());
        } else if constexpr (std::same_as<T, ThinkingBlock>) {
            return "[thinking omitted from compact summary]";
        } else {
            return "[unknown content]";
        }
    }, block);
}

[[nodiscard]] std::string QueryEngine::message_compaction_text(const Message& message) {
    std::string text;
    std::visit([&](const auto& m) {
        for (const auto& block : m.content) {
            auto block_text = content_block_compaction_text(block);
            if (block_text.empty()) continue;
            if (!text.empty()) text += "\n";
            text += std::move(block_text);
        }
    }, message);
    if (text.empty()) return "[empty message]";
    return truncate_for_compaction(std::move(text), 300);
}

[[nodiscard]] bool QueryEngine::is_compact_boundary_message(const Message& message) {
    const auto* system = std::get_if<SystemMessage>(&message);
    return system && system->subtype == "compact_boundary";
}

[[nodiscard]] bool QueryEngine::is_snip_boundary_message(const Message& message) {
    const auto* system = std::get_if<SystemMessage>(&message);
    return system && system->subtype == "snip_boundary" && system->snip_metadata.has_value();
}

void QueryEngine::replay_snip_boundaries() {
    std::lock_guard lock(conversation_mutex_);
    std::unordered_set<std::string> removed_ids;
    for (const auto& message : conversation_) {
        const auto* system = std::get_if<SystemMessage>(&message);
        if (!system || !system->snip_metadata) continue;
        for (const auto& uuid : system->snip_metadata->removed_uuids) {
            if (!uuid.empty()) removed_ids.insert(uuid);
        }
    }
    if (removed_ids.empty()) return;

    auto should_remove = [&](const Message& message) {
        const auto id = message_id_value(message);
        const bool protected_system =
            std::holds_alternative<SystemMessage>(message) && !is_snip_boundary_message(message);
        return removed_ids.contains(id) && !protected_system;
    };
    if (!std::ranges::any_of(conversation_, should_remove)) return;

    std::vector<Message> projected;
    projected.reserve(conversation_.size());
    for (auto& message : conversation_) {
        if (should_remove(message)) continue;
        projected.push_back(std::move(message));
    }

    conversation_ = std::move(projected);
    rebuild_content_replacement_state_locked();
}

[[nodiscard]] std::string QueryEngine::message_id_value(const Message& message) {
    return std::visit([](const auto& value) {
        return value.id.value;
    }, message);
}

[[nodiscard]] std::optional<CompactPreservedSegment> QueryEngine::compact_preserved_segment(
    std::vector<Message>::const_iterator first,
    std::vector<Message>::const_iterator last,
    std::string_view anchor_id) {
    if (first == last) return std::nullopt;
    return CompactPreservedSegment{
        .head_uuid = message_id_value(*first),
        .anchor_uuid = std::string(anchor_id),
        .tail_uuid = message_id_value(*(last - 1)),
    };
}

[[nodiscard]] std::string QueryEngine::build_compaction_summary(
    std::vector<Message>::const_iterator first,
    std::vector<Message>::const_iterator last) {
    const auto count = static_cast<std::size_t>(std::ranges::count_if(
        first,
        last,
        [](const Message& message) {
            return !is_compact_boundary_message(message);
        }));
    std::string summary = std::format(
        "[Conversation compacted: {} older messages summarized]\n"
        "Preserve these details from the compacted history:",
        count);
    constexpr std::size_t max_summary_chars = 8000;
    std::size_t index = 1;
    for (auto it = first; it != last; ++it) {
        if (is_compact_boundary_message(*it)) continue;
        auto line = std::format("\n- {} #{}: {}",
            role_to_string(get_role(*it)),
            index++,
            message_compaction_text(*it));
        if (summary.size() + line.size() > max_summary_chars) {
            summary += "\n- [remaining compacted messages omitted due to summary size limit]";
            break;
        }
        summary += std::move(line);
    }
    return summary;
}

[[nodiscard]] bool QueryEngine::is_tool_enabled_for_query(
    std::string_view tool_name,
    const QueryOptions& options) {
    if (options.enabled_tools.empty()) return true;
    return std::ranges::any_of(options.enabled_tools, [tool_name](const std::string& enabled) {
        return enabled == tool_name;
    });
}

[[nodiscard]] std::uint32_t QueryEngine::estimate_conversation_tokens_locked() const noexcept {
    // Rough estimate: ~4 chars per token for English text
    std::uint32_t total_chars = 0;
    for (const auto& msg : conversation_) {
        std::visit([&total_chars](const auto& m) {
            for (const auto& block : m.content) {
                if (const auto* text = std::get_if<TextBlock>(&block)) {
                    total_chars += static_cast<std::uint32_t>(text->text.size());
                }
            }
        }, msg);
    }
    return total_chars / 4;
}

} // namespace cc::core
