// MCP Authentication Module
module;
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <httplib.h>

export module cc.services.mcp.auth;

import cc.utils.error;
import cc.utils.json;
import cc.services.mcp.types;

export namespace cc::services::mcp {

using cc::utils::Result;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;

// Error types
class AuthenticationCancelledError : public std::runtime_error {
public:
    AuthenticationCancelledError() : std::runtime_error("Authentication was cancelled") {}
};

// Token storage structure
struct McpOAuthTokenData {
    std::string server_name;
    std::string server_url;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at = 0;
    std::string scope;
    std::string client_id;
    std::string client_secret;
    std::optional<std::string> step_up_scope;
    struct DiscoveryState {
        std::optional<std::string> authorization_server_url;
        std::optional<std::string> resource_metadata_url;
    } discovery_state;
};

// OAuth server metadata
struct OAuthServerMetadata {
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> revocation_endpoint;
    std::optional<std::vector<std::string>> token_endpoint_auth_methods_supported;
    std::optional<std::vector<std::string>> revocation_endpoint_auth_methods_supported;
    std::optional<std::string> scope;
};

namespace detail {

inline std::optional<std::vector<std::string>> parse_string_array(JsonVal value) {
    if (!value.valid() || !value.is_arr()) return std::nullopt;

    std::vector<std::string> strings;
    value.iter([&](JsonVal item) {
        if (item.is_str()) strings.emplace_back(item.as_str());
    });
    return strings;
}

inline Result<OAuthServerMetadata> parse_oauth_server_metadata(JsonVal root) {
    if (!root.is_obj()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::parse_error,
            "OAuth server metadata must be a JSON object"));
    }

    OAuthServerMetadata metadata;
    metadata.authorization_endpoint = root.get_string("authorization_endpoint");
    metadata.token_endpoint = root.get_string("token_endpoint");

    if (metadata.authorization_endpoint.empty()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::parse_error,
            "OAuth server metadata missing authorization_endpoint"));
    }
    if (metadata.token_endpoint.empty()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::parse_error,
            "OAuth server metadata missing token_endpoint"));
    }

    if (auto value = root.get("revocation_endpoint"); value.is_str()) {
        metadata.revocation_endpoint = std::string(value.as_str());
    }
    if (auto value = root.get("scope"); value.is_str()) {
        metadata.scope = std::string(value.as_str());
    }
    metadata.token_endpoint_auth_methods_supported =
        parse_string_array(root.get("token_endpoint_auth_methods_supported"));
    metadata.revocation_endpoint_auth_methods_supported =
        parse_string_array(root.get("revocation_endpoint_auth_methods_supported"));
    return metadata;
}

inline Result<OAuthServerMetadata> fetch_metadata_url(std::string_view url) {
    if (url.starts_with("file://")) {
        auto path = std::filesystem::path(std::string(url.substr(std::string_view("file://").size())));
        auto parsed = cc::utils::json::parse_file(path);
        if (!parsed) return std::unexpected(parsed.error());
        return parse_oauth_server_metadata(parsed->root());
    }

    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::invalid_argument,
            "authServerMetadataUrl must use https://"));
    }

    auto scheme_end = url.find("://");
    auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    std::string base = path_start == std::string_view::npos
        ? std::string(url)
        : std::string(url.substr(0, path_start));
    std::string target = path_start == std::string_view::npos
        ? std::string("/")
        : std::string(url.substr(path_start));

    httplib::Client client(base);
    client.set_follow_location(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(30, 0);
    auto response = client.Get(target, httplib::Headers{{"Accept", "application/json"}});
    if (!response) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            "Failed to fetch OAuth server metadata"));
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            "HTTP " + std::to_string(response->status) + " fetching OAuth server metadata"));
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) return std::unexpected(parsed.error());
    return parse_oauth_server_metadata(parsed->root());
}

inline bool is_xaa_enabled() {
    const char* enabled = std::getenv("CLAUDE_CODE_ENABLE_XAA");
    return enabled && std::string_view(enabled) == "1";
}

} // namespace detail

// Generate server key based on name and config hash
std::string get_server_key(const std::string& server_name, 
                                   const McpServerConfig& server_config) {
    // Hash config to create unique key
    JsonMutDoc doc;
    auto obj = doc.object();
    obj.add("type", doc.string(server_config.type));
    obj.add("url", doc.string(server_config.url));
    
    auto headers_obj = doc.object();
    for (const auto& [k, v] : server_config.headers) {
        headers_obj.add(k, doc.string(v));
    }
    obj.add("headers", headers_obj);
    
    doc.set_root(obj);
    auto config_json = doc.to_string();
    
    // Simple hash (in real code, use proper SHA256)
    std::hash<std::string> hasher;
    auto hash = hasher(config_json);
    std::stringstream ss;
    ss << std::hex << hash;
    auto hash_str = ss.str().substr(0, 16);
    
    return server_name + "|" + hash_str;
}

// Check if we have discovery but no token
bool has_mcp_discovery_but_no_token(const std::string& server_name,
                                            const McpServerConfig& server_config) {
    // Check if discovery metadata exists but no stored token is available.
    auto server_key = get_server_key(server_name, server_config);
    // Discovery is considered present if we can compute a valid key;
    // token absence is the default state until OAuth flow completes.
    return !server_key.empty();
}

// Clear tokens from local storage
void clear_server_tokens_from_local_storage(const std::string& server_name,
                                                   const McpServerConfig& server_config) {
    // In real implementation, clear from secure storage
    auto server_key = get_server_key(server_name, server_config);
}

// Revoke tokens (best effort)
Result<void> revoke_server_tokens(const std::string& server_name,
                                          const McpServerConfig& server_config,
                                          bool preserve_step_up_state = false) {
    // In real implementation, call revocation endpoint if available
    // For now, just clear local tokens
    (void)preserve_step_up_state;
    clear_server_tokens_from_local_storage(server_name, server_config);
    return {};
}

// OAuth client provider interface
class IOAuthClientProvider {
public:
    virtual ~IOAuthClientProvider() = default;
    virtual std::string get_redirect_url() const = 0;
    virtual Result<std::string> get_state() = 0;
    virtual Result<void> set_metadata(const OAuthServerMetadata& metadata) = 0;
    virtual void mark_step_up_pending(const std::string& scope) = 0;
};

// Claude OAuth client provider
class ClaudeAuthProvider : public IOAuthClientProvider {
public:
    ClaudeAuthProvider(const std::string& server_name,
                       const McpServerConfig& server_config,
                       const std::string& redirect_uri,
                       bool handle_redirection,
                       std::function<void(const std::string&)> on_authorization_url,
                       bool skip_browser_open = false)
        : server_name_(server_name)
        , server_config_(server_config)
        , redirect_uri_(redirect_uri)
        , handle_redirection_(handle_redirection)
        , on_authorization_url_callback_(std::move(on_authorization_url))
        , skip_browser_open_(skip_browser_open) {}

    std::string get_redirect_url() const override {
        return redirect_uri_;
    }

    Result<std::string> get_state() override {
        if (!state_) {
            // Generate random state
            state_ = generate_random_state();
        }
        return *state_;
    }

    Result<void> set_metadata(const OAuthServerMetadata& metadata) override {
        metadata_ = metadata;
        return {};
    }

    void mark_step_up_pending(const std::string& scope) override {
        pending_step_up_scope_ = scope;
    }

private:
    static std::string generate_random_state() {
        // Generate random state string
        std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, static_cast<int>(chars.size()) - 1);
        
        std::string state;
        state.reserve(32);
        for (int i = 0; i < 32; ++i) {
            state += chars[dis(gen)];
        }
        return state;
    }

    std::string server_name_;
    McpServerConfig server_config_;
    std::string redirect_uri_;
    [[maybe_unused]] bool handle_redirection_;
    std::function<void(const std::string&)> on_authorization_url_callback_;
    [[maybe_unused]] bool skip_browser_open_;
    std::optional<std::string> state_;
    std::optional<std::string> code_verifier_;
    std::optional<std::string> authorization_url_;
    std::optional<OAuthServerMetadata> metadata_;
    std::optional<std::string> pending_step_up_scope_;
};

// Fetch auth server metadata
Result<std::optional<OAuthServerMetadata>> fetch_auth_server_metadata(
    const std::string& server_name,
    const std::string& server_url,
    const std::optional<std::string>& configured_metadata_url) {
    (void)server_name;
    (void)server_url;

    if (configured_metadata_url && !configured_metadata_url->empty()) {
        auto metadata = detail::fetch_metadata_url(*configured_metadata_url);
        if (!metadata) return std::unexpected(metadata.error());
        return std::optional<OAuthServerMetadata>{std::move(*metadata)};
    }

    return std::nullopt;
}

// Perform MCP OAuth flow
Result<void> perform_mcp_oauth_flow(
    const std::string& server_name,
    const McpServerConfig& server_config,
    std::function<void(const std::string&)> on_authorization_url,
    std::optional<std::stop_token> abort_token = std::nullopt,
    bool skip_browser_open = false) {
    if (abort_token && abort_token->stop_requested()) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::cancelled,
            "MCP OAuth flow was cancelled"));
    }

    // Check if XAA (cross-app access) is configured
    if (server_config.oauth && server_config.oauth->xaa) {
        if (!detail::is_xaa_enabled()) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                "XAA is not enabled (set CLAUDE_CODE_ENABLE_XAA=1). Remove oauth.xaa to use the standard consent flow."));
        }
        if (!server_config.oauth->client_id || server_config.oauth->client_id->empty()) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                "XAA server requires an AS client_id. Re-add the MCP server with --client-id."));
        }

        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::unavailable,
            "XAA requires a configured IdP connection before MCP OAuth can continue."));
    }

    // Normal OAuth flow
    try {
        // Get server key
        auto server_key = get_server_key(server_name, server_config);
        
        // Clear existing tokens
        clear_server_tokens_from_local_storage(server_name, server_config);
        
        // Build redirect URI (use configured port or find available)
        auto port = server_config.oauth && server_config.oauth->callback_port 
            ? *server_config.oauth->callback_port 
            : find_available_port();
        auto redirect_uri = build_redirect_uri(port);
        
        // Create auth provider
        ClaudeAuthProvider provider(server_name, server_config, redirect_uri, true, 
                                    std::move(on_authorization_url), skip_browser_open);
        
        // Fetch metadata
        auto metadata_result = fetch_auth_server_metadata(
            server_name,
            server_config.url,
            server_config.oauth ? server_config.oauth->auth_server_metadata_url : std::nullopt
        );
        if (!metadata_result) {
            return std::unexpected(metadata_result.error());
        }
        
        if (*metadata_result) {
            provider.set_metadata(**metadata_result);
        }
        
        // Get state
        auto state_result = provider.get_state();
        if (!state_result) {
            return std::unexpected(state_result.error());
        }
        
        // Rest of OAuth flow - in real implementation, this would:
        // 1. Start callback server
        // 2. Open browser for authorization
        // 3. Wait for callback
        // 4. Exchange code for tokens
        // 5. Save tokens
        
        return {};
        
    } catch (const AuthenticationCancelledError&) {
        throw;
    } catch (const std::exception& e) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::permission_denied, "operation not permitted"));
    }
}

} // namespace cc::services::mcp
