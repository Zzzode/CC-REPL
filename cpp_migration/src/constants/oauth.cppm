// C++23 module: OAuth configuration constants for authentication flows.
// Covers production, staging, and local dev environments.
module;
#include <array>
#include <string>
#include <string_view>

export module cc.constants.oauth;


export namespace cc::constants::oauth {

// OAuth scope constants
inline constexpr std::string_view claude_ai_inference_scope = "user:inference";
inline constexpr std::string_view claude_ai_profile_scope = "user:profile";
inline constexpr std::string_view console_scope = "org:create_api_key";
inline constexpr std::string_view oauth_beta_header = "oauth-2025-04-20";

// Console OAuth scopes - for API key creation via Console
inline constexpr std::array<std::string_view, 2> console_oauth_scopes = {
    console_scope,
    claude_ai_profile_scope,
};

// Claude.ai OAuth scopes - for Claude.ai subscribers (Pro/Max/Team/Enterprise)
inline constexpr std::array<std::string_view, 5> claude_ai_oauth_scopes = {
    claude_ai_profile_scope,
    claude_ai_inference_scope,
    "user:sessions:claude_code",
    "user:mcp_servers",
    "user:file_upload",
};

// All unique OAuth scopes (union of console and claude.ai scopes)
inline constexpr std::array<std::string_view, 6> all_oauth_scopes = {
    console_scope,
    claude_ai_profile_scope,
    claude_ai_inference_scope,
    "user:sessions:claude_code",
    "user:mcp_servers",
    "user:file_upload",
};

// MCP OAuth Client ID Metadata Document URL (SEP-991/CIMD)
inline constexpr std::string_view mcp_client_metadata_url =
    "https://claude.ai/oauth/claude-code-client-metadata";

enum class OauthConfigType {
    prod,
    staging,
    local
};

// OAuth configuration structure
struct OauthConfig {
    std::string_view base_api_url;
    std::string_view console_authorize_url;
    std::string_view claude_ai_authorize_url;
    std::string_view claude_ai_origin;
    std::string_view token_url;
    std::string_view api_key_url;
    std::string_view roles_url;
    std::string_view console_success_url;
    std::string_view claudeai_success_url;
    std::string_view manual_redirect_url;
    std::string_view client_id;
    std::string_view oauth_file_suffix;
    std::string_view mcp_proxy_url;
    std::string_view mcp_proxy_path;
};

// Production OAuth configuration
inline constexpr OauthConfig prod_oauth_config = {
    .base_api_url = "https://api.anthropic.com",
    .console_authorize_url = "https://platform.claude.com/oauth/authorize",
    .claude_ai_authorize_url = "https://claude.com/cai/oauth/authorize",
    .claude_ai_origin = "https://claude.ai",
    .token_url = "https://platform.claude.com/v1/oauth/token",
    .api_key_url = "https://api.anthropic.com/api/oauth/claude_cli/create_api_key",
    .roles_url = "https://api.anthropic.com/api/oauth/claude_cli/roles",
    .console_success_url = "https://platform.claude.com/buy_credits?returnUrl=/oauth/code/success%3Fapp%3Dclaude-code",
    .claudeai_success_url = "https://platform.claude.com/oauth/code/success?app=claude-code",
    .manual_redirect_url = "https://platform.claude.com/oauth/code/callback",
    .client_id = "9d1c250a-e61b-44d9-88ed-5944d1962f5e",
    .oauth_file_suffix = "",
    .mcp_proxy_url = "https://mcp-proxy.anthropic.com",
    .mcp_proxy_path = "/v1/mcp/{server_id}",
};

// Staging OAuth configuration (ant builds only)
inline constexpr OauthConfig staging_oauth_config = {
    .base_api_url = "https://api-staging.anthropic.com",
    .console_authorize_url = "https://platform.staging.ant.dev/oauth/authorize",
    .claude_ai_authorize_url = "https://claude-ai.staging.ant.dev/oauth/authorize",
    .claude_ai_origin = "https://claude-ai.staging.ant.dev",
    .token_url = "https://platform.staging.ant.dev/v1/oauth/token",
    .api_key_url = "https://api-staging.anthropic.com/api/oauth/claude_cli/create_api_key",
    .roles_url = "https://api-staging.anthropic.com/api/oauth/claude_cli/roles",
    .console_success_url = "https://platform.staging.ant.dev/buy_credits?returnUrl=/oauth/code/success%3Fapp%3Dclaude-code",
    .claudeai_success_url = "https://platform.staging.ant.dev/oauth/code/success?app=claude-code",
    .manual_redirect_url = "https://platform.staging.ant.dev/oauth/code/callback",
    .client_id = "22422756-60c9-4084-8eb7-27705fd5cf9a",
    .oauth_file_suffix = "-staging-oauth",
    .mcp_proxy_url = "https://mcp-proxy-staging.anthropic.com",
    .mcp_proxy_path = "/v1/mcp/{server_id}",
};

// Allowed base URLs for CLAUDE_CODE_CUSTOM_OAUTH_URL override
// Only FedStart/PubSec deployments are permitted
inline constexpr std::array<std::string_view, 3> allowed_oauth_base_urls = {
    "https://beacon.claude-ai.staging.ant.dev",
    "https://claude.fedstart.com",
    "https://claude-staging.fedstart.com",
};

// File suffix helpers for different oauth configurations
inline constexpr std::string_view file_suffix_custom_oauth = "-custom-oauth";
inline constexpr std::string_view file_suffix_local_oauth = "-local-oauth";
inline constexpr std::string_view file_suffix_staging_oauth = "-staging-oauth";
inline constexpr std::string_view file_suffix_prod = "";

} // namespace cc::constants::oauth
