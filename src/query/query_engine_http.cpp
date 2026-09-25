// Implementation unit for cc.query.query_engine — the direct httplib HTTP
// path: non-streaming send_request, SSE stream_single_api_call, response
// parsing, SSE framing (SseEventDecoder::feed), and the free
// api_messages_endpoint helper. This is the ONLY implementation unit that
// textually includes <httplib.h>; keeping it here lets the third-party
// header leave the module interface BMI. Raw httplib types are NOT exported
// by cc.utils.http (it includes the same header in its own global module
// fragment and only exports cc::utils::HttpClient wrappers), so this TU
// textually includes <httplib.h> exactly like src/services/auth/*.cppm.
module;

#include <cstddef>
#include <cstdint>
#include <httplib.h>

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.utils.error;
import cc.utils.json;

namespace cc::core {

namespace {

// TU-local replacement for the former QueryEngine::add_beta_headers private
// member. Demoted to a free function so <httplib.h> never needs to name a
// type in the class definition; both call sites (send_request and
// stream_single_api_call) live in this same TU. The booleans are computed
// by the callers (which have private access) instead of reading config_.
void add_beta_headers(httplib::Headers& headers,
                      bool has_task_budget,
                      bool has_context_management) {
    if (has_task_budget) {
        headers.emplace("anthropic-beta", "task-budgets-2026-03-13");
    }
    if (has_context_management) {
        headers.emplace("anthropic-beta", "context-management-2025-06-27");
    }
}

} // namespace

[[nodiscard]] std::expected<ApiMessagesEndpoint, std::string> api_messages_endpoint(
    std::string_view base_url
) {
    const auto scheme_end = base_url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected("API base URL must include http:// or https://");
    }

    const auto authority_start = scheme_end + 3;
    const auto path_start = base_url.find('/', authority_start);
    std::string client_base_url = path_start == std::string_view::npos
        ? std::string(base_url)
        : std::string(base_url.substr(0, path_start));
    std::string path_prefix = path_start == std::string_view::npos
        ? std::string{}
        : std::string(base_url.substr(path_start));

    while (client_base_url.size() > scheme_end + 3 && client_base_url.ends_with('/')) {
        client_base_url.pop_back();
    }
    while (!path_prefix.empty() && path_prefix.ends_with('/')) {
        path_prefix.pop_back();
    }
    if (path_prefix == "/") path_prefix.clear();

    if (client_base_url.size() <= scheme_end + 3) {
        return std::unexpected("API base URL host cannot be empty");
    }

    std::string path = path_prefix;
    path += path.ends_with("/v1") ? "/messages" : "/v1/messages";
    return ApiMessagesEndpoint{
        .client_base_url = std::move(client_base_url),
        .path = std::move(path),
    };
}

[[nodiscard]] std::vector<SseEvent> SseEventDecoder::feed(std::string_view chunk) {
    std::vector<SseEvent> out;
    buffer_.append(chunk.data(), chunk.size());
    while (true) {
        const auto double_nl = buffer_.find("\n\n");
        if (double_nl == std::string::npos) break;

        std::string event_block = buffer_.substr(0, double_nl);
        buffer_.erase(0, double_nl + 2);

        std::string event_data;
        std::size_t pos = 0;
        while (pos < event_block.size()) {
            const auto nl = event_block.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) {
                line = event_block.substr(pos);
                pos = event_block.size();
            } else {
                line = event_block.substr(pos, nl - pos);
                pos = nl + 1;
            }
            if (line.starts_with("event: ")) {
                current_event_type_ = line.substr(7);
            } else if (line.starts_with("data: ")) {
                if (!event_data.empty()) event_data += '\n';
                event_data += line.substr(6);
            } else if (line == "data:") {
                if (!event_data.empty()) event_data += '\n';
            }
        }
        if (!event_data.empty()) {
            out.push_back({current_event_type_, std::move(event_data)});
        }
    }
    return out;
}

[[nodiscard]] Result<QueryEngine::ApiCallResult> QueryEngine::send_request(
    const QueryOptions& options,
    bool is_retry_after_compact) {
    replay_snip_boundaries();
    apply_time_based_microcompact();
    apply_tool_result_budget();

    auto endpoint = api_messages_endpoint(api_config_.base_url);
    if (!endpoint) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidRequest,
            endpoint.error()));
    }

    // Build headers
    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("anthropic-version", api_config_.api_version);
    if (!api_config_.auth_token.empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", api_config_.auth_token));
    } else {
        headers.emplace("x-api-key", api_config_.api_key);
    }
    headers.emplace("User-Agent", "LOOM/1.0");
    add_beta_headers(headers, config_.task_budget.has_value(),
                     api_context_management().has_value());

    // Build request body
    std::string body = build_request_body(options);

    // Create HTTP client
    httplib::Client cli(endpoint->client_base_url);
    cli.set_connection_timeout(api_config_.timeout);
    cli.set_read_timeout(api_config_.timeout);
    cli.set_write_timeout(api_config_.timeout);

    // Send request
    auto res = cli.Post(endpoint->path, headers, body, "application/json");

    if (!res) {
        auto err = res.error();
        return std::unexpected(Error::make(
            ErrorCode::ConnectionFailed,
            std::format("HTTP request failed: {}", httplib::to_string(err))));
    }

    // Check for HTTP errors
    if (res->status >= 400) {
        // P1-2: Reactive compact on 413 / prompt-too-long
        if (!is_retry_after_compact &&
            (res->status == 413 ||
             res->body.find("prompt_too_long") != std::string::npos ||
             res->body.find("Prompt is too long") != std::string::npos)) {
            auto compact_result = compact_conversation("reactive");
            if (compact_result) {
                // Retry once after compaction
                return send_request(options, /*is_retry_after_compact=*/true);
            }
        }

        return std::unexpected(Error::make(
            classify_http_error(res->status),
            std::format("API error ({}): {}", res->status, res->body)));
    }

    // Parse response
    return parse_api_response(res->body);
}

[[nodiscard]] ErrorCode QueryEngine::classify_http_error(int status) const {
    if (status == 401 || status == 403) {
        return ErrorCode::AuthenticationFailed;
    } else if (status == 429) {
        return ErrorCode::RateLimited;
    } else if (status == 529) {
        return ErrorCode::OverloadedError;
    } else if (status >= 400 && status < 500) {
        return ErrorCode::InvalidRequest;
    } else if (status >= 500) {
        return ErrorCode::InternalError;
    }
    return ErrorCode::InternalError;
}

[[nodiscard]] Result<QueryEngine::ApiCallResult> QueryEngine::parse_api_response(std::string_view response_body) {
    auto doc_result = cc::utils::json::parse(response_body);
    if (!doc_result) {
        return std::unexpected(Error::make(
            ErrorCode::InvalidRequest,
            "Failed to parse API response"));
    }

    auto root = doc_result->root();

    ApiCallResult result;

    // Build assistant message
    result.message.id.value = std::string(root.get("id").as_str());
    result.message.model = std::string(root.get("model").as_str());
    result.message.timestamp = std::chrono::system_clock::now();

    // Parse stop reason
    auto stop_reason = root.get("stop_reason");
    if (stop_reason.valid() && !stop_reason.is_null()) {
        result.message.stop_reason = std::string(stop_reason.as_str());
    }

    // Parse content
    auto content = root.get("content");
    if (content.valid() && content.is_arr()) {
        content.iter([&](cc::utils::json::JsonVal block) {
            parse_content_block(block, result.message.content);
        });
    }

    // Parse usage
    auto usage = root.get("usage");
    if (usage.valid() && usage.is_obj()) {
        result.usage.input_tokens = static_cast<std::uint32_t>(usage.get("input_tokens").as_int());
        result.usage.output_tokens = static_cast<std::uint32_t>(usage.get("output_tokens").as_int());
        auto cache_creation = usage.get("cache_creation_input_tokens");
        if (cache_creation.valid()) {
            result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
        }
        auto cache_read = usage.get("cache_read_input_tokens");
        if (cache_read.valid()) {
            result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
        }
    }

    return result;
}

void QueryEngine::parse_content_block(cc::utils::json::JsonVal block,
                                      std::vector<ContentBlock>& content) const {
    auto type = block.get("type").as_str();

    if (type == "text") {
        TextBlock tb;
        tb.text = std::string(block.get("text").as_str());
        content.push_back(std::move(tb));
    } else if (type == "tool_use") {
        ToolUseBlock tub;
        tub.id.value = std::string(block.get("id").as_str());
        tub.name = std::string(block.get("name").as_str());
        auto input = block.get("input");
        tub.input_json = input.valid() ? cc::utils::json::to_string(input) : "{}";
        content.push_back(std::move(tub));
    } else if (type == "thinking") {
        ThinkingBlock tb;
        tb.thinking = std::string(block.get("thinking").as_str());
        auto signature = block.get("signature");
        if (signature.valid()) {
            tb.signature = std::string(signature.as_str());
        }
        content.push_back(std::move(tb));
    }
}

[[nodiscard]] QueryEngine::StreamCallResult QueryEngine::stream_single_api_call(const QueryOptions& options) {
    StreamCallResult result;
    replay_snip_boundaries();
    apply_time_based_microcompact();
    apply_tool_result_budget();

    // Build headers
    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("anthropic-version", api_config_.api_version);
    if (!api_config_.auth_token.empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", api_config_.auth_token));
    } else {
        headers.emplace("x-api-key", api_config_.api_key);
    }
    headers.emplace("User-Agent", "LOOM/1.0");
    add_beta_headers(headers, config_.task_budget.has_value(),
                     api_context_management().has_value());

    // Build streaming request body
    std::string body = build_request_body(options, /*stream=*/true);

    // ── Dump Prompts: log API request ──
    if (dump_prompts_dir_) {
        auto dp = dump_prompts_path();
        if (dp) {
            std::ofstream ofs(*dp, std::ios::app);
            if (ofs.is_open()) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                ofs << "{\"type\":\"request\",\"timestamp\":" << now_ms
                    << ",\"body\":" << body << "}\n";
            }
        }
    }

    auto endpoint = api_messages_endpoint(api_config_.base_url);
    if (!endpoint) {
        result.failed = true;
        result.error_message = endpoint.error();
        if (options.on_event) {
            StreamError ev;
            ev.error_type = "invalid_request_error";
            ev.message = result.error_message;
            (*options.on_event)(ev);
        }
        return result;
    }

    // Create HTTP client
    httplib::Client cli(endpoint->client_base_url);
    cli.set_connection_timeout(api_config_.timeout);
    cli.set_read_timeout(api_config_.timeout);
    cli.set_write_timeout(api_config_.timeout);

    // SSE parsing state
    SseEventDecoder sse_decoder_;
    std::uint32_t block_index = 0;

    // Content accumulation
    struct BlockAccum {
        ContentBlock block;
        std::string accumulated_text;
        std::string accumulated_json;
    };
    std::vector<BlockAccum> blocks;

    auto emit_stream_error = [&](std::string error_type, std::string message) {
        result.failed = true;
        result.error_message = message.empty() ? error_type : message;
        if (options.on_event) {
            StreamError ev;
            ev.error_type = std::move(error_type);
            ev.message = result.error_message;
            (*options.on_event)(ev);
        }
    };

    auto parse_sse_event = [&](const std::string& event_type,
                                const std::string& data) {
        if (data.empty() || data == "[DONE]") return;

        auto doc_result = cc::utils::json::parse(data);
        if (!doc_result) return;
        auto root = doc_result->root();

        if (event_type == "message_start") {
            auto msg = root.get("message");
            if (msg.valid()) {
                auto id = msg.get("id");
                if (id.valid()) result.message.id.value = std::string(id.as_str());
                auto model = msg.get("model");
                if (model.valid()) result.message.model = std::string(model.as_str());
                auto usage = msg.get("usage");
                if (usage.valid() && usage.is_obj()) {
                    auto input = usage.get("input_tokens");
                    if (input.valid()) {
                        result.usage.input_tokens = static_cast<std::uint32_t>(input.as_int());
                    }
                    auto cache_creation = usage.get("cache_creation_input_tokens");
                    if (cache_creation.valid()) {
                        result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                    }
                    auto cache_read = usage.get("cache_read_input_tokens");
                    if (cache_read.valid()) {
                        result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                    }
                }
            }
            if (options.on_event) {
                StreamStart ev;
                ev.message_id = result.message.id;
                ev.model = result.message.model.value_or(config_.model_params.model);
                (*options.on_event)(ev);
            }
        } else if (event_type == "content_block_start") {
            auto idx = root.get("index");
            block_index = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : static_cast<std::uint32_t>(blocks.size());
            auto cb = root.get("content_block");
            BlockAccum accum;
            if (cb.valid()) {
                auto type = cb.get("type");
                if (type.valid()) {
                    auto type_str = type.as_str();
                    if (type_str == "text") {
                        accum.block = TextBlock{};
                    } else if (type_str == "tool_use") {
                        ToolUseBlock tub;
                        auto id = cb.get("id");
                        if (id.valid()) tub.id.value = std::string(id.as_str());
                        auto name = cb.get("name");
                        if (name.valid()) tub.name = std::string(name.as_str());
                        accum.block = std::move(tub);
                    } else if (type_str == "thinking") {
                        accum.block = ThinkingBlock{};
                    }
                }
            }
            blocks.push_back(std::move(accum));
            if (options.on_event) {
                ContentBlockStart ev;
                ev.index = block_index;
                ev.block = blocks.back().block;
                (*options.on_event)(ev);
            }
        } else if (event_type == "content_block_delta") {
            auto idx = root.get("index");
            auto cur_idx = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : block_index;
            auto delta = root.get("delta");
            if (delta.valid() && cur_idx < blocks.size()) {
                auto type = delta.get("type");
                if (type.valid()) {
                    auto type_str = type.as_str();
                    std::string delta_text;
                    if (type_str == "text_delta") {
                        auto text = delta.get("text");
                        if (text.valid()) {
                            delta_text = std::string(text.as_str());
                            blocks[cur_idx].accumulated_text += delta_text;
                        }
                    } else if (type_str == "input_json_delta") {
                        auto pj = delta.get("partial_json");
                        if (pj.valid()) {
                            delta_text = std::string(pj.as_str());
                            blocks[cur_idx].accumulated_json += delta_text;
                        }
                    } else if (type_str == "thinking_delta") {
                        auto thinking = delta.get("thinking");
                        if (thinking.valid()) {
                            delta_text = std::string(thinking.as_str());
                            blocks[cur_idx].accumulated_text += delta_text;
                        }
                    }
                    if (!delta_text.empty() && options.on_event) {
                        ContentBlockDelta ev;
                        ev.index = cur_idx;
                        ev.delta_text = delta_text;
                        (*options.on_event)(ev);
                    }
                }
            }
        } else if (event_type == "content_block_stop") {
            auto idx = root.get("index");
            auto cur_idx = idx.valid() ? static_cast<std::uint32_t>(idx.as_int()) : block_index;
            // Finalize block content
            if (cur_idx < blocks.size()) {
                auto& accum = blocks[cur_idx];
                if (auto* tb = std::get_if<TextBlock>(&accum.block)) {
                    tb->text = std::move(accum.accumulated_text);
                } else if (auto* tub = std::get_if<ToolUseBlock>(&accum.block)) {
                    tub->input_json = accum.accumulated_json.empty() ? "{}" : std::move(accum.accumulated_json);
                } else if (auto* thk = std::get_if<ThinkingBlock>(&accum.block)) {
                    thk->thinking = std::move(accum.accumulated_text);
                }
            }
            if (options.on_event) {
                ContentBlockStop ev;
                ev.index = cur_idx;
                (*options.on_event)(ev);
            }
        } else if (event_type == "message_delta") {
            auto delta = root.get("delta");
            if (delta.valid()) {
                auto sr = delta.get("stop_reason");
                if (sr.valid() && !sr.is_null()) {
                    result.message.stop_reason = std::string(sr.as_str());
                }
            }
            auto usage = root.get("usage");
            if (usage.valid()) {
                auto ot = usage.get("output_tokens");
                if (ot.valid()) result.usage.output_tokens = static_cast<std::uint32_t>(ot.as_int());
                auto cache_creation = usage.get("cache_creation_input_tokens");
                if (cache_creation.valid()) {
                    result.usage.cache_creation_tokens = static_cast<std::uint32_t>(cache_creation.as_int());
                }
                auto cache_read = usage.get("cache_read_input_tokens");
                if (cache_read.valid()) {
                    result.usage.cache_read_tokens = static_cast<std::uint32_t>(cache_read.as_int());
                }
            }
        } else if (event_type == "message_stop") {
            // Stream completed successfully
        } else if (event_type == "error") {
            auto err = root.get("error");
            if (err.valid()) {
                std::string type_text = "stream_error";
                std::string message_text = "Streaming API error";
                auto type = err.get("type");
                if (type.valid()) type_text = std::string(type.as_str());
                auto msg = err.get("message");
                if (msg.valid()) message_text = std::string(msg.as_str());
                emit_stream_error(std::move(type_text), std::move(message_text));
            }
        }
    };

    // Track current SSE event type
    // (now held inside sse_decoder_)

    // Streaming POST using httplib's send() with content_receiver on Request
    httplib::Request req;
    req.method = "POST";
    req.path = endpoint->path;
    req.headers = headers;
    req.headers.emplace("Content-Type", "application/json");
    req.body = body;
    req.content_receiver = [&](const char* data, size_t len,
                               uint64_t /*offset*/, uint64_t /*total*/) -> bool {
        if (should_abort()) return false;

        // The decoder buffers partial chunks and yields complete events;
        // dispatch each to the domain handler (parse_sse_event).
        for (const auto& ev : sse_decoder_.feed(std::string_view(data, len))) {
            parse_sse_event(ev.type, ev.data);
        }

        return true;
    };

    httplib::Response res;
    httplib::Error err;
    bool ok = cli.send(req, res, err);

    if (!ok || (res.status >= 400 && blocks.empty())) {
        // If streaming failed and we got nothing, fall back to non-streaming
        auto fallback = send_request(options);
        if (fallback) {
            result.message = std::move(fallback->message);
            result.usage = fallback->usage;
        } else {
            emit_stream_error("api_error", fallback.error().format());
            return result;
        }
    } else if (result.failed && blocks.empty()) {
        return result;
    } else {
        // Assemble final message from accumulated blocks
        result.message.timestamp = std::chrono::system_clock::now();
        for (auto& accum : blocks) {
            result.message.content.push_back(std::move(accum.block));
        }
        // Parse input_tokens from response if available
        auto usage_hdr = res.get_header_value("x-usage-input-tokens");
        if (!usage_hdr.empty()) {
            try { result.usage.input_tokens = static_cast<std::uint32_t>(std::stoul(usage_hdr)); }
            catch (...) {}
        }
    }

    // Check for tool use
    for (const auto& block : result.message.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            result.has_tool_use = true;
            break;
        }
    }

    // Emit stream end
    if (!result.failed && options.on_event) {
        StreamEnd end_event;
        end_event.stop_reason = result.message.stop_reason;
        end_event.usage = result.usage;
        (*options.on_event)(end_event);
    }

    // ── Dump Prompts: log API response ──
    if (dump_prompts_dir_) {
        auto dp = dump_prompts_path();
        if (dp) {
            std::ofstream ofs(*dp, std::ios::app);
            if (ofs.is_open()) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                // Serialize the assembled response message as JSONL
                std::string resp_json = message_to_jsonl_(Message{result.message});
                ofs << "{\"type\":\"response\",\"timestamp\":" << now_ms
                    << ",\"stop_reason\":\""
                    << result.message.stop_reason.value_or("") << "\""
                    << ",\"has_tool_use\":" << (result.has_tool_use ? "true" : "false")
                    << ",\"failed\":" << (result.failed ? "true" : "false")
                    << ",\"message\":" << resp_json << "}\n";
            }
        }
    }

    return result;
}

} // namespace cc::core
