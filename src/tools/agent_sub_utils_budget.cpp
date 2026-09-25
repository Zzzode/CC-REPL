// Implementation unit for cc.tools.agent.utils — GrowthBook env overrides,
// threshold maps, tool-result candidate grouping, persisted-output
// replacement writing, and budget application. The template
// with_agent_growthbook_env_overrides stays defined in the interface and is
// merely called from here.
module;

module cc.tools.agent.utils;

import std;

import cc.utils.json;
// Genuinely used: tool_result_already_replaced and
// build_agent_tool_result_replacement reference cc::utils::PERSISTED_OUTPUT_TAG
// / PERSISTED_OUTPUT_CLOSING_TAG (defined in tool_helpers.cppm); graph_check
// cannot see qualified cc::utils::NAME evidence because cc::utils is a
// shallow (<3 segment) namespace path.
import cc.utils.tool_helpers;  // arch-check: keep-import
import cc.tools.tool;
import cc.tools.agent_runtime;
import cc.services.api.client;

namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

[[nodiscard]] bool agent_growthbook_env_overrides_enabled() {
    const char* user_type = std::getenv("USER_TYPE");
    return user_type && std::string_view(user_type) == "ant";
}

[[nodiscard]] std::optional<std::size_t> json_positive_size_t(
    cc::utils::json::JsonVal value
) {
    if (!value.valid() || !value.is_num()) return std::nullopt;
    const auto numeric = value.as_double();
    if (!std::isfinite(numeric) || numeric <= 0.0) return std::nullopt;
    if (numeric > static_cast<double>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
    return static_cast<std::size_t>(numeric);
}

[[nodiscard]] std::optional<std::size_t> agent_tool_threshold_override(
    std::string_view tool_name
) {
    std::optional<std::size_t> override;
    with_agent_growthbook_env_overrides([&](cc::utils::json::JsonVal root) {
        auto overrides = root.get(AGENT_PERSIST_THRESHOLD_OVERRIDE_FLAG);
        if (!overrides.valid() || !overrides.is_obj()) return;

        auto lookup = [&](std::string_view key) -> std::optional<std::size_t> {
            return json_positive_size_t(overrides.get(key));
        };

        override = lookup(tool_name);
        if (!override) {
            const auto lowered = lowercase_ascii(tool_name);
            if (lowered != tool_name) override = lookup(lowered);
        }
    });
    return override;
}

[[nodiscard]] std::size_t agent_per_message_budget_limit() {
    std::optional<std::size_t> override;
    with_agent_growthbook_env_overrides([&](cc::utils::json::JsonVal root) {
        override = json_positive_size_t(root.get(AGENT_PER_MESSAGE_BUDGET_OVERRIDE_FLAG));
    });
    return override.value_or(AGENT_MAX_TOOL_RESULTS_PER_MESSAGE_CHARS);
}

[[nodiscard]] AgentContentReplacementState agent_content_replacement_state_from_entries(
    const std::vector<std::string>& entries
) {
    return AgentContentReplacementState{
        .seen_ids = {},
        .replacements = resume_content_replacements_from_entries(entries),
    };
}

void mark_seen_tool_result_ids(
    AgentContentReplacementState& state,
    const std::vector<Message>& messages
) {
    for (const auto& message : messages) {
        if (message.role != "user") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolResult && !block.tool_use_id.empty()) {
                state.seen_ids.insert(block.tool_use_id);
            }
        }
    }
}

[[nodiscard]] bool tool_result_already_replaced(std::string_view text) {
    return text.starts_with(cc::utils::PERSISTED_OUTPUT_TAG);
}

[[nodiscard]] std::unordered_set<std::string> unbounded_tool_result_budget_names(
    const std::vector<ToolDefinition>& definitions
) {
    std::unordered_set<std::string> names;
    for (const auto& definition : definitions) {
        if (definition.max_result_size_unbounded) {
            names.insert(lowercase_ascii(definition.name));
        }
    }
    return names;
}

[[nodiscard]] std::unordered_map<std::string, std::size_t> tool_result_budget_thresholds(
    const std::vector<ToolDefinition>& definitions
) {
    std::unordered_map<std::string, std::size_t> thresholds;
    for (const auto& definition : definitions) {
        if (definition.max_result_size_unbounded) continue;
        auto threshold = agent_tool_threshold_override(definition.name).value_or(definition.max_result_size_chars == 0
            ? AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS
            : std::min(definition.max_result_size_chars, AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS));
        thresholds[lowercase_ascii(definition.name)] = threshold;
    }
    return thresholds;
}

[[nodiscard]] std::unordered_map<std::string, std::string> tool_name_by_tool_use_id(
    const std::vector<Message>& messages
) {
    std::unordered_map<std::string, std::string> names;
    for (const auto& message : messages) {
        if (message.role != "assistant") continue;
        for (const auto& block : message.content) {
            if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
                names[block.tool_use_id] = block.tool_name;
            }
        }
    }
    return names;
}

[[nodiscard]] bool should_skip_agent_budget_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_set<std::string>& skip_tool_names,
    std::string_view tool_use_id
) {
    auto it = tool_names.find(std::string(tool_use_id));
    if (it == tool_names.end()) return false;
    return skip_tool_names.contains(lowercase_ascii(it->second));
}

[[nodiscard]] std::size_t agent_budget_threshold_for_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_map<std::string, std::size_t>& thresholds,
    std::string_view tool_use_id
) {
    auto name = tool_names.find(std::string(tool_use_id));
    if (name == tool_names.end()) return AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS;
    auto threshold = thresholds.find(lowercase_ascii(name->second));
    if (threshold == thresholds.end()) return AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS;
    return threshold->second;
}

[[nodiscard]] std::vector<std::vector<AgentToolResultCandidate>> collect_agent_budget_candidates_by_message(
    const std::vector<Message>& messages
) {
    std::vector<std::vector<AgentToolResultCandidate>> groups;
    std::vector<AgentToolResultCandidate> current;
    auto flush = [&] {
        if (!current.empty()) groups.push_back(std::exchange(current, {}));
    };

    for (std::size_t message_index = 0; message_index < messages.size(); ++message_index) {
        const auto& message = messages[message_index];
        if (message.role == "assistant") {
            flush();
            continue;
        }
        if (message.role != "user") continue;
        for (std::size_t block_index = 0; block_index < message.content.size(); ++block_index) {
            const auto& block = message.content[block_index];
            if (block.type != ContentBlockType::ToolResult || block.tool_use_id.empty()) continue;
            if (block.text.empty() || tool_result_already_replaced(block.text)) continue;
            current.push_back(AgentToolResultCandidate{
                .message_index = message_index,
                .block_index = block_index,
                .tool_use_id = block.tool_use_id,
                .size = block.text.size(),
            });
        }
    }
    flush();
    return groups;
}

[[nodiscard]] fs::path agent_tool_result_path(
    std::string_view agent_id,
    std::string_view tool_use_id
) {
    return cc::tools::agent_runtime::runtime_state_dir() /
        "tool-results" /
        (cc::tools::agent_runtime::safe_agent_filename(agent_id) + "-" +
            cc::tools::agent_runtime::safe_agent_filename(tool_use_id) + ".txt");
}

[[nodiscard]] std::optional<std::string> build_agent_tool_result_replacement(
    std::string_view agent_id,
    const AgentToolResultCandidate& candidate,
    std::string_view content
) {
    auto path = agent_tool_result_path(agent_id, candidate.tool_use_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::nullopt;
    if (!fs::exists(path, ec)) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) return std::nullopt;
        out << content;
        if (!out.good()) return std::nullopt;
    }

    constexpr std::size_t preview_size = 2'000;
    const auto preview_len = std::min(preview_size, content.size());
    std::string preview{content.substr(0, preview_len)};
    const bool has_more = content.size() > preview_len;

    std::string replacement;
    replacement.reserve(preview.size() + path.string().size() + 192);
    replacement += cc::utils::PERSISTED_OUTPUT_TAG;
    replacement += "\n";
    replacement += std::format(
        "Output too large ({} bytes). Full output saved to: {}\n\n",
        content.size(),
        path.string());
    replacement += std::format("Preview (first {} bytes):\n", preview_len);
    replacement += preview;
    replacement += has_more ? "\n...\n" : "\n";
    replacement += cc::utils::PERSISTED_OUTPUT_CLOSING_TAG;
    return replacement;
}

[[nodiscard]] std::string agent_content_replacement_entry_json(
    std::string_view agent_id,
    const std::vector<std::pair<std::string, std::string>>& replacements
) {
    std::string out = R"({"type":"content-replacement","sessionId":")";
    out += json_escape_string(teammate_parent_session_id());
    out += R"(","agentId":")";
    out += json_escape_string(agent_id);
    out += R"(","replacements":[)";
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        if (i != 0) out += ',';
        out += R"({"kind":"tool-result","toolUseId":")";
        out += json_escape_string(replacements[i].first);
        out += R"(","replacement":")";
        out += json_escape_string(replacements[i].second);
        out += R"("})";
    }
    out += R"(]})";
    return out;
}

AgentToolResultBudgetOutcome apply_agent_tool_result_budget(
    std::string_view agent_id,
    std::vector<Message>& messages,
    AgentContentReplacementState& state,
    const std::unordered_set<std::string>& skip_tool_names,
    const std::unordered_map<std::string, std::size_t>& tool_thresholds
) {
    AgentToolResultBudgetOutcome outcome;
    const auto tool_names = tool_name_by_tool_use_id(messages);
    const auto message_budget_limit = agent_per_message_budget_limit();
    std::vector<std::pair<std::string, std::string>> newly_replaced;

    for (const auto& candidates : collect_agent_budget_candidates_by_message(messages)) {
        std::vector<AgentToolResultCandidate> fresh;
        std::size_t frozen_size = 0;
        std::size_t fresh_size = 0;

        for (const auto& candidate : candidates) {
            if (auto replacement = state.replacements.find(candidate.tool_use_id);
                replacement != state.replacements.end()) {
                messages[candidate.message_index].content[candidate.block_index].text = replacement->second;
                state.seen_ids.insert(candidate.tool_use_id);
                ++outcome.reapplied;
            } else if (state.seen_ids.contains(candidate.tool_use_id)) {
                frozen_size += candidate.size;
            } else if (should_skip_agent_budget_candidate(tool_names, skip_tool_names, candidate.tool_use_id)) {
                state.seen_ids.insert(candidate.tool_use_id);
            } else {
                fresh.push_back(candidate);
                fresh_size += candidate.size;
            }
        }

        std::vector<AgentToolResultCandidate> selected;
        std::size_t selected_size = 0;
        std::unordered_set<std::string> selected_ids;
        for (const auto& candidate : fresh) {
            if (candidate.size > agent_budget_threshold_for_candidate(tool_names, tool_thresholds, candidate.tool_use_id)) {
                selected.push_back(candidate);
                selected_ids.insert(candidate.tool_use_id);
                selected_size += candidate.size;
            }
        }

        auto visible_size_after_threshold_replacements = frozen_size + fresh_size - selected_size;
        if (!fresh.empty() && visible_size_after_threshold_replacements > message_budget_limit) {
            std::vector<AgentToolResultCandidate> remaining_candidates;
            remaining_candidates.reserve(fresh.size());
            for (const auto& candidate : fresh) {
                if (!selected_ids.contains(candidate.tool_use_id)) {
                    remaining_candidates.push_back(candidate);
                }
            }
            std::ranges::sort(remaining_candidates, {}, &AgentToolResultCandidate::size);
            std::ranges::reverse(remaining_candidates);
            for (const auto& candidate : remaining_candidates) {
                if (visible_size_after_threshold_replacements <= message_budget_limit) break;
                selected.push_back(candidate);
                selected_ids.insert(candidate.tool_use_id);
                visible_size_after_threshold_replacements -= candidate.size;
            }
        }

        for (const auto& candidate : fresh) {
            if (!selected_ids.contains(candidate.tool_use_id)) {
                state.seen_ids.insert(candidate.tool_use_id);
            }
        }

        for (const auto& candidate : selected) {
            auto& block = messages[candidate.message_index].content[candidate.block_index];
            auto replacement = build_agent_tool_result_replacement(agent_id, candidate, block.text);
            state.seen_ids.insert(candidate.tool_use_id);
            if (!replacement) continue;
            block.text = *replacement;
            state.replacements[candidate.tool_use_id] = *replacement;
            newly_replaced.emplace_back(candidate.tool_use_id, *replacement);
            ++outcome.newly_replaced;
        }
    }

    if (!newly_replaced.empty()) {
        cc::tools::agent_runtime::native_agent_store().append_sidechain_entry(
            agent_id,
            agent_content_replacement_entry_json(agent_id, newly_replaced));
    }
    return outcome;
}

} // namespace cc::tools::agent::utils
