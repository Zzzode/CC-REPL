module;

#include <atomic>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <utility>
#include <sstream>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/wait.h>

export module cc.tools.agent.fork;

import cc.utils.error;
import cc.utils.git;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.agent_runtime;
import cc.tools.agent_constants;
import cc.tools.agent_memory;
import cc.tools.agent_memory_snapshot;
import cc.tools.agent_color_manager;
import cc.tools.agent_display;
import cc.tools.bash;
import cc.tools.todo_write;
import cc.tools.send_message;
import cc.tools.team;
import cc.tools.mcp;
import cc.tools.sleep;
import cc.tools.web_fetch;
import cc.skills.skill;
import cc.utils.team_helpers;
import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.bootstrap;
import cc.services.mcp.types;
import cc.utils.swarm_backends;
import cc.utils.env_utils;
import cc.utils.tool_helpers;
import cc.utils.bash_execution;
import cc.tools.agent.utils;

export namespace cc::tools::agent::fork_ {

namespace fs = std::filesystem;

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::tools::agent::utils::AgentExecutionPlan;
using cc::tools::agent::utils::message_from_json_value;
using cc::tools::agent::utils::text_contains_fork_boilerplate;
using cc::tools::agent::utils::message_json_object;
using cc::tools::agent::utils::agent_tool_input_omits_agent_type;


[[nodiscard]] inline std::vector<Message> forked_messages_from_parent_assistant(
    std::string_view directive,
    Message assistant_message
) {
    assistant_message.role = "assistant";

    std::vector<ContentBlock> tool_uses;
    for (const auto& block : assistant_message.content) {
        if (block.type == ContentBlockType::ToolUse && !block.tool_use_id.empty()) {
            tool_uses.push_back(block);
        }
    }

    const auto directive_message = cc::tools::agent_runtime::build_fork_child_message(directive);
    if (tool_uses.empty()) {
        return {Message::from_text("user", directive_message)};
    }

    Message missing_tool_results;
    missing_tool_results.role = "user";
    for (const auto& tool_use : tool_uses) {
        missing_tool_results.content.push_back(ContentBlock{
            .type = ContentBlockType::ToolResult,
            .text = "Fork started \u2014 processing in background",
            .tool_use_id = tool_use.tool_use_id,
        });
    }
    missing_tool_results.content.push_back(ContentBlock{
        .type = ContentBlockType::Text,
        .text = directive_message,
    });

    return {std::move(assistant_message), std::move(missing_tool_results)};
}

[[nodiscard]] inline std::vector<Message> forked_messages_from_parent_assistant_entries(
    std::string_view directive,
    const std::vector<std::string>& entries
) {
    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed) continue;
        if (auto message = message_from_json_value(parsed->root())) {
            return forked_messages_from_parent_assistant(directive, std::move(*message));
        }
    }
    return {};
}

[[nodiscard]] inline bool message_contains_fork_boilerplate(const Message& message) {
    return std::ranges::any_of(message.content, [](const ContentBlock& block) {
        return block.type == ContentBlockType::Text && text_contains_fork_boilerplate(block.text);
    });
}

[[nodiscard]] inline bool messages_contain_fork_boilerplate(const std::vector<Message>& messages) {
    return std::ranges::any_of(messages, [](const Message& message) {
        return message_contains_fork_boilerplate(message);
    });
}

[[nodiscard]] inline bool should_reject_fork_child_agent_call(
    const AgentExecutionPlan& plan,
    std::string_view tool_name,
    std::string_view tool_input_json
) {
    if (!plan.fork_child_context) return false;
    if (tool_name != "Agent") return false;
    return agent_tool_input_omits_agent_type(tool_input_json);
}

[[nodiscard]] inline bool exact_tools_allow_tool(
    const AgentExecutionPlan& plan,
    std::string_view tool_name
) {
    if (!plan.use_exact_tools) return true;
    if (plan.exact_tools.empty()) return true;
    return std::ranges::contains(plan.exact_tools, tool_name);
}

[[nodiscard]] inline bool implicit_fork_injection_replaces_key(std::string_view key) {
    constexpr std::array<std::string_view, 16> keys{
        "query_source",
        "querySource",
        "fork_child",
        "forkChild",
        "run_in_background",
        "parent_system_prompt",
        "parentSystemPrompt",
        "exact_tools",
        "exactTools",
        "available_tools",
        "availableTools",
        "use_exact_tools",
        "useExactTools",
        "parent_assistant_message",
        "parentAssistantMessage",
        "assistantMessage",
    };
    return std::ranges::contains(keys, key);
}

[[nodiscard]] inline std::vector<std::string> exact_tool_names_from_api_tools(
    const std::vector<cc::services::api::ToolDefinition>& tools
) {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& tool : tools) {
        if (tool.name.empty() || std::ranges::contains(names, tool.name)) continue;
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] inline std::string build_implicit_fork_agent_input_json(
    std::string_view raw_json,
    const AgentExecutionPlan& parent_plan,
    const Message& parent_assistant_message,
    const std::vector<cc::services::api::ToolDefinition>& parent_tools
) {
    auto parsed = cc::utils::json::parse(raw_json);
    if (!parsed || !parsed->root().is_obj()) return std::string(raw_json);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto key_text = key.as_str();
        if (implicit_fork_injection_replaces_key(key_text)) return;
        root.add(key_text, doc.copy_val(value));
    });

    root.add("querySource", doc.string("agent:builtin:fork"));
    root.add("forkChild", doc.boolean(true));
    root.add("run_in_background", doc.boolean(true));
    if (!parent_plan.system_prompt.empty()) {
        root.add("parentSystemPrompt", doc.string(parent_plan.system_prompt));
    }

    auto exact_tools = doc.array();
    for (const auto& name : exact_tool_names_from_api_tools(parent_tools)) {
        exact_tools.append(doc.string(name));
    }
    root.add("exactTools", exact_tools);
    root.add("useExactTools", doc.boolean(true));

    if (auto parent_message = doc.raw_json(message_json_object(parent_assistant_message));
        parent_message.valid()) {
        root.add("parentAssistantMessage", parent_message);
    }

    doc.set_root(root);
    return doc.to_string();
}


} // namespace cc::tools::agent::fork_
