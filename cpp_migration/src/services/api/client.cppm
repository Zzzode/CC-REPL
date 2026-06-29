// Anthropic API Client - Complete implementation with async, streaming, retry
module;
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <curl/curl.h>

export module cc.services.api.client;

import cc.services.api.streaming;
import cc.services.api.models;
import cc.services.api.errors;
import cc.services.auth.provider_selector;
import cc.utils.json;
import cc.utils.error;

export namespace cc::services::api {

using cc::services::api::errors::ApiErrorDetails;
using cc::services::api::errors::ErrorClassifier;
using cc::services::api::errors::ErrorFactory;
using cc::services::api::errors::RetryContext;
using cc::services::api::errors::RetryDecision;
using cc::utils::Result;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonMutVal;
using cc::utils::json::JsonVal;

// =========================================================================
// Content Block Types
// =========================================================================

enum class ContentBlockType {
    Text,
    Image,
    ToolUse,
    ToolResult,
    Thinking,
    RedactedThinking,
    Document
};

struct ContentBlock {
    ContentBlockType type = ContentBlockType::Text;
    std::string text{};

    // Tool use specific
    std::string tool_use_id{};
    std::string tool_name{};
    std::string tool_input_json{};

    // Image specific
    std::string media_type{};
    std::string image_data{};

    // Thinking specific
    std::string thinking{};
    std::string signature{};
};

// =========================================================================
// Message Types
// =========================================================================

struct Message {
    std::string role{};  // "user" or "assistant"
    std::vector<ContentBlock> content{};

    [[nodiscard]] static Message from_text(std::string_view role,
                                           std::string_view text) {
        Message msg;
        msg.role = std::string(role);
        ContentBlock block;
        block.type = ContentBlockType::Text;
        block.text = std::string(text);
        msg.content.push_back(std::move(block));
        return msg;
    }
};

// =========================================================================
// Tool Definition
// =========================================================================

struct ToolDefinition {
    std::string name;
    std::string description;
    std::string input_schema_json;
    bool defer_load = false;
};

struct TaskBudget {
    int total = 0;
    std::optional<int> remaining;
};

// =========================================================================
// API Request
// =========================================================================

struct CreateMessageRequest {
    std::string model;
    std::vector<Message> messages;
    int max_tokens = 4096;
    std::optional<std::string> system_prompt;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<double> top_k;
    std::vector<std::string> stop_sequences;
    bool stream = false;
    std::vector<ToolDefinition> tools;
    std::optional<std::string> tool_choice;
    std::optional<int> thinking_budget;
    std::optional<std::string> output_effort;
    std::optional<TaskBudget> task_budget;
    std::optional<int> internal_effort_override;
    std::vector<std::string> betas;
    std::optional<std::string> metadata_user_id;
};

// =========================================================================
// Token Usage
// =========================================================================

struct TokenUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_creation_tokens = 0;
    int cache_read_tokens = 0;

    [[nodiscard]] int total() const {
        return input_tokens + output_tokens;
    }

    [[nodiscard]] int total_with_cache() const {
        return input_tokens + output_tokens + cache_creation_tokens + cache_read_tokens;
    }
};

// =========================================================================
// API Response
// =========================================================================

struct CreateMessageResponse {
    std::string id;
    std::string model;
    std::string role;
    std::vector<ContentBlock> content;
    std::string stop_reason;
    std::optional<std::string> stop_sequence;
    TokenUsage usage;

    [[nodiscard]] std::string get_text_content() const {
        std::string result;
        for (const auto& block : content) {
            if (block.type == ContentBlockType::Text) {
                result += block.text;
            }
        }
        return result;
    }
};

// =========================================================================
// CURL Handle RAII Wrapper
// =========================================================================

class CurlHandle {
public:
    // Factory: CURL init failure is reported via Result instead of throwing,
    // so callers that already return Result<T> (post/stream) stay consistent
    // and a failure inside the streaming worker thread cannot terminate.
    [[nodiscard]] static Result<CurlHandle> create() {
        void* h = curl_easy_init();
        if (!h) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::internal_error,
                "Failed to initialize CURL"));
        }
        CurlHandle handle;
        handle.handle_ = h;
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
        return handle;
    }

    ~CurlHandle() {
        if (handle_) curl_easy_cleanup(handle_);
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CurlHandle(CurlHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    CurlHandle& operator=(CurlHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) curl_easy_cleanup(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] void* get() const { return handle_; }

    template<typename T>
    void setopt(CURLoption option, T value) {
        curl_easy_setopt(handle_, option, value);
    }

private:
    CurlHandle() = default;
    void* handle_ = nullptr;
};

[[nodiscard]] bool is_loopback_url(std::string_view url) {
    return url.starts_with("http://127.0.0.1") ||
           url.starts_with("https://127.0.0.1") ||
           url.starts_with("http://localhost") ||
           url.starts_with("https://localhost") ||
           url.starts_with("http://[::1]") ||
           url.starts_with("https://[::1]");
}

void apply_loopback_no_proxy(CurlHandle& curl, std::string_view url) {
    if (!is_loopback_url(url)) return;
    curl.setopt(CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
}

// =========================================================================
// HTTP Response
// =========================================================================

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    [[nodiscard]] std::optional<std::string> get_header(std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key == name) return value;
        }
        return std::nullopt;
    }
};

// =========================================================================
// Rate Limiter
// =========================================================================

class RateLimiter {
public:
    explicit RateLimiter(int requests_per_minute = 60)
        : max_tokens_(requests_per_minute)
        , available_tokens_(requests_per_minute)
        , last_refill_(std::chrono::steady_clock::now()) {}

    [[nodiscard]] bool try_acquire() {
        refill();
        if (available_tokens_ > 0) {
            --available_tokens_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::chrono::milliseconds wait_time() const {
        if (available_tokens_ > 0) return std::chrono::milliseconds{0};
        auto elapsed = std::chrono::steady_clock::now() - last_refill_;
        auto refill_interval = std::chrono::minutes{1} / max_tokens_;
        auto remaining = refill_interval - elapsed;
        return std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    }

    void set_rate(int requests_per_minute) {
        max_tokens_ = requests_per_minute;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refill_);
        auto tokens_to_add = elapsed.count() * max_tokens_ / 60000;
        if (tokens_to_add > 0) {
            available_tokens_ = std::min(max_tokens_, available_tokens_ + static_cast<int>(tokens_to_add));
            last_refill_ = now;
        }
    }

    int max_tokens_;
    int available_tokens_;
    std::chrono::steady_clock::time_point last_refill_;
};

// =========================================================================
// Request Serializer
// =========================================================================

class RequestSerializer {
public:
    [[nodiscard]] static std::string serialize(const CreateMessageRequest& request) {
        JsonMutDoc doc;
        auto root = doc.object();

        root.add("model", doc.string(request.model));
        root.add("max_tokens", doc.number(static_cast<int64_t>(request.max_tokens)));
        root.add("stream", doc.boolean(request.stream));

        // Messages
        auto messages_arr = doc.array();
        for (const auto& msg : request.messages) {
            auto msg_obj = doc.object();
            msg_obj.add("role", doc.string(msg.role));
            msg_obj.add("content", serialize_content(msg.content, doc));
            messages_arr.append(msg_obj);
        }
        root.add("messages", messages_arr);

        // Optional fields
        if (request.system_prompt) {
            root.add("system", doc.string(*request.system_prompt));
        }
        if (request.temperature) {
            root.add("temperature", doc.number(*request.temperature));
        }
        if (request.top_p) {
            root.add("top_p", doc.number(*request.top_p));
        }
        if (request.top_k) {
            root.add("top_k", doc.number(*request.top_k));
        }
        if (!request.stop_sequences.empty()) {
            auto stops = doc.array();
            for (const auto& seq : request.stop_sequences) {
                stops.append(doc.string(seq));
            }
            root.add("stop_sequences", stops);
        }
        if (!request.tools.empty()) {
            auto tools_arr = doc.array();
            for (const auto& tool : request.tools) {
                auto tool_obj = doc.object();
                tool_obj.add("name", doc.string(tool.name));
                tool_obj.add("description", doc.string(tool.description));

                // Parse input schema JSON
                auto schema_result = cc::utils::json::parse(tool.input_schema_json);
                if (schema_result) {
                    tool_obj.add("input_schema", doc.copy_val(schema_result->root()));
                } else {
                    tool_obj.add("input_schema", doc.object());
                }
                tools_arr.append(tool_obj);
            }
            root.add("tools", tools_arr);
        }
        if (request.tool_choice) {
            root.add("tool_choice", doc.string(*request.tool_choice));
        }
        if (request.thinking_budget) {
            auto thinking = doc.object();
            thinking.add("type", doc.string("enabled"));
            thinking.add("budget_tokens", doc.number(static_cast<int64_t>(*request.thinking_budget)));
            root.add("thinking", thinking);
        }
        if (request.output_effort || request.task_budget) {
            auto output_config = doc.object();
            if (request.output_effort) {
                output_config.add("effort", doc.string(*request.output_effort));
            }
            if (request.task_budget) {
                auto task_budget = doc.object();
                task_budget.add("type", doc.string("tokens"));
                task_budget.add("total", doc.number(static_cast<int64_t>(request.task_budget->total)));
                if (request.task_budget->remaining) {
                    task_budget.add(
                        "remaining",
                        doc.number(static_cast<int64_t>(*request.task_budget->remaining)));
                }
                output_config.add("task_budget", task_budget);
            }
            root.add("output_config", output_config);
        }
        if (request.internal_effort_override) {
            auto anthropic_internal = doc.object();
            anthropic_internal.add(
                "effort_override",
                doc.number(static_cast<int64_t>(*request.internal_effort_override)));
            root.add("anthropic_internal", anthropic_internal);
        }

        doc.set_root(root);
        return doc.to_string();
    }

private:
    [[nodiscard]] static JsonMutVal serialize_content(const std::vector<ContentBlock>& content,
                                                   JsonMutDoc& doc) {
        if (content.size() == 1 && content[0].type == ContentBlockType::Text) {
            return doc.string(content[0].text);
        }

        auto arr = doc.array();
        for (const auto& block : content) {
            auto obj = doc.object();
            switch (block.type) {
                case ContentBlockType::Text:
                    obj.add("type", doc.string("text"));
                    obj.add("text", doc.string(block.text));
                    break;
                case ContentBlockType::ToolUse:
                    obj.add("type", doc.string("tool_use"));
                    obj.add("id", doc.string(block.tool_use_id));
                    obj.add("name", doc.string(block.tool_name));
                    if (!block.tool_input_json.empty()) {
                        auto input_doc = cc::utils::json::parse(block.tool_input_json);
                        obj.add("input", input_doc ? doc.copy_val(input_doc->root()) : doc.object());
                    } else {
                        obj.add("input", doc.object());
                    }
                    break;
                case ContentBlockType::ToolResult:
                    obj.add("type", doc.string("tool_result"));
                    obj.add("tool_use_id", doc.string(block.tool_use_id));
                    obj.add("content", doc.string(block.text));
                    break;
                case ContentBlockType::Image: {
                    obj.add("type", doc.string("image"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(block.media_type));
                    source.add("data", doc.string(block.image_data));
                    obj.add("source", source);
                    break;
                }
                case ContentBlockType::Document: {
                    obj.add("type", doc.string("document"));
                    auto source = doc.object();
                    source.add("type", doc.string("base64"));
                    source.add("media_type", doc.string(block.media_type));
                    source.add("data", doc.string(block.image_data));
                    obj.add("source", source);
                    break;
                }
                default:
                    break;
            }
            arr.append(obj);
        }
        return arr;
    }
};

// =========================================================================
// Response Parser
// =========================================================================

class ResponseParser {
public:
    [[nodiscard]] static Result<CreateMessageResponse> parse(std::string_view json_str) {
        auto doc_result = cc::utils::json::parse(json_str);
        if (!doc_result) {
            return std::unexpected(doc_result.error());
        }

        CreateMessageResponse response;
        auto root = doc_result->root();

        response.id = std::string(root.get("id").as_str());
        response.model = std::string(root.get("model").as_str());
        response.role = std::string(root.get("role").as_str());
        response.stop_reason = std::string(root.get("stop_reason").as_str());

        // Parse content
        auto content = root.get("content");
        if (content.is_arr()) {
            content.iter([&response](JsonVal block) {
                response.content.push_back(parse_content_block(block));
            });
        }

        // Parse usage
        auto usage = root.get("usage");
        if (usage.is_obj()) {
            response.usage.input_tokens = static_cast<int>(usage.get("input_tokens").as_int());
            response.usage.output_tokens = static_cast<int>(usage.get("output_tokens").as_int());
            response.usage.cache_creation_tokens = static_cast<int>(usage.get("cache_creation_input_tokens").as_int());
            response.usage.cache_read_tokens = static_cast<int>(usage.get("cache_read_input_tokens").as_int());
        }

        return response;
    }

private:
    [[nodiscard]] static ContentBlock parse_content_block(JsonVal block) {
        ContentBlock result;
        auto type = block.get("type").as_str();

        if (type == "text") {
            result.type = ContentBlockType::Text;
            result.text = std::string(block.get("text").as_str());
        } else if (type == "tool_use") {
            result.type = ContentBlockType::ToolUse;
            result.tool_use_id = std::string(block.get("id").as_str());
            result.tool_name = std::string(block.get("name").as_str());
            auto input = block.get("input");
            result.tool_input_json = input.valid() ? cc::utils::json::to_string(input) : "{}";
        } else if (type == "thinking") {
            result.type = ContentBlockType::Thinking;
            result.thinking = std::string(block.get("thinking").as_str());
        }

        return result;
    }
};

// =========================================================================
// HTTP Client
// =========================================================================

class HttpClient {
public:
    [[nodiscard]] static Result<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers,
        std::chrono::milliseconds timeout) {

        auto curl_result = CurlHandle::create();
        if (!curl_result) return std::unexpected(curl_result.error());
        CurlHandle curl = std::move(*curl_result);
        HttpResponse response;
        std::string response_body;
        std::string response_headers;

        // Set URL
        curl.setopt(CURLOPT_URL, url.c_str());
        apply_loopback_no_proxy(curl, url);

        // Set POST data
        curl.setopt(CURLOPT_POSTFIELDS, body.c_str());
        curl.setopt(CURLOPT_POSTFIELDSIZE, body.size());

        // Set timeout
        curl.setopt(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));

        // Set headers
        struct curl_slist* header_list = nullptr;
        for (const auto& h : headers) {
            header_list = curl_slist_append(header_list, h.c_str());
        }
        curl.setopt(CURLOPT_HTTPHEADER, header_list);

        // Set write callbacks
        auto write_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* resp = static_cast<std::string*>(userdata);
            resp->append(ptr, size * nmemb);
            return size * nmemb;
        };
        curl.setopt(CURLOPT_WRITEFUNCTION, write_cb);
        curl.setopt(CURLOPT_WRITEDATA, &response_body);

        // Set header callback
        auto header_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* headers_str = static_cast<std::string*>(userdata);
            headers_str->append(ptr, size * nmemb);
            return size * nmemb;
        };
        curl.setopt(CURLOPT_HEADERFUNCTION, header_cb);
        curl.setopt(CURLOPT_HEADERDATA, &response_headers);

        // Perform request
        auto res = curl_easy_perform(static_cast<CURL*>(curl.get()));

        // Cleanup headers
        curl_slist_free_all(header_list);

        if (res != CURLE_OK) {
            if (res == CURLE_OPERATION_TIMEDOUT) {
                return std::unexpected(cc::utils::Error(
                    cc::utils::ErrorCode::timeout,
                    std::format("Request timed out after {}ms", timeout.count())));
            }
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::format("CURL error: {}", curl_easy_strerror(res))));
        }

        // Get status code
        long http_code = 0;
        curl_easy_getinfo(static_cast<CURL*>(curl.get()), CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = static_cast<int>(http_code);
        response.body = std::move(response_body);

        size_t offset = 0;
        while (offset < response_headers.size()) {
            auto line_end = response_headers.find('\n', offset);
            auto line = response_headers.substr(
                offset,
                line_end == std::string::npos ? std::string::npos : line_end - offset);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            offset = line_end == std::string::npos ? response_headers.size() : line_end + 1;
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto key = line.substr(0, colon);
            auto value_start = colon + 1;
            while (value_start < line.size() && line[value_start] == ' ') ++value_start;
            response.headers.emplace_back(std::move(key), line.substr(value_start));
        }

        return response;
    }
};

// =========================================================================
// Main Anthropic API Client
// =========================================================================

class AnthropicClient {
public:
    struct Config {
        std::string base_url = "https://api.anthropic.com";
        std::string api_key;
        std::string auth_token;  // For Claude AI OAuth
        std::string api_version = "2023-06-01";
        std::chrono::milliseconds timeout{120000};
        int max_retries = 10;
        std::chrono::milliseconds base_retry_delay{500};
        std::optional<std::string> fallback_model;
        Provider provider = Provider::Anthropic;
        std::string region;
        std::vector<std::string> beta_headers;
        std::string user_agent = "ClaudeCode/1.0";
    };

    explicit AnthropicClient(Config config)
        : config_(std::move(config))
        , rate_limiter_(60)
        , auth_ctx_(std::make_unique<cc::services::auth::byoc::EnterpriseAuthContext>()) {}

    [[nodiscard]] static cc::utils::Error error_from_http_response(
        int status_code,
        std::string_view body,
        std::optional<std::string> request_id = std::nullopt) {
        const auto details = ErrorFactory::from_json(status_code, body, std::move(request_id));
        auto code = cc::utils::ErrorCode::internal_error;
        switch (details.category) {
            case errors::ApiErrorCategory::Authentication:
                code = cc::utils::ErrorCode::permission_denied;
                break;
            case errors::ApiErrorCategory::InvalidRequest:
                code = cc::utils::ErrorCode::invalid_argument;
                break;
            case errors::ApiErrorCategory::RateLimited:
                code = cc::utils::ErrorCode::resource_exhausted;
                break;
            case errors::ApiErrorCategory::NetworkError:
                code = cc::utils::ErrorCode::network_error;
                break;
            case errors::ApiErrorCategory::Overloaded:
            case errors::ApiErrorCategory::ServerError:
                code = cc::utils::ErrorCode::unavailable;
                break;
            case errors::ApiErrorCategory::Unknown:
            default:
                code = cc::utils::ErrorCode::internal_error;
                break;
        }

        auto message = std::format("HTTP {} {}: {}",
            status_code,
            details.error_type.empty() ? "api_error" : details.error_type,
            details.error_message.empty() ? std::string(body) : details.error_message);
        if (details.request_id && !details.request_id->empty()) {
            message += " (request id: " + *details.request_id + ")";
        }
        if (details.retry_after_seconds) {
            message += " (retry after: " + std::to_string(*details.retry_after_seconds) + "s)";
        }
        return cc::utils::Error(code, std::move(message));
    }

    [[nodiscard]] static ApiErrorDetails error_details_from_error(const cc::utils::Error& error) {
        ApiErrorDetails details;
        details.error_message = error.message();
        details.error_type = "api_error";

        switch (error.code()) {
            case cc::utils::ErrorCode::permission_denied:
                details.category = errors::ApiErrorCategory::Authentication;
                break;
            case cc::utils::ErrorCode::invalid_argument:
                details.category = errors::ApiErrorCategory::InvalidRequest;
                break;
            case cc::utils::ErrorCode::resource_exhausted:
                details.category = errors::ApiErrorCategory::RateLimited;
                break;
            case cc::utils::ErrorCode::network_error:
            case cc::utils::ErrorCode::timeout:
                details.category = errors::ApiErrorCategory::NetworkError;
                break;
            case cc::utils::ErrorCode::unavailable:
                details.category = errors::ApiErrorCategory::ServerError;
                break;
            default:
                details.category = errors::ApiErrorCategory::Unknown;
                break;
        }

        const auto& message = error.message();
        if (message.starts_with("HTTP ")) {
            std::size_t pos = 5;
            int status = 0;
            while (pos < message.size() && message[pos] >= '0' && message[pos] <= '9') {
                status = status * 10 + (message[pos] - '0');
                ++pos;
            }
            if (status > 0) {
                details.http_status = status;
                details.category = ErrorClassifier::classify_status(status);
            }
            while (pos < message.size() && message[pos] == ' ') ++pos;
            auto type_end = message.find(':', pos);
            if (type_end != std::string::npos) {
                details.error_type = message.substr(pos, type_end - pos);
                auto message_start = type_end + 1;
                while (message_start < message.size() && message[message_start] == ' ') ++message_start;
                auto message_end = message.find(" (", message_start);
                details.error_message = message.substr(
                    message_start,
                    message_end == std::string::npos ? std::string::npos : message_end - message_start);
            }
            auto retry_pos = message.find("(retry after: ");
            if (retry_pos != std::string::npos) {
                retry_pos += std::string_view{"(retry after: "}.size();
                int seconds = 0;
                while (retry_pos < message.size() && message[retry_pos] >= '0' && message[retry_pos] <= '9') {
                    seconds = seconds * 10 + (message[retry_pos] - '0');
                    ++retry_pos;
                }
                details.retry_after_seconds = seconds;
            }
        }

        return details;
    }

    // Create message (non-streaming)
    [[nodiscard]] Result<CreateMessageResponse> create_message(
        const CreateMessageRequest& request) {

        RetryContext retry_context;
        retry_context.max_attempts = config_.max_retries;
        retry_context.fallback_model = config_.fallback_model;
        retry_context.first_attempt_time = std::chrono::steady_clock::now();

        CreateMessageRequest current_request = request;

        while (true) {
            // Check rate limit
            if (!rate_limiter_.try_acquire()) {
                auto wait_time = rate_limiter_.wait_time();
                std::this_thread::sleep_for(wait_time);
            }

            // Perform request
            auto result = perform_request(current_request);

            if (result) {
                return result;
            }

            // Handle error
            auto error_details = extract_error_details(result.error());
            auto decision = ErrorClassifier::decide(error_details, retry_context);

            switch (decision) {
                case RetryDecision::RetryImmediately:
                case RetryDecision::RetryWithDelay: {
                    auto delay = ErrorClassifier::calculate_delay(retry_context, error_details);
                    if (decision == RetryDecision::RetryWithDelay) {
                        std::this_thread::sleep_for(delay);
                    }
                    ++retry_context.attempt;
                    continue;
                }

                case RetryDecision::FallbackModel:
                    if (config_.fallback_model) {
                        current_request.model = *config_.fallback_model;
                        retry_context.consecutive_529_errors = 0;
                        ++retry_context.attempt;
                        continue;
                    }
                    return std::unexpected(result.error());

                case RetryDecision::NoRetry:
                case RetryDecision::Abort:
                default:
                    return std::unexpected(result.error());
            }
        }
    }

    // Create streaming message — returns a StreamParser fed by a background CURL transfer
    [[nodiscard]] Result<StreamParser> create_message_stream(
        const CreateMessageRequest& request) {

        // Create streaming request
        CreateMessageRequest stream_request = request;
        stream_request.stream = true;

        auto betas = betas_for_request(request);
        auto prepared = prepare_request(stream_request, betas,
                                        /*streaming=*/true);
        if (!prepared) return std::unexpected(prepared.error());

        auto& url = prepared->url;
        auto& json_body = prepared->body;
        auto& headers = prepared->headers;
        headers.push_back("Accept: text/event-stream");
        headers.push_back("Cache-Control: no-cache");
        headers.push_back("Connection: keep-alive");

        // Create stream parser with config
        StreamConfig stream_config;
        stream_config.connect_timeout = config_.timeout;
        stream_config.read_timeout = config_.timeout;
        auto parser = std::make_shared<StreamParser>(stream_config);
        parser->start();

        // On streaming 401/403, invalidate caches so the QueryEngine's retry
        // loop (which re-enters create_message_stream) gets fresh tokens.
        const bool is_enterprise_provider =
            (auth_ctx_->provider() != EProv::FirstParty);
        auto auth_ctx_ptr = auth_ctx_.get();

        // Launch streaming CURL transfer in a detached thread.
        // The parser is shared between the producer thread and the consumer.
        std::thread([url = std::move(url),
                     json_body = std::move(json_body),
                     headers = std::move(headers),
                     timeout = config_.timeout,
                     parser, auth_ctx_ptr, is_enterprise_provider]() {
            auto curl_result = CurlHandle::create();
            if (!curl_result) {
                ApiErrorDetails details;
                details.category = errors::ApiErrorCategory::NetworkError;
                details.error_type = "curl_error";
                details.error_message = "Failed to initialize CURL";
                parser->set_error(std::move(details));
                return;
            }
            CurlHandle curl = std::move(*curl_result);
            curl.setopt(CURLOPT_URL, url.c_str());
            apply_loopback_no_proxy(curl, url);
            curl.setopt(CURLOPT_POSTFIELDS, json_body.c_str());
            curl.setopt(CURLOPT_POSTFIELDSIZE,
                        static_cast<long>(json_body.size()));
            curl.setopt(CURLOPT_TIMEOUT_MS,
                        static_cast<long>(timeout.count()));
            curl.setopt(CURLOPT_CONNECTTIMEOUT_MS,
                static_cast<long>(std::min(timeout,
                                std::chrono::milliseconds{30000}).count()));

            // Set headers
            struct curl_slist* header_list = nullptr;
            for (const auto& h : headers) {
                header_list = curl_slist_append(header_list, h.c_str());
            }
            curl.setopt(CURLOPT_HTTPHEADER, header_list);

            // WRITEFUNCTION: feed chunks into StreamParser
            struct WriteCtx {
                std::shared_ptr<StreamParser> parser;
            };
            WriteCtx ctx{parser};

            auto write_cb = +[](char* ptr, size_t size, size_t nmemb,
                                void* userdata) -> size_t {
                auto* wctx = static_cast<WriteCtx*>(userdata);
                size_t bytes = size * nmemb;
                wctx->parser->feed(std::string_view(ptr, bytes));
                return bytes;
            };
            curl.setopt(CURLOPT_WRITEFUNCTION, write_cb);
            curl.setopt(CURLOPT_WRITEDATA, &ctx);

            // Perform the streaming request
            auto res = curl_easy_perform(static_cast<CURL*>(curl.get()));
            curl_slist_free_all(header_list);

            // Signal completion/error to the parser
            if (res != CURLE_OK) {
                ApiErrorDetails details;
                details.category = errors::ApiErrorCategory::NetworkError;
                details.error_type = "curl_error";
                details.error_message = curl_easy_strerror(res);
                parser->set_error(std::move(details));
            } else {
                // Check HTTP status
                long http_code = 0;
                curl_easy_getinfo(static_cast<CURL*>(curl.get()),
                                  CURLINFO_RESPONSE_CODE, &http_code);
                if (is_enterprise_provider &&
                    (http_code == 401 || http_code == 403)) {
                    auth_ctx_ptr->invalidate_all();
                }
                if (http_code >= 400) {
                    parser->set_error(ErrorFactory::from_http(
                        static_cast<int>(http_code),
                        std::format("HTTP {} error during streaming at {}",
                                    http_code, url)));
                } else {
                    parser->finish();
                }
            }
        }).detach();

        // Return a StreamParser by value; the returned copy and curl thread share
        // parser state, so next_event()/process_events() observe producer writes.
        return *parser;
    }

    // Verify API key
    [[nodiscard]] Result<bool> verify_api_key() {
        CreateMessageRequest request;
        request.model = "claude-3-haiku-20240307";  // Use cheapest model
        request.max_tokens = 1;
        request.messages.push_back(Message::from_text("user", "test"));

        auto result = create_message(request);
        if (result) {
            return true;
        }
        // Check if error is auth-related
        return false;
    }

    // Configuration access
    [[nodiscard]] const Config& config() const { return config_; }
    void set_api_key(std::string api_key) { config_.api_key = std::move(api_key); }
    void set_auth_token(std::string token) { config_.auth_token = std::move(token); }

private:
    [[nodiscard]] static std::vector<std::string> betas_for_request(
        const CreateMessageRequest& request
    ) {
        auto betas = request.betas;
        const auto task_budget_beta = std::string{"task-budgets-2026-03-13"};
        if (request.task_budget && !std::ranges::contains(betas, task_budget_beta)) {
            betas.push_back(task_budget_beta);
        }
        return betas;
    }

    // Replace the top-level "model": "..." value in a serialized JSON body by
    // reparsing.  Cheap because requests are already valid compact JSON.
    [[nodiscard]] static std::string override_model_in_json(
        std::string json_body,
        std::string_view new_model) {
        using namespace cc::utils::json;
        auto parsed = parse(json_body);
        if (!parsed) return json_body;
        auto old_model = parsed->root().get_string("model");
        if (old_model == new_model) return json_body;
        // Deep-copy the whole JSON into a mutable doc to mutate "model" string.
        JsonMutDoc mdoc;
        auto new_root = mdoc.copy_val(parsed->root());
        if (!new_root.valid() || !new_root.is_obj()) return json_body;
        // If the old model string node exists and is a string, we need to
        // remove and replace: JsonMutVal::remove + add(new_model).
        (void)new_root.remove("model");
        new_root.add("model", mdoc.string(new_model));
        mdoc.set_root(new_root);
        auto rewritten = mdoc.to_string();
        return rewritten.empty() ? json_body : rewritten;
    }

    // Drop existing 1P auth headers, then append provider-specific headers.
    [[nodiscard]] static std::vector<std::string> merge_auth_headers(
        std::vector<std::string> base_headers,
        const std::vector<std::pair<std::string, std::string>>& extra) {
        std::vector<std::string> out;
        out.reserve(base_headers.size() + extra.size());
        for (auto& h : base_headers) {
            std::string_view hv(h);
            // Remove x-api-key / Authorization: Bearer that build_headers()
            // may have appended for FirstParty.  Enterprise providers supply
            // their own auth.
            const bool is_1p_auth =
                hv.starts_with("x-api-key:") || hv.starts_with("X-Api-Key:") ||
                hv.starts_with("Authorization:");
            if (is_1p_auth) continue;
            out.push_back(std::move(h));
        }
        for (const auto& [k, v] : extra) {
            out.push_back(std::format("{}: {}", k, v));
        }
        return out;
    }

    // Output of prepare_request() — rewritten URL / body / headers that
    // account for Bedrock/Vertex/Foundry routing, SigV4, OAuth2 etc.
    struct PreparedRequest {
        std::string url;
        std::string body;
        std::vector<std::string> headers;
    };

    using EProv = cc::services::auth::byoc::EnterpriseProvider;

    [[nodiscard]] Result<PreparedRequest> prepare_request(
        const CreateMessageRequest& request,
        const std::vector<std::string>& betas,
        bool streaming) {
        const std::string canonical_id = request.model;
        std::string json_body = RequestSerializer::serialize(request);
        std::vector<std::string> headers = build_headers(betas);

        auto resolved = auth_ctx_->resolve_auth_for_request(
            canonical_id, "POST", json_body, streaming,
            /*extra_agnostic_headers=*/{});
        if (!resolved) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                std::format("enterprise auth resolve: {}",
                            resolved.error().message())));
        }

        std::string url;
        if (resolved->provider == EProv::FirstParty) {
            url = std::format("{}/v1/messages", config_.base_url);
        } else {
            // Bedrock: {base_url}{/model/.../invoke[-with-response-stream]}
            // Vertex:  {base_url}{/publishers/.../models/ID:streamRawPredict}
            // Foundry: {base_url}{/openai/deployments/.../chat/completions}
            url = resolved->base_url + resolved->bedrock_path;
            headers = merge_auth_headers(std::move(headers), resolved->headers);
            if (!resolved->model_id.empty() &&
                resolved->model_id != canonical_id) {
                json_body = override_model_in_json(std::move(json_body),
                                                   resolved->model_id);
            }
        }
        return PreparedRequest{
            .url = std::move(url),
            .body = std::move(json_body),
            .headers = std::move(headers),
        };
    }

    [[nodiscard]] Result<CreateMessageResponse> perform_request(
        const CreateMessageRequest& request) {

        auto betas = betas_for_request(request);
        auto prepared = prepare_request(request, betas, /*streaming=*/false);
        if (!prepared) return std::unexpected(prepared.error());

        auto http_result = HttpClient::post(
            prepared->url, prepared->body, prepared->headers, config_.timeout);
        if (!http_result) {
            return std::unexpected(http_result.error());
        }

        // On 401/403, refresh all credential caches so the retry loop uses
        // fresh tokens / signatures on the next attempt.
        if (http_result->status_code == 401 || http_result->status_code == 403) {
            auth_ctx_->invalidate_all();
        }

        // Check for HTTP errors
        if (http_result->status_code >= 400) {
            return std::unexpected(error_from_http_response(
                http_result->status_code,
                http_result->body));
        }

        // Parse successful response
        return ResponseParser::parse(http_result->body);
    }

    [[nodiscard]] std::vector<std::string> build_headers(
        const std::vector<std::string>& request_betas = {}
    ) const {
        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        headers.push_back(std::format("anthropic-version: {}", config_.api_version));
        headers.push_back(std::format("User-Agent: {}", config_.user_agent));

        // Authentication
        if (!config_.auth_token.empty()) {
            headers.push_back(std::format("Authorization: Bearer {}", config_.auth_token));
        } else if (!config_.api_key.empty()) {
            headers.push_back(std::format("x-api-key: {}", config_.api_key));
        }

        // Beta headers
        auto append_beta = [&](const std::string& beta) {
            for (const auto& header : headers) {
                if (header == std::format("anthropic-beta: {}", beta)) return;
            }
            headers.push_back(std::format("anthropic-beta: {}", beta));
        };
        for (const auto& beta : config_.beta_headers) append_beta(beta);
        for (const auto& beta : request_betas) append_beta(beta);

        return headers;
    }

    [[nodiscard]] ApiErrorDetails extract_error_details(const cc::utils::Error& error) {
        return error_details_from_error(error);
    }

    Config config_;
    RateLimiter rate_limiter_;
    std::unique_ptr<cc::services::auth::byoc::EnterpriseAuthContext> auth_ctx_;
};

} // namespace cc::services::api
