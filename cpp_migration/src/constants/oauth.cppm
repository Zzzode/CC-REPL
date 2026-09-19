// C++23 module: OAuth configuration for the interactive login flow.
//
// This module deliberately ships NO vendor credentials. The upstream project
// hard-coded a first-party OAuth deployment (client ids, console URLs, staging
// and FedStart hosts); all of that is vendor infrastructure that does not
// belong in a personal build, and pointing at it after a rename would have sent
// users to hosts that do not resolve.
//
// Instead the OAuth deployment is USER-SUPPLIED: set the environment variables
// below to talk to any OAuth 2.0 provider that supports the authorization-code
// flow. With none set, the interactive OAuth flow reports that no provider is
// configured and the API-key / token paths are used instead.
//
// The scope STRINGS are kept as-is where they are wire values that a provider
// may recognise verbatim; they are harmless when unused.
module;
#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

export module cc.constants.oauth;


export namespace cc::constants::oauth {

// OAuth scope constants.
inline constexpr std::string_view inference_scope = "user:inference";
inline constexpr std::string_view profile_scope = "user:profile";
inline constexpr std::string_view api_key_scope = "org:create_api_key";
inline constexpr std::string_view oauth_beta_header = "oauth-2025-04-20";

inline constexpr std::array<std::string_view, 2> api_key_oauth_scopes = {
    api_key_scope,
    profile_scope,
};

inline constexpr std::array<std::string_view, 5> subscriber_oauth_scopes = {
    profile_scope,
    inference_scope,
    "user:sessions:loom_code",
    "user:mcp_servers",
    "user:file_upload",
};

/// Union of both scope sets, for providers that accept a single request.
inline constexpr std::array<std::string_view, 6> all_oauth_scopes = {
    api_key_scope,
    profile_scope,
    inference_scope,
    "user:sessions:loom_code",
    "user:mcp_servers",
    "user:file_upload",
};

/// OAuth 2.0 authorization-code endpoint set. Every field is empty unless the
/// user configures a provider, so an unconfigured build fails loudly at the
/// point of use rather than silently targeting someone else's server.
struct OauthConfig {
    std::string_view authorize_url;
    std::string_view token_url;
    std::string_view client_id;
    std::string_view redirect_uri;
    std::string_view oauth_file_suffix;
};

namespace detail {
/// Read an environment variable, returning "" when unset or empty.
[[nodiscard]] inline std::string_view env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string_view{value} : std::string_view{};
}
}  // namespace detail

/// Environment variables a user sets to enable the interactive OAuth flow:
///   LOOM_OAUTH_AUTHORIZE_URL   authorization endpoint
///   LOOM_OAUTH_TOKEN_URL       token endpoint
///   LOOM_OAUTH_CLIENT_ID       client id issued by that provider
///   LOOM_OAUTH_REDIRECT_URI    loopback redirect the provider is told about
///
/// @note this is a function, not a constant: the values must be read at run
/// time, and a constexpr global would bake in whatever the build environment
/// happened to contain.
[[nodiscard]] inline OauthConfig oauth_config_from_env() {
    return OauthConfig{
        .authorize_url = detail::env_or_empty("LOOM_OAUTH_AUTHORIZE_URL"),
        .token_url = detail::env_or_empty("LOOM_OAUTH_TOKEN_URL"),
        .client_id = detail::env_or_empty("LOOM_OAUTH_CLIENT_ID"),
        .redirect_uri = detail::env_or_empty("LOOM_OAUTH_REDIRECT_URI"),
        .oauth_file_suffix = "",
    };
}

/// True when enough of the provider is configured for the flow to be startable.
[[nodiscard]] inline bool oauth_config_is_usable(const OauthConfig& cfg) {
    return !cfg.authorize_url.empty() && !cfg.token_url.empty() &&
           !cfg.client_id.empty();
}

// File suffix helpers for the credential store, keyed by which provider a token
// came from. Retained so credentials written by different configurations do not
// overwrite one another.
inline constexpr std::string_view file_suffix_custom_oauth = "-custom-oauth";
inline constexpr std::string_view file_suffix_env_oauth = "-env-oauth";
inline constexpr std::string_view file_suffix_default = "";

} // namespace cc::constants::oauth
