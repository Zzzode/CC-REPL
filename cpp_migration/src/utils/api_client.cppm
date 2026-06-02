/// @file api_client.cppm
/// @brief API request/response types, client interface, connection prewarming, rate limit handling
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include <string_view>

export module cc.utils.api_client;

import cc.utils.json;
import cc.utils.http;

export namespace cc::utils::api_client {

using cc::utils::json::JsonVal;

// ---------------------------------------------------------------------------
// Cache scope for system prompt blocks
// ---------------------------------------------------------------------------

enum class CacheScope : std::uint8_t {
    Global,
    Org,
};

/// A system prompt block with optional cache scope
struct SystemPromptBlock {
    std::string text;
    std::optional<CacheScope> cache_scope;
};

// ---------------------------------------------------------------------------
// Tool schema types
// ---------------------------------------------------------------------------

/// Cache control options for tool schemas
struct CacheControl {
    std::string type{"ephemeral"};
    std::optional<std::string> scope;   // "global" | "org"
    std::optional<std::string> ttl;     // "5m" | "1h"
};

/// Extended tool schema with beta fields
struct ToolSchema {
    std::string name;
    std::string description;
    JsonVal input_schema;
    std::optional<bool> strict;
    std::optional<bool> defer_loading;
    std::optional<CacheControl> cache_control;
    std::optional<bool> eager_input_streaming;
};

/// Options for converting a tool to API schema
struct ToolToSchemaOptions {
    std::string model;
    bool defer_loading{false};
    std::optional<CacheControl> cache_control;
    std::vector<std::string> allowed_agent_types;
};

// ---------------------------------------------------------------------------
// API request/response types
// ---------------------------------------------------------------------------

/// HTTP method enum
enum class HttpMethod : std::uint8_t {
    Get,
    Post,
    Put,
    Delete,
    Head,
    Patch,
};

/// Rate limit information from API response headers
struct RateLimitInfo {
    std::optional<int> requests_remaining;
    std::optional<int> tokens_remaining;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<std::chrono::system_clock::time_point> reset_at;
};

/// API error response
struct ApiError {
    int status_code{0};
    std::string error_type;
    std::string message;
    std::optional<RateLimitInfo> rate_limit;
};

/// API response wrapper
struct ApiResponse {
    int status_code{0};
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::optional<RateLimitInfo> rate_limit;
};

// ---------------------------------------------------------------------------
// API client configuration
// ---------------------------------------------------------------------------

/// Provider type for API routing
enum class ApiProvider : std::uint8_t {
    FirstParty,
    Bedrock,
    Vertex,
    Foundry,
    ThirdParty,
};

/// API client configuration
struct ApiClientConfig {
    std::string base_url;
    std::string api_key;
    ApiProvider provider{ApiProvider::FirstParty};
    std::optional<std::string> organization_id;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::string> proxy_url;
};

// ---------------------------------------------------------------------------
// Connection prewarming
// ---------------------------------------------------------------------------

/// Prewarm state for connection pooling
struct PreconnectState {
    bool fired{false};
};

/// Preconnect to the API to overlap TCP+TLS handshake with startup.
/// Skipped when proxy/mTLS/unix socket configured or using Bedrock/Vertex/Foundry.
[[nodiscard]] auto preconnect_api(const ApiClientConfig& config)
    -> std::expected<void, std::string>;

// ---------------------------------------------------------------------------
// Rate limit handling
// ---------------------------------------------------------------------------

/// Rate limit strategy
enum class RateLimitStrategy : std::uint8_t {
    Retry,
    Backoff,
    Abort,
};

/// Determine strategy for handling a rate limit response
[[nodiscard]] auto determine_rate_limit_strategy(
    const RateLimitInfo& info,
    int attempt_number) -> RateLimitStrategy;

/// Calculate backoff duration using exponential backoff with jitter
[[nodiscard]] auto calculate_backoff(int attempt_number)
    -> std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// API client interface
// ---------------------------------------------------------------------------

/// Abstract API client for making requests
class IApiClient {
public:
    virtual ~IApiClient() = default;

    [[nodiscard]] virtual auto request(
        HttpMethod method,
        std::string_view path,
        std::optional<std::string_view> body,
        const std::unordered_map<std::string, std::string>& headers)
        -> std::expected<ApiResponse, std::string> = 0;

    [[nodiscard]] virtual auto is_connected() const -> bool = 0;
    virtual void disconnect() = 0;
};

/// Create an API client with the given configuration
[[nodiscard]] auto create_api_client(ApiClientConfig config)
    -> std::unique_ptr<IApiClient>;

// ---------------------------------------------------------------------------
// System prompt utilities
// ---------------------------------------------------------------------------

/// Split system prompt into blocks for API cache key matching
[[nodiscard]] auto split_sys_prompt_prefix(
    const std::vector<std::string>& system_prompt,
    bool skip_global_cache_for_system_prompt = false)
    -> std::vector<SystemPromptBlock>;

/// Append system context key-value pairs to system prompt
[[nodiscard]] auto append_system_context(
    const std::vector<std::string>& system_prompt,
    const std::unordered_map<std::string, std::string>& context)
    -> std::vector<std::string>;

/// Normalize tool input before execution (provider-specific adjustments)
[[nodiscard]] auto normalize_tool_input(
    std::string_view tool_name,
    JsonVal input) -> std::expected<JsonVal, std::string>;

/// Strip injected fields from tool input before sending to API
[[nodiscard]] auto normalize_tool_input_for_api(
    std::string_view tool_name,
    JsonVal input) -> JsonVal;

} // namespace cc::utils::api_client
