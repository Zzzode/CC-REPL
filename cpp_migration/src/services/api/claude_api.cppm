// Claude API - High-level API wrapper with streaming support and content accumulation
module;
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module cc.services.claude_api;

import cc.services.api.client;
import cc.services.api.streaming;
import cc.services.api.models;
import cc.services.api.errors;
import cc.utils.error;

export namespace cc::services::claude_api {

using cc::services::api::AnthropicClient;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;
using cc::services::api::CreateMessageRequest;
using cc::services::api::CreateMessageResponse;
using cc::services::api::Message;
using cc::services::api::StreamContentBlockType;
using cc::services::api::StreamEvent;
using cc::services::api::StreamEventType;
using cc::services::api::StreamParser;
using cc::services::api::TokenUsage;
using cc::services::api::ToolDefinition;
using cc::services::api::CostCalculator;
using cc::services::api::ModelPricing;
using cc::utils::Result;

// =========================================================================
// Model Tiers
// =========================================================================

enum class ModelTier { Opus, Sonnet, Haiku };

// =========================================================================
// High-Level Message Types (simplified interface for callers)
// =========================================================================

struct APIMessage {
    std::string role;
    std::string content;
    std::optional<std::string> model;
};

struct APIRequest {
    std::string model;
    std::vector<APIMessage> messages;
    std::optional<std::uint32_t> max_tokens;
    std::optional<double> temperature;
    bool stream{true};
    std::optional<std::string> system_prompt;
    std::vector<ToolDefinition> tools;
    std::optional<int> thinking_budget;
};

struct APIResponse {
    std::string id;
    std::string content;
    std::string model;
    std::uint64_t input_tokens;
    std::uint64_t output_tokens;
    std::optional<std::string> stop_reason;
    std::vector<ContentBlock> content_blocks;
};

struct UsageInfo {
    std::uint64_t total_input_tokens{0};
    std::uint64_t total_output_tokens{0};
    std::optional<double> cost_usd;
};

// =========================================================================
// Stream Event Callback
// =========================================================================

/// Callback invoked for each streaming event (text deltas, tool calls, etc.)
using StreamCallback = std::function<void(const StreamEvent&)>;

// =========================================================================
// Core API Functions
// =========================================================================

/// Send a request using the provided client, consuming the stream and accumulating the response.
/// The optional `on_event` callback is invoked for each SSE event (for real-time rendering).
inline std::expected<APIResponse, std::string> send_request(
    AnthropicClient& client,
    const APIRequest& request,
    StreamCallback on_event = nullptr) {

    // Build low-level request
    CreateMessageRequest low_req;
    low_req.model = request.model;
    low_req.max_tokens = request.max_tokens.value_or(16384);
    low_req.stream = request.stream;
    low_req.temperature = request.temperature ? std::optional<double>(*request.temperature) : std::nullopt;
    low_req.system_prompt = request.system_prompt;
    low_req.tools = request.tools;
    low_req.thinking_budget = request.thinking_budget;

    for (const auto& msg : request.messages) {
        low_req.messages.push_back(Message::from_text(msg.role, msg.content));
    }

    if (!request.stream) {
        // Non-streaming path
        auto result = client.create_message(low_req);
        if (!result) {
            return std::unexpected(result.error().message());
        }
        APIResponse resp;
        resp.id = result->id;
        resp.model = result->model;
        resp.content = result->get_text_content();
        resp.input_tokens = static_cast<uint64_t>(result->usage.input_tokens);
        resp.output_tokens = static_cast<uint64_t>(result->usage.output_tokens);
        resp.stop_reason = result->stop_reason;
        resp.content_blocks = result->content;
        return resp;
    }

    // Streaming path
    auto stream_result = client.create_message_stream(low_req);
    if (!stream_result) {
        return std::unexpected(stream_result.error().message());
    }

    auto& parser = *stream_result;
    APIResponse response;
    std::string accumulated_text;
    std::string accumulated_tool_json;
    ContentBlock current_block;
    bool in_block = false;

    // Consume all events
    while (true) {
        auto event_result = parser.next_event();
        if (!event_result) {
            return std::unexpected(event_result.error().message());
        }
        if (!event_result->has_value()) {
            // No more data available yet — check if stream finished
            if (parser.is_finished()) break;
            // Busy-wait briefly for more data (producer thread is feeding)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const auto& event = **event_result;
        if (on_event) on_event(event);

        switch (event.type) {
            case StreamEventType::MessageStart:
                response.id = event.message_id;
                response.model = event.model;
                break;

            case StreamEventType::ContentBlockStart:
                in_block = true;
                current_block = ContentBlock{};
                if (event.block_type == StreamContentBlockType::Text) {
                    current_block.type = ContentBlockType::Text;
                    current_block.text = event.block_text;
                } else if (event.block_type == StreamContentBlockType::ToolUse) {
                    current_block.type = ContentBlockType::ToolUse;
                    current_block.tool_use_id = event.tool_use_id;
                    current_block.tool_name = event.tool_name;
                    accumulated_tool_json.clear();
                } else if (event.block_type == StreamContentBlockType::Thinking) {
                    current_block.type = ContentBlockType::Thinking;
                }
                break;

            case StreamEventType::ContentBlockDelta:
                if (event.delta.type == StreamContentBlockType::Text) {
                    current_block.text += event.delta.text;
                    accumulated_text += event.delta.text;
                } else if (event.delta.type == StreamContentBlockType::ToolUse) {
                    accumulated_tool_json += event.delta.partial_json;
                } else if (event.delta.type == StreamContentBlockType::Thinking) {
                    current_block.thinking += event.delta.thinking;
                }
                break;

            case StreamEventType::ContentBlockStop:
                if (in_block) {
                    if (current_block.type == ContentBlockType::ToolUse) {
                        current_block.tool_input_json = accumulated_tool_json;
                    }
                    response.content_blocks.push_back(std::move(current_block));
                    in_block = false;
                }
                break;

            case StreamEventType::MessageDelta:
                if (event.message_delta.stop_reason) {
                    response.stop_reason = *event.message_delta.stop_reason;
                }
                if (event.message_delta.usage.output_tokens) {
                    response.output_tokens = static_cast<uint64_t>(*event.message_delta.usage.output_tokens);
                }
                break;

            case StreamEventType::MessageStop:
                goto done;

            case StreamEventType::Error:
                return std::unexpected(event.error.error_message);

            default:
                break;
        }
    }
done:
    response.content = accumulated_text;
    response.input_tokens = static_cast<uint64_t>(parser.stream_state().input_tokens);
    if (response.output_tokens == 0) {
        response.output_tokens = static_cast<uint64_t>(parser.stream_state().output_tokens);
    }
    return response;
}

/// Overload: send_request using default global client (for backward compat)
inline std::expected<APIResponse, std::string> send_request(const APIRequest& request) {
    // Create a one-shot client from environment
    AnthropicClient::Config cfg;
    if (auto* key = std::getenv("ANTHROPIC_API_KEY"); key && key[0] != '\0') {
        cfg.api_key = key;
    }
    AnthropicClient client(cfg);
    return send_request(client, request);
}

// =========================================================================
// Utility Functions
// =========================================================================

inline std::expected<void, std::string> dump_prompts(std::string_view session_id,
                                                     std::string_view output_dir) {
    if (session_id.empty() || output_dir.empty()) {
        return std::unexpected("session_id and output_dir are required");
    }
    std::filesystem::path dir(output_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("Failed to create output directory: " + ec.message());
    }
    auto file_path = dir / (std::string(session_id) + "_prompts.json");
    std::ofstream file(file_path);
    if (!file) {
        return std::unexpected("Failed to open prompt dump file");
    }
    file << "{\"session_id\": \"" << session_id << "\", \"prompts\": []}\n";
    return {};
}

inline UsageInfo get_empty_usage() {
    return {0, 0, std::nullopt};
}

inline std::optional<std::chrono::system_clock::time_point> get_first_token_date() {
    return std::nullopt;
}

inline bool detect_prompt_cache_break(const std::vector<APIMessage>& messages) {
    // Detection heuristic: if any message has content > 10k chars, cache likely breaks
    for (const auto& msg : messages) {
        if (msg.content.size() > 10000) return true;
    }
    return false;
}

inline std::expected<std::uint32_t, std::string> get_overage_credit_grant() {
    return 0;
}

inline std::expected<std::uint32_t, std::string> get_ultrareview_quota() {
    return 0;
}

} // namespace cc::services::claude_api
