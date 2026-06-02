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

export module cc.tools.agent;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;
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
                    SchemaProperty{
                        .name = "task",
                        .type = "string",
                        .description = "The task for the agent to perform",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "skill",
                        .type = "string",
                        .description = "Specific agent type/skill to use (optional)",
                        .required = false
                    }
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
        // Parse input parameters
        std::string task;
        std::string skill;
        auto json_str = input.json();
        
        // Use simple JSON extraction (consistent with existing pattern)
        auto task_pos = json_str.find("\"task\"");
        if (task_pos != std::string::npos) {
            auto val_start = json_str.find(":", task_pos);
            auto quote_start = json_str.find("\"", val_start + 1);
            auto quote_end = json_str.find("\"", quote_start + 1);
            if (quote_start != std::string::npos && quote_end != std::string::npos) {
                task = std::string(json_str.substr(quote_start + 1, quote_end - quote_start - 1));
            }
        }
        auto skill_pos = json_str.find("\"skill\"");
        if (skill_pos != std::string::npos) {
            auto val_start = json_str.find(":", skill_pos);
            auto quote_start = json_str.find("\"", val_start + 1);
            auto quote_end = json_str.find("\"", quote_start + 1);
            if (quote_start != std::string::npos && quote_end != std::string::npos) {
                skill = std::string(json_str.substr(quote_start + 1, quote_end - quote_start - 1));
            }
        }
        
        if (task.empty()) {
            return ToolResult::error("Missing required 'task' field");
        }
        
        // Check recursion depth
        if (current_depth_ >= config_.max_depth) {
            return ToolResult::error(std::format(
                "Agent recursion depth limit reached ({}/{})", 
                current_depth_, config_.max_depth));
        }
        
        // Run the sub-agent loop
        return run_agent_loop(task, skill);
    }

private:
    /// Run the sub-agent's recursive API loop
    [[nodiscard]] Result<ToolResult> run_agent_loop(
        const std::string& task, const std::string& skill) {
        
        auto client = get_default_client();
        
        // Build initial messages
        std::vector<Message> messages;
        std::string user_prompt = task;
        if (!skill.empty()) {
            user_prompt = std::format("[Agent type: {}]\n\n{}", skill, task);
        }
        messages.push_back(Message::from_text("user", user_prompt));
        
        std::string final_output;
        
        for (int turn = 0; turn < config_.max_turns; ++turn) {
            // Build request
            CreateMessageRequest req;
            req.model = config_.default_model;
            req.messages = messages;
            req.max_tokens = 16384;
            req.stream = true;
            
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
                } else if (!is_tool_allowed(tu.tool_name)) {
                    result_block.text = std::format(
                        "[Tool '{}' not available in sub-agent context]", tu.tool_name);
                } else if (tu.tool_name == "Agent" && current_depth_ + 1 >= config_.max_depth) {
                    result_block.text = std::format(
                        "[Agent recursion depth limit reached ({}/{})]",
                        current_depth_ + 1, config_.max_depth);
                } else {
                    auto tool_input = ToolInput::from_json(tu.tool_input_json);
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
