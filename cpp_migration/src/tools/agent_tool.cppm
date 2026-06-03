// AgentTool - Sub-agent delegation with recursive API loop
module;

#include <atomic>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <array>
#include <utility>
#include <sstream>
#include <cctype>

export module cc.tools.agent;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;
import cc.tools.agent_runtime;
import cc.tools.mcp;
import cc.skills.skill;
import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.bootstrap;

export namespace cc::tools::agent {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;
using cc::services::api::AnthropicClient;
using cc::services::api::CreateMessageRequest;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::services::api::StreamParser;
using cc::services::api::StreamEventType;
using cc::services::api::StreamContentBlockType;
using cc::services::api::get_default_client;

// =========================================================================
// Agent Configuration
// =========================================================================

struct AgentConfig {
    int max_turns = 200;           // Max agentic loop iterations
    int max_depth = 3;             // Max recursive agent nesting depth
    std::string default_model = "claude-sonnet-4-20250514";
    std::vector<std::string> allowed_tools;  // Empty = inherit all from parent
    std::vector<std::string> denied_tools;   // Explicitly blocked tools for sub-agents
};

[[nodiscard]] inline SchemaProperty agent_schema_property(
    std::string name,
    std::string type,
    std::string description,
    bool required = false
) {
    return SchemaProperty{
        std::move(name),
        std::move(type),
        std::move(description),
        required,
        std::nullopt,
        std::nullopt
    };
}

struct AgentToolRequest {
    std::string prompt;
    std::optional<std::string> description;
    std::string subagent_type = "general-purpose";
    std::optional<std::string> model;
    bool run_in_background = false;
    std::vector<std::string> unsupported_fields;
};

struct AgentMcpToolBinding {
    std::string server_name;
    std::string tool_name;
    std::string description;
};

struct AgentExecutionPlan {
    std::string prompt;
    std::string agent_type;
    std::string model;
    std::string system_prompt;
    std::vector<std::string> preloaded_skill_messages;
    std::vector<std::string> agent_mcp_servers;
    std::vector<AgentMcpToolBinding> agent_mcp_tools;
    std::optional<std::string> agent_mcp_context_message;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> disallowed_tools;
    int max_turns = 200;
};

[[nodiscard]] inline std::optional<std::string> json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] inline bool json_bool(
    cc::utils::json::JsonVal root,
    std::string_view key,
    bool fallback = false
) {
    auto value = root.get(key);
    return value.is_bool() ? value.as_bool() : fallback;
}

[[nodiscard]] inline bool has_non_empty_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    return value.is_str() && !value.as_str().empty();
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_model(std::optional<std::string> model) {
    if (!model || model->empty() || *model == "inherit") return std::nullopt;
    if (*model == "sonnet") return "claude-sonnet-4-20250514";
    if (*model == "opus") return "claude-opus-4-20250514";
    if (*model == "haiku") return "claude-3-5-haiku-20241022";
    return model;
}

[[nodiscard]] inline std::expected<AgentToolRequest, std::string> parse_agent_tool_request(
    const ToolInput& input
) {
    auto doc = cc::utils::json::parse(input.json());
    if (!doc) return std::unexpected(std::string(doc.error().format()));

    auto root = doc->root();
    if (!root.is_obj()) return std::unexpected("Agent input must be a JSON object");

    AgentToolRequest request;
    if (auto prompt = json_string(root, "prompt").or_else([&] { return json_string(root, "task"); })) {
        request.prompt = *prompt;
    }
    request.description = json_string(root, "description");
    if (auto subagent = json_string(root, "subagent_type").or_else([&] { return json_string(root, "skill"); })) {
        if (!subagent->empty()) request.subagent_type = *subagent;
    }
    request.model = resolve_agent_model(json_string(root, "model"));
    request.run_in_background = json_bool(root, "run_in_background", false);

    if (request.run_in_background) request.unsupported_fields.push_back("run_in_background");
    static constexpr std::array unsupported_string_fields = {"name", "team_name", "mode", "isolation", "cwd"};
    for (auto field : unsupported_string_fields) {
        if (has_non_empty_string(root, field)) request.unsupported_fields.emplace_back(field);
    }
    return request;
}

[[nodiscard]] inline std::string join_fields(const std::vector<std::string>& fields) {
    std::string output;
    for (const auto& field : fields) {
        if (!output.empty()) output += ", ";
        output += field;
    }
    return output;
}

[[nodiscard]] inline std::string built_in_system_prompt(std::string_view agent_type) {
    if (agent_type == "Explore") {
        return R"(You are an exploration agent. Your job is to understand codebases, find relevant files, and gather context.

Guidelines:
- Use search and read tools extensively.
- Summarize findings concisely.
- Identify key files, patterns, and architecture.
- Report dependencies and relationships between components.
- Do not make code changes.)";
    }
    if (agent_type == "Plan") {
        return R"(You are a planning agent. Your job is to create detailed implementation plans.

Guidelines:
- Break down tasks into clear, sequential steps.
- Identify risks and dependencies.
- Suggest testing strategies.
- Consider edge cases.
- Output a structured plan.
- Do not implement the plan yourself.)";
    }
    if (agent_type == "verification") {
        return R"(You are a verification agent. Your job is to test and validate completed work.

Guidelines:
- Run targeted checks when tools are available.
- Verify code compiles or builds successfully.
- Check for regressions.
- Validate that requirements are met.
- Report issues with specific evidence.)";
    }
    return R"(You are a sub-agent working on a delegated task. Complete the task thoroughly.

Guidelines:
- Focus only on the assigned task.
- Use available tools effectively.
- Report results concisely.
- If blocked, explain what is needed to proceed.)";
}

[[nodiscard]] inline bool tool_name_allowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& allowed_tools
) {
    if (allowed_tools.empty()) return true;
    for (const auto& allowed : allowed_tools) {
        if (allowed == "*" || allowed == tool_name) return true;
    }
    return false;
}

[[nodiscard]] inline bool tool_name_disallowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& disallowed_tools
) {
    for (const auto& disallowed : disallowed_tools) {
        if (disallowed == "*" || disallowed == tool_name) return true;
    }
    return false;
}

[[nodiscard]] inline std::string lowercase_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return out;
}

[[nodiscard]] inline bool case_insensitive_contains(std::string_view haystack, std::string_view needle) {
    return lowercase_copy(haystack).contains(lowercase_copy(needle));
}

[[nodiscard]] inline std::vector<std::string> available_mcp_servers_with_tools() {
    std::vector<std::string> names;
    for (const auto& status : cc::tools::native_mcp_statuses()) {
        if (status.status == "ready" && !status.tools.empty()) {
            names.push_back(status.name);
        }
    }
    return names;
}

[[nodiscard]] inline std::vector<std::string> missing_required_mcp_servers(
    const std::vector<std::string>& required_patterns,
    const std::vector<std::string>& available_servers
) {
    std::vector<std::string> missing;
    for (const auto& pattern : required_patterns) {
        bool matched = false;
        for (const auto& server : available_servers) {
            if (case_insensitive_contains(server, pattern)) {
                matched = true;
                break;
            }
        }
        if (!matched) missing.push_back(pattern);
    }
    return missing;
}

[[nodiscard]] inline std::string prepend_initial_prompt(
    const std::optional<std::string>& initial_prompt,
    std::string_view prompt
) {
    if (!initial_prompt || initial_prompt->empty()) return std::string(prompt);
    return std::format("{}\n\n{}", *initial_prompt, prompt);
}

[[nodiscard]] inline std::optional<std::string> resolve_agent_skill_name(
    std::string_view skill_name,
    const std::vector<cc::skills::SkillDefinition>& skills,
    const cc::tools::agent_runtime::AgentDefinition& agent_definition
) {
    for (const auto& skill : skills) {
        if (skill.name == skill_name) return skill.name;
    }

    const auto colon = agent_definition.agent_type.find(':');
    if (colon != std::string::npos) {
        const auto qualified = std::format("{}:{}", agent_definition.agent_type.substr(0, colon), skill_name);
        for (const auto& skill : skills) {
            if (skill.name == qualified) return skill.name;
        }
    }

    const auto suffix = std::format(":{}", skill_name);
    for (const auto& skill : skills) {
        if (skill.name.ends_with(suffix)) return skill.name;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::string format_preloaded_skill_message(
    std::string_view skill_name,
    std::string_view content
) {
    return std::format("<skill name=\"{}\">\n{}\n</skill>", skill_name, content);
}

[[nodiscard]] inline std::vector<std::string> load_preloaded_skill_messages(
    const cc::tools::agent_runtime::AgentDefinition& definition
) {
    std::vector<std::string> messages;
    if (definition.skills.empty()) return messages;

    cc::skills::SkillLoader loader;
    auto discovered = loader.discover_all();
    if (!discovered) return messages;

    for (const auto& requested : definition.skills) {
        auto resolved_name = resolve_agent_skill_name(requested, *discovered, definition);
        if (!resolved_name) continue;

        for (const auto& skill : *discovered) {
            if (skill.name != *resolved_name) continue;
            messages.push_back(format_preloaded_skill_message(skill.name, skill.content));
            break;
        }
    }
    return messages;
}

[[nodiscard]] inline std::string format_agent_mcp_context_message(
    const std::vector<AgentMcpToolBinding>& tools
) {
    if (tools.empty()) return {};

    std::string message = "The following MCP tools are available to this agent through the `mcp` tool.\n";
    message += "Call `mcp` with `server_name`, `tool_name`, and `arguments`.\n\n";
    for (const auto& tool : tools) {
        message += std::format("- {}/{}", tool.server_name, tool.tool_name);
        if (!tool.description.empty()) message += std::format(": {}", tool.description);
        message += "\n";
    }
    return message;
}

[[nodiscard]] inline std::vector<AgentMcpToolBinding> connect_agent_mcp_servers(
    const std::vector<std::string>& server_names
) {
    std::vector<AgentMcpToolBinding> tools;
    for (const auto& server_name : server_names) {
        if (server_name.empty()) continue;

        auto status = cc::tools::restart_native_mcp_server(server_name);
        if (!status || status->status != "ready") continue;

        for (const auto& tool : status->tools) {
            tools.push_back(AgentMcpToolBinding{
                .server_name = status->name,
                .tool_name = tool.name,
                .description = tool.description,
            });
        }
    }
    return tools;
}

[[nodiscard]] inline std::expected<AgentExecutionPlan, std::string> build_agent_execution_plan(
    const AgentToolRequest& request,
    const AgentConfig& config
) {
    const auto agents = cc::tools::agent_runtime::get_all_agent_definitions();
    auto resolved_type = cc::tools::agent_runtime::resolve_requested_agent_type(request.subagent_type, agents);
    if (!resolved_type) {
        return std::unexpected(std::format(
            "Agent type '{}' not found. Available agents: {}",
            request.subagent_type,
            cc::tools::agent_runtime::format_agent_type_list(agents)));
    }

    std::optional<cc::tools::agent_runtime::AgentDefinition> definition;
    for (const auto& agent : agents) {
        if (agent.agent_type == *resolved_type) {
            definition = agent;
            break;
        }
    }
    if (!definition) {
        return std::unexpected(std::format("Agent type '{}' could not be resolved", request.subagent_type));
    }

    std::vector<std::string> unsupported_definition_features;
    if (definition->background) unsupported_definition_features.push_back("background");
    if (definition->isolation) unsupported_definition_features.push_back("isolation");
    if (definition->hooks_present) unsupported_definition_features.push_back("hooks");
    if (!unsupported_definition_features.empty()) {
        return std::unexpected(std::format(
            "Agent definition '{}' uses features not yet supported by the native runtime: {}",
            definition->agent_type,
            join_fields(unsupported_definition_features)));
    }

    if (!definition->required_mcp_servers.empty()) {
        const auto available_servers = available_mcp_servers_with_tools();
        const auto missing = missing_required_mcp_servers(definition->required_mcp_servers, available_servers);
        if (!missing.empty()) {
            return std::unexpected(std::format(
                "Agent '{}' requires MCP servers matching: {}. MCP servers with tools: {}. Use /mcp to configure and authenticate the required MCP servers.",
                definition->agent_type,
                join_fields(missing),
                available_servers.empty() ? "none" : join_fields(available_servers)));
        }
    }

    auto definition_model = resolve_agent_model(definition->model);
    AgentExecutionPlan plan;
    plan.prompt = prepend_initial_prompt(definition->initial_prompt, request.prompt);
    plan.agent_type = definition->agent_type;
    plan.model = request.model.value_or(definition_model.value_or(config.default_model));
    plan.system_prompt = definition->system_prompt.empty()
        ? built_in_system_prompt(definition->agent_type)
        : definition->system_prompt;
    plan.preloaded_skill_messages = load_preloaded_skill_messages(*definition);
    plan.agent_mcp_servers = definition->mcp_servers;
    plan.agent_mcp_tools = connect_agent_mcp_servers(plan.agent_mcp_servers);
    if (!plan.agent_mcp_tools.empty()) {
        plan.agent_mcp_context_message = format_agent_mcp_context_message(plan.agent_mcp_tools);
    }
    plan.allowed_tools = definition->tools;
    plan.disallowed_tools = definition->disallowed_tools;
    plan.max_turns = definition->max_turns.value_or(config.max_turns);
    return plan;
}

// =========================================================================
// AgentTool Implementation
// =========================================================================

/// AgentTool - Delegates tasks to sub-agents via recursive API loop
class AgentTool {
public:
    static constexpr std::string_view kName = "Agent";
    static constexpr std::string_view kDescription = 
        "Launch a new agent to handle complex, multi-step tasks autonomously. "
        "The agent runs in a recursive loop, making API calls and executing tools "
        "until it completes the task or reaches the turn limit.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    agent_schema_property("description", "string", "A short description of the task", true),
                    agent_schema_property("prompt", "string", "The task for the agent to perform", true),
                    agent_schema_property("subagent_type", "string", "The specialized agent type to use"),
                    agent_schema_property("model", "string", "Optional model override: sonnet, opus, haiku, or a concrete model id"),
                    agent_schema_property("run_in_background", "boolean", "Run the agent asynchronously"),
                    agent_schema_property("task", "string", "Legacy alias for prompt"),
                    agent_schema_property("skill", "string", "Legacy alias for subagent_type"),
                    agent_schema_property("name", "string", "Teammate name for agent swarms"),
                    agent_schema_property("team_name", "string", "Team name for agent swarms"),
                    agent_schema_property("mode", "string", "Permission mode for spawned teammates"),
                    agent_schema_property("isolation", "string", "Isolation mode for worktree or remote agents"),
                    agent_schema_property("cwd", "string", "Working directory override for the spawned agent")
                }
            },
            .permission = ToolPermission::Execute,
            .category = "agent"
        };
    }
    
    explicit AgentTool(AgentConfig config = {}, int current_depth = 0,
                       cc::core::ToolRegistry* registry = nullptr)
        : config_(config), current_depth_(current_depth), registry_(registry) {}
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        (void)input;
        // Deny if at max recursion depth
        if (current_depth_ >= config_.max_depth) return false;
        return true;
    }
    
    /// Check if a specific tool is allowed for this sub-agent
    [[nodiscard]] bool is_tool_allowed(std::string_view tool_name) const {
        // Check denied list first (takes precedence)
        for (const auto& denied : config_.denied_tools) {
            if (denied == tool_name) return false;
        }
        // If allowed list is specified, tool must be in it
        if (!config_.allowed_tools.empty()) {
            for (const auto& allowed : config_.allowed_tools) {
                if (allowed == tool_name) return true;
            }
            return false;
        }
        return true;  // No restrictions = allow all
    }
    
    /// Get config for creating child agents (depth incremented)
    [[nodiscard]] AgentConfig child_config() const {
        AgentConfig child = config_;
        // Children inherit tool restrictions
        return child;
    }
    
    [[nodiscard]] int current_depth() const { return current_depth_; }
    [[nodiscard]] int max_depth() const { return config_.max_depth; }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto request = parse_agent_tool_request(input);
        if (!request) return ToolResult::error(std::format("Invalid Agent input: {}", request.error()));

        if (request->prompt.empty()) {
            return ToolResult::error("Missing required 'prompt' field");
        }

        if (!request->unsupported_fields.empty()) {
            return ToolResult::error(std::format(
                "Agent parameters not yet supported by the native runtime: {}",
                join_fields(request->unsupported_fields)));
        }

        auto plan = build_agent_execution_plan(*request, config_);
        if (!plan) return ToolResult::error(plan.error());
        
        // Check recursion depth
        if (current_depth_ >= config_.max_depth) {
            return ToolResult::error(std::format(
                "Agent recursion depth limit reached ({}/{})", 
                current_depth_, config_.max_depth));
        }
        
        // Run the sub-agent loop
        return run_agent_loop(*plan);
    }

private:
    [[nodiscard]] bool plan_allows_generic_mcp_tool(const AgentExecutionPlan& plan) const {
        return !plan.agent_mcp_tools.empty() &&
            is_tool_allowed("mcp") &&
            !tool_name_disallowed_by_definition("mcp", plan.disallowed_tools);
    }

    [[nodiscard]] bool is_tool_allowed_for_plan(
        std::string_view tool_name,
        const AgentExecutionPlan& plan
    ) const {
        if (tool_name == "mcp" && plan_allows_generic_mcp_tool(plan)) {
            return true;
        }
        return is_tool_allowed(tool_name) &&
            tool_name_allowed_by_definition(tool_name, plan.allowed_tools) &&
            !tool_name_disallowed_by_definition(tool_name, plan.disallowed_tools);
    }

    [[nodiscard]] std::optional<std::string> mcp_scope_error_for_plan(
        const ToolInput& input,
        const AgentExecutionPlan& plan
    ) const {
        if (plan.agent_mcp_servers.empty()) return std::nullopt;

        auto doc = cc::utils::json::parse(input.json());
        if (!doc || !doc->root().is_obj()) {
            return "MCP input must be a JSON object in this sub-agent context";
        }

        auto root = doc->root();
        auto server = json_string(root, "server_name").or_else([&] { return json_string(root, "server"); });
        if (!server || server->empty()) {
            return "MCP server_name is required in this sub-agent context";
        }

        for (const auto& allowed : plan.agent_mcp_servers) {
            if (*server == allowed) return std::nullopt;
        }
        return std::format(
            "MCP server '{}' is not available in this sub-agent context. Available servers: {}",
            *server,
            join_fields(plan.agent_mcp_servers));
    }

    [[nodiscard]] std::vector<cc::services::api::ToolDefinition> api_tools_for_plan(
        const AgentExecutionPlan& plan
    ) const {
        std::vector<cc::services::api::ToolDefinition> tools;
        if (!registry_) return tools;

        for (const auto& definition : registry_->get_visible_definitions()) {
            if (!is_tool_allowed_for_plan(definition.name, plan)) continue;
            tools.push_back(cc::services::api::ToolDefinition{
                .name = definition.name,
                .description = definition.description,
                .input_schema_json = definition.input_schema.to_json(),
                .defer_load = false,
            });
        }
        return tools;
    }

    /// Run the sub-agent's recursive API loop
    [[nodiscard]] Result<ToolResult> run_agent_loop(const AgentExecutionPlan& plan) {
        
        auto client = get_default_client();
        
        // Build initial messages
        std::vector<Message> messages;
        for (const auto& skill_message : plan.preloaded_skill_messages) {
            messages.push_back(Message::from_text("user", skill_message));
        }
        if (plan.agent_mcp_context_message) {
            messages.push_back(Message::from_text("user", *plan.agent_mcp_context_message));
        }
        messages.push_back(Message::from_text("user", plan.prompt));
        
        std::string final_output;
        
        for (int turn = 0; turn < plan.max_turns; ++turn) {
            // Build request
            CreateMessageRequest req;
            req.model = plan.model;
            req.messages = messages;
            req.max_tokens = 16384;
            req.stream = true;
            if (!plan.system_prompt.empty()) req.system_prompt = plan.system_prompt;
            req.tools = api_tools_for_plan(plan);
            
            // Perform streaming request
            auto stream_result = client.create_message_stream(req);
            if (!stream_result) {
                return ToolResult::error(std::format(
                    "Agent API call failed: {}", stream_result.error().message()));
            }
            
            auto& parser = *stream_result;
            
            // Consume the stream and accumulate content blocks
            std::string text_content;
            std::vector<ContentBlock> tool_uses;
            std::string stop_reason;
            ContentBlock current_block;
            std::string accumulated_json;
            bool in_block = false;
            
            while (true) {
                auto event_result = parser.next_event();
                if (!event_result) break;
                if (!event_result->has_value()) {
                    if (parser.is_finished()) break;
                    // Brief wait for producer
                    continue;
                }
                
                const auto& event = **event_result;
                
                switch (event.type) {
                    case StreamEventType::ContentBlockStart:
                        in_block = true;
                        current_block = ContentBlock{};
                        if (event.block_type == StreamContentBlockType::Text) {
                            current_block.type = ContentBlockType::Text;
                        } else if (event.block_type == StreamContentBlockType::ToolUse) {
                            current_block.type = ContentBlockType::ToolUse;
                            current_block.tool_use_id = event.tool_use_id;
                            current_block.tool_name = event.tool_name;
                            accumulated_json.clear();
                        }
                        break;
                        
                    case StreamEventType::ContentBlockDelta:
                        if (event.delta.type == StreamContentBlockType::Text) {
                            current_block.text += event.delta.text;
                        } else if (event.delta.type == StreamContentBlockType::ToolUse) {
                            accumulated_json += event.delta.partial_json;
                        }
                        break;
                        
                    case StreamEventType::ContentBlockStop:
                        if (in_block) {
                            if (current_block.type == ContentBlockType::Text) {
                                text_content += current_block.text;
                            } else if (current_block.type == ContentBlockType::ToolUse) {
                                current_block.tool_input_json = accumulated_json;
                                tool_uses.push_back(current_block);
                            }
                            in_block = false;
                        }
                        break;
                        
                    case StreamEventType::MessageDelta:
                        if (event.message_delta.stop_reason) {
                            stop_reason = *event.message_delta.stop_reason;
                        }
                        break;
                        
                    case StreamEventType::MessageStop:
                        goto stream_done;
                        
                    case StreamEventType::Error:
                        return ToolResult::error(std::format(
                            "Agent stream error: {}", event.error.error_message));
                        
                    default:
                        break;
                }
            }
            stream_done:
            
            // If stop_reason is "end_turn" and no tool_use, we're done
            if (stop_reason == "end_turn" || tool_uses.empty()) {
                final_output = text_content;
                break;
            }
            
            // Add assistant message to history
            Message assistant_msg;
            assistant_msg.role = "assistant";
            if (!text_content.empty()) {
                assistant_msg.content.push_back(ContentBlock{
                    .type = ContentBlockType::Text,
                    .text = text_content
                });
            }
            for (auto& tu : tool_uses) {
                assistant_msg.content.push_back(tu);
            }
            messages.push_back(std::move(assistant_msg));
            
            // Execute tools and add results
            Message tool_result_msg;
            tool_result_msg.role = "user";
            for (const auto& tu : tool_uses) {
                ContentBlock result_block;
                result_block.type = ContentBlockType::ToolResult;
                result_block.tool_use_id = tu.tool_use_id;
                
                if (!registry_) {
                    result_block.text = "[Tool execution not available: no registry]";
                } else if (!is_tool_allowed_for_plan(tu.tool_name, plan)) {
                    result_block.text = std::format(
                        "[Tool '{}' not available in sub-agent context]", tu.tool_name);
                } else if (tu.tool_name == "Agent" && current_depth_ + 1 >= config_.max_depth) {
                    result_block.text = std::format(
                        "[Agent recursion depth limit reached ({}/{})]",
                        current_depth_ + 1, config_.max_depth);
                } else {
                    auto tool_input = ToolInput::from_json(tu.tool_input_json);
                    if (tu.tool_name == "mcp") {
                        if (auto scope_error = mcp_scope_error_for_plan(tool_input, plan)) {
                            result_block.text = std::format("[Tool execution error: {}]", *scope_error);
                            tool_result_msg.content.push_back(std::move(result_block));
                            continue;
                        }
                    }
                    auto exec_result = registry_->execute(tu.tool_name, tool_input);
                    if (exec_result) {
                        // Concatenate all content blocks from the tool result
                        std::string output;
                        for (const auto& c : exec_result->content) {
                            if (!output.empty()) output += "\n";
                            output += c.text;
                        }
                        result_block.text = std::move(output);
                    } else {
                        result_block.text = std::format(
                            "[Tool execution error: {}]", exec_result.error().message);
                    }
                }
                
                tool_result_msg.content.push_back(std::move(result_block));
            }
            messages.push_back(std::move(tool_result_msg));
            
            // Record partial output
            if (!text_content.empty()) {
                final_output += text_content + "\n";
            }
        }
        
        if (final_output.empty()) {
            final_output = "[Agent completed without producing output]";
        }
        
        return ToolResult::success(final_output);
    }
    
    AgentConfig config_;
    int current_depth_ = 0;
    cc::core::ToolRegistry* registry_ = nullptr;
};

} // namespace cc::tools::agent

// Export main tool class
export namespace cc::tools {
    using cc::tools::agent::AgentTool;
    using cc::tools::agent::AgentConfig;

    /// Factory: create AgentTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_agent_tool(AgentConfig config = {},
                                        int depth = 0,
                                        cc::core::ToolRegistry* registry = nullptr)
        -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            AgentTool tool_;
            cc::core::ToolDefinition def_ = AgentTool::definition();

            explicit Adapter(AgentConfig cfg, int d, cc::core::ToolRegistry* reg)
                : tool_(std::move(cfg), d, reg) {}

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>(std::move(config), depth, registry);
    }
}
