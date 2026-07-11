/// @file xaa.cppm
/// @brief Cross-App Access (XAA) / Enterprise Managed Authorization (SEP-990)
///
/// Obtains an MCP access token WITHOUT a browser consent screen by chaining:
///   1. RFC 8693 Token Exchange at the IdP: id_token → ID-JAG
///   2. RFC 7523 JWT Bearer Grant at the AS: ID-JAG → access_token
///
/// TS REF: src/services/mcp/xaa.ts
///
/// Spec refs:
///   - ID-JAG (IETF draft): https://datatracker.ietf.org/doc/draft-ietf-oauth-identity-assertion-authz-grant/
///   - MCP ext-auth (SEP-990): https://github.com/modelcontextprotocol/ext-auth
///   - RFC 8693 (Token Exchange), RFC 7523 (JWT Bearer), RFC 9728 (PRM)
module;
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>

export module cc.services.mcp.xaa;

import cc.utils.crypto;
import cc.utils.error;
import cc.utils.json;
import cc.services.mcp.xaa_idp_login;

export namespace cc::services::mcp {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;
namespace fs = std::filesystem;

// ─── Constants ────────────────────────────────────────────────────────────

// TS REF: xaa.ts:29
inline constexpr int kXaaRequestTimeoutMs = 30000;

// TS REF: xaa.ts:31-34 — grant type and token type URNs
inline constexpr std::string_view kTokenExchangeGrant =
    "urn:ietf:params:oauth:grant-type:token-exchange";
inline constexpr std::string_view kJwtBearerGrant =
    "urn:ietf:params:oauth:grant-type:jwt-bearer";
inline constexpr std::string_view kIdJagTokenType =
    "urn:ietf:params:oauth:token-type:id-jag";
inline constexpr std::string_view kIdTokenType =
    "urn:ietf:params:oauth:token-type:id_token";

// ─── Error Types ──────────────────────────────────────────────────────────

/// TS REF: xaa.ts:77-84 XaaTokenExchangeError
///
/// Thrown when the IdP token-exchange leg fails. Carries should_clear_id_token
/// so callers can decide whether to drop the cached id_token based on OAuth
/// error semantics:
///   - 4xx / invalid_grant / invalid_token → id_token is bad, clear it
///   - 5xx → IdP is down, id_token may still be valid, keep it
///   - 200 with structurally-invalid body → protocol violation, clear it
class XaaTokenExchangeError : public std::runtime_error {
public:
    const bool should_clear_id_token;

    XaaTokenExchangeError(std::string_view message, bool clear_id_token)
        : std::runtime_error(std::string(message))
        , should_clear_id_token(clear_id_token) {}
};

// ─── Types ────────────────────────────────────────────────────────────────

/// TS REF: xaa.ts:402-415 XaaConfig
///
/// Config needed to run the full XAA orchestrator.
/// Mirrors the conformance test context shape.
struct XaaConfig {
    /// Client ID registered at the MCP server's authorization server
    std::string client_id;
    /// Client secret for the MCP server's authorization server
    std::string client_secret;
    /// Client ID registered at the IdP (for the token-exchange request)
    std::string idp_client_id;
    /// Optional IdP client secret (client_secret_post)
    std::optional<std::string> idp_client_secret;
    /// The user's OIDC id_token from the IdP login
    std::string idp_id_token;
    /// IdP token endpoint (where to send the RFC 8693 token-exchange)
    std::string idp_token_endpoint;
    /// IdP issuer URL (for discovery and caching)
    std::string idp_issuer;
    /// Optional scope
    std::optional<std::string> scope;
};

/// TS REF: xaa.ts:126-129 ProtectedResourceMetadata
struct ProtectedResourceMetadata {
    std::string resource;
    std::vector<std::string> authorization_servers;
};

/// TS REF: xaa.ts:167-172 AuthorizationServerMetadata
struct AuthorizationServerMetadata {
    std::string issuer;
    std::string token_endpoint;
    std::optional<std::vector<std::string>> grant_types_supported;
    std::optional<std::vector<std::string>> token_endpoint_auth_methods_supported;
};

/// TS REF: xaa.ts:214-219 JwtAuthGrantResult
struct JwtAuthGrantResult {
    /// The ID-JAG (Identity Assertion Authorization Grant)
    std::string jwt_auth_grant;
    std::optional<int64_t> expires_in;
    std::optional<std::string> scope;
};

/// TS REF: xaa.ts:312-318 XaaTokenResult
struct XaaTokenResult {
    std::string access_token;
    std::string token_type = "Bearer";
    std::optional<int64_t> expires_in;
    std::optional<std::string> scope;
    std::optional<std::string> refresh_token;
};

/// TS REF: xaa.ts:320-328 XaaResult
struct XaaResult : XaaTokenResult {
    /// The AS issuer URL discovered via PRM. Callers must persist this as
    /// discovery_state.authorization_server_url for refresh and revocation.
    std::string authorization_server_url;
};

// ─── URL Utilities ────────────────────────────────────────────────────────

namespace detail {

/// TS REF: xaa.ts:61-67 normalizeUrl()
///
/// RFC 8414 §3.3 / RFC 9728 §3.3 identifier comparison. Roundtrip through
/// URL parsing to apply RFC 3986 §6.2.2 syntax-based normalization (lowercases
/// scheme+host, drops default port), then strip trailing slash.
[[nodiscard]] inline std::string normalize_url(std::string_view url) {
    std::string s(url);
    auto scheme_end = s.find("://");
    if (scheme_end == std::string_view::npos) {
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    auto host_start = scheme_end + 3;
    auto path_start = s.find('/', host_start);
    std::string host = path_start == std::string::npos
        ? s.substr(host_start)
        : s.substr(host_start, path_start - host_start);

    // Lowercase host
    for (auto& c : host) c = static_cast<char>(std::tolower(c));

    // Extract port and check if it's the default for the scheme
    std::string hostname = host;
    auto colon_pos = host.rfind(':');
    if (colon_pos != std::string::npos) {
        std::string port_str = host.substr(colon_pos + 1);
        hostname = host.substr(0, colon_pos);
        std::string scheme = s.substr(0, scheme_end);
        for (auto& c : scheme) c = static_cast<char>(std::tolower(c));
        bool is_default_port = false;
        if (scheme == "http" && port_str == "80") is_default_port = true;
        if (scheme == "https" && port_str == "443") is_default_port = true;
        if (is_default_port) host = hostname;
    }

    std::string path = path_start == std::string::npos ? "" : s.substr(path_start);
    while (!path.empty() && path.back() == '/') path.pop_back();

    std::string scheme = s.substr(0, scheme_end);
    for (auto& c : scheme) c = static_cast<char>(std::tolower(c));

    return scheme + "://" + host + path;
}

/// TS REF: xaa.ts:91-97 redactTokens()
///
/// Redacts sensitive token values from debug output. Works on both parsed and
/// raw string bodies.
[[nodiscard]] inline std::string redact_tokens(std::string_view raw) {
    static const std::regex sensitive_re(
        R"REGEX("(access_token|refresh_token|id_token|assertion|subject_token|client_secret)"\s*:\s*"[^"]*")REGEX");
    std::string s(raw);
    return std::regex_replace(s, sensitive_re,
        "\"$1\":\"[REDACTED]\"",
        std::regex_constants::format_default);
}

/// TS REF: xaa.ts:94 redactTokens() overload for unknown types
[[nodiscard]] inline std::string redact_tokens_json(JsonVal val) {
    // Serialize then redact
    // We can't easily serialize a JsonVal to string without yyjson_write,
    // so we redact known keys by constructing a safe representation.
    // For simplicity, return a placeholder — callers should use the string
    // version with the raw response body.
    (void)val;
    return "[JSON object — tokens redacted]";
}

/// Perform an HTTP POST with form-encoded body. Returns {status, body}.
struct FormPostResult {
    int status = 0;
    std::string body;
};

[[nodiscard]] inline Result<FormPostResult> post_form(
    std::string_view url,
    std::string_view body,
    const httplib::Headers& headers = {}) {
    auto parsed = parse_url(url);
    if (!parsed) return std::unexpected(parsed.error());

    httplib::Client client(parsed->base);
    client.set_follow_location(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(kXaaRequestTimeoutMs / 1000,
        (kXaaRequestTimeoutMs % 1000) * 1000);

    httplib::Headers merged_headers = headers;
    if (merged_headers.find("Accept") == merged_headers.end()) {
        merged_headers.emplace("Accept", "application/json");
    }

    auto response = client.Post(
        parsed->path, merged_headers,
        std::string(body),
        "application/x-www-form-urlencoded");
    if (!response) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA: POST request failed to " + parsed->base + parsed->path));
    }
    return FormPostResult{
        .status = response->status,
        .body = std::move(response->body)};
}

/// Perform an HTTP GET. Returns {status, body}.
[[nodiscard]] inline Result<FormPostResult> http_get(
    std::string_view url,
    const httplib::Headers& headers = {}) {
    auto parsed = parse_url(url);
    if (!parsed) return std::unexpected(parsed.error());

    httplib::Client client(parsed->base);
    client.set_follow_location(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(kXaaRequestTimeoutMs / 1000,
        (kXaaRequestTimeoutMs % 1000) * 1000);

    httplib::Headers merged_headers = headers;
    if (merged_headers.find("Accept") == merged_headers.end()) {
        merged_headers.emplace("Accept", "application/json");
    }

    auto response = client.Get(parsed->path, merged_headers);
    if (!response) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA: GET request failed to " + parsed->base + parsed->path));
    }
    return FormPostResult{
        .status = response->status,
        .body = std::move(response->body)};
}

} // namespace detail

// ─── Layer 2: PRM Discovery ───────────────────────────────────────────────

/// TS REF: xaa.ts:135-165 discoverProtectedResource()
///
/// RFC 9728 PRM discovery: GET {serverUrl}/.well-known/oauth-protected-resource
/// Plus RFC 9728 §3.3 resource-mismatch validation (mix-up protection).
[[nodiscard]] inline Result<ProtectedResourceMetadata> discover_protected_resource(
    std::string_view server_url) {
    // Build PRM discovery URL: {serverUrl}/.well-known/oauth-protected-resource
    std::string base(server_url);
    // Find the path start to insert the well-known at the right place
    auto scheme_end = base.find("://");
    auto authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    auto path_start = base.find('/', authority_start);

    std::string discovery_url;
    if (path_start == std::string::npos) {
        discovery_url = base + "/.well-known/oauth-protected-resource";
    } else {
        // Insert .well-known after the authority
        std::string authority = base.substr(0, path_start);
        std::string path = base.substr(path_start);
        // Strip trailing slash from path for clean append
        while (!path.empty() && path.back() == '/') path.pop_back();
        discovery_url = authority + path + "/.well-known/oauth-protected-resource";
    }

    auto response = detail::http_get(discovery_url);
    if (!response) return std::unexpected(response.error());

    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA: PRM discovery failed: HTTP " + std::to_string(response->status)
            + " at " + discovery_url));
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: PRM discovery returned non-JSON at " + discovery_url));
    }

    auto root = parsed->root();
    if (!root.is_obj()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: PRM discovery response is not a JSON object"));
    }

    auto resource_val = root.get("resource");
    auto as_val = root.get("authorization_servers");

    std::string resource = resource_val.is_str() ? std::string(resource_val.as_str()) : "";
    if (resource.empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: PRM discovery failed: PRM missing resource"));
    }

    // Parse authorization_servers array
    std::vector<std::string> auth_servers;
    if (as_val.is_arr()) {
        as_val.iter([&](JsonVal item) {
            if (item.is_str()) auth_servers.emplace_back(item.as_str());
        });
    }
    if (auth_servers.empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: PRM discovery failed: PRM missing authorization_servers"));
    }

    // RFC 9728 §3.3 resource-mismatch validation (mix-up protection)
    if (detail::normalize_url(resource) != detail::normalize_url(server_url)) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA: PRM discovery failed: PRM resource mismatch: expected "
            + std::string(server_url) + ", got " + resource));
    }

    return ProtectedResourceMetadata{
        .resource = std::move(resource),
        .authorization_servers = std::move(auth_servers)};
}

// ─── Layer 2: AS Metadata Discovery ───────────────────────────────────────

/// TS REF: xaa.ts:178-210 discoverAuthorizationServer()
///
/// AS metadata discovery via RFC 8414 + OIDC fallback.
/// Plus RFC 8414 §3.3 issuer-mismatch validation (mix-up protection).
[[nodiscard]] inline Result<AuthorizationServerMetadata> discover_authorization_server(
    std::string_view as_url) {
    // Build AS metadata URL: {asUrl}/.well-known/oauth-authorization-server
    std::string base(as_url);
    auto scheme_end = base.find("://");
    auto authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    auto path_start = base.find('/', authority_start);

    std::string discovery_url;
    if (path_start == std::string::npos) {
        discovery_url = base + "/.well-known/oauth-authorization-server";
    } else {
        std::string authority = base.substr(0, path_start);
        std::string path = base.substr(path_start);
        while (!path.empty() && path.back() == '/') path.pop_back();
        discovery_url = authority + path + "/.well-known/oauth-authorization-server";
    }

    auto response = detail::http_get(discovery_url);
    if (!response) return std::unexpected(response.error());

    if (response->status < 200 || response->status >= 300) {
        // Fallback: try OIDC discovery URL
        std::string oidc_url;
        if (path_start == std::string::npos) {
            oidc_url = base + "/.well-known/openid-configuration";
        } else {
            std::string authority = base.substr(0, path_start);
            std::string path = base.substr(path_start);
            while (!path.empty() && path.back() == '/') path.pop_back();
            oidc_url = authority + path + "/.well-known/openid-configuration";
        }

        auto fallback = detail::http_get(oidc_url);
        if (!fallback) return std::unexpected(fallback.error());
        if (fallback->status < 200 || fallback->status >= 300) {
            return std::unexpected(Error(ErrorCode::network_error,
                "XAA: AS metadata discovery failed: HTTP "
                + std::to_string(fallback->status) + " at " + oidc_url));
        }
        response = std::move(*fallback);
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: AS metadata discovery returned non-JSON"));
    }

    auto root = parsed->root();
    if (!root.is_obj()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: AS metadata response is not a JSON object"));
    }

    AuthorizationServerMetadata meta;
    meta.issuer = root.get_string("issuer");
    meta.token_endpoint = root.get_string("token_endpoint");

    if (meta.issuer.empty() || meta.token_endpoint.empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: AS metadata discovery failed: no valid metadata at "
            + std::string(as_url)));
    }

    // RFC 8414 §3.3 issuer-mismatch validation
    if (detail::normalize_url(meta.issuer) != detail::normalize_url(as_url)) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA: AS metadata discovery failed: issuer mismatch: expected "
            + std::string(as_url) + ", got " + meta.issuer));
    }

    // RFC 8414 §3.3 / RFC 9728 §3 require HTTPS for token endpoint
    if (!meta.token_endpoint.starts_with("https://")
        && !meta.token_endpoint.starts_with("http://localhost:")
        && !meta.token_endpoint.starts_with("http://127.0.0.1:")) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA: refusing non-HTTPS token endpoint: " + meta.token_endpoint));
    }

    meta.grant_types_supported =
        detail::parse_string_array(root.get("grant_types_supported"));
    meta.token_endpoint_auth_methods_supported =
        detail::parse_string_array(root.get("token_endpoint_auth_methods_supported"));

    return meta;
}

// ─── Layer 2: Token Exchange (id_token → ID-JAG) ──────────────────────────

/// TS REF: xaa.ts:233-310 requestJwtAuthorizationGrant()
///
/// RFC 8693 Token Exchange at the IdP: id_token → ID-JAG.
/// Validates issued_token_type is id-jag.
[[nodiscard]] inline Result<JwtAuthGrantResult> request_jwt_authorization_grant(
    std::string_view token_endpoint,
    std::string_view audience,
    std::string_view resource,
    std::string_view id_token,
    std::string_view client_id,
    std::optional<std::string_view> client_secret = std::nullopt,
    std::optional<std::string_view> scope = std::nullopt) {

    std::string body = "grant_type=" + detail::url_encode(kTokenExchangeGrant)
        + "&requested_token_type=" + detail::url_encode(kIdJagTokenType)
        + "&audience=" + detail::url_encode(audience)
        + "&resource=" + detail::url_encode(resource)
        + "&subject_token=" + detail::url_encode(id_token)
        + "&subject_token_type=" + detail::url_encode(kIdTokenType)
        + "&client_id=" + detail::url_encode(client_id);

    if (client_secret && !client_secret->empty()) {
        body += "&client_secret=" + detail::url_encode(*client_secret);
    }
    if (scope && !scope->empty()) {
        body += "&scope=" + detail::url_encode(*scope);
    }

    auto response = detail::post_form(token_endpoint, body);
    if (!response) return std::unexpected(response.error());

    if (response->status < 200 || response->status >= 300) {
        std::string redacted = detail::redact_tokens(
            std::string_view(response->body).substr(0, 200));
        // 4xx → id_token rejected (invalid_grant etc.), clear cache.
        // 5xx → IdP outage, id_token may still be valid, preserve it.
        bool should_clear = response->status < 500;
        throw XaaTokenExchangeError(
            "XAA: token exchange failed: HTTP " + std::to_string(response->status)
            + ": " + redacted,
            should_clear);
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) {
        // Transient network condition (captive portal, proxy) — don't clear id_token.
        throw XaaTokenExchangeError(
            "XAA: token exchange returned non-JSON (captive portal?) at "
            + std::string(token_endpoint),
            false);
    }

    auto root = parsed->root();
    if (!root.is_obj()) {
        throw XaaTokenExchangeError(
            "XAA: token exchange response did not match expected shape: "
            + detail::redact_tokens(response->body),
            true);
    }

    auto access_token_val = root.get("access_token");
    if (!access_token_val.is_str() || access_token_val.as_str().empty()) {
        throw XaaTokenExchangeError(
            "XAA: token exchange response missing access_token",
            true);
    }

    auto issued_type_val = root.get("issued_token_type");
    if (!issued_type_val.is_str()
        || issued_type_val.as_str() != kIdJagTokenType) {
        std::string got = issued_type_val.is_str()
            ? std::string(issued_type_val.as_str()) : "(missing)";
        throw XaaTokenExchangeError(
            "XAA: token exchange returned unexpected issued_token_type: " + got,
            true);
    }

    JwtAuthGrantResult result;
    result.jwt_auth_grant = std::string(access_token_val.as_str());

    if (auto exp_val = root.get("expires_in"); exp_val.is_num()) {
        result.expires_in = static_cast<int64_t>(exp_val.as_int());
    }
    if (auto scope_val = root.get("scope"); scope_val.is_str()) {
        result.scope = std::string(scope_val.as_str());
    }

    return result;
}

// ─── Layer 2: JWT Bearer Grant (ID-JAG → access_token) ────────────────────

/// TS REF: xaa.ts:337-394 exchangeJwtAuthGrant()
///
/// RFC 7523 JWT Bearer Grant at the AS: ID-JAG → access_token.
/// auth_method defaults to client_secret_basic (SEP-990 conformance test
/// requires this). Only set client_secret_post if AS explicitly requires it.
[[nodiscard]] inline Result<XaaTokenResult> exchange_jwt_auth_grant(
    std::string_view token_endpoint,
    std::string_view assertion,
    std::string_view client_id,
    std::string_view client_secret,
    std::string_view auth_method = "client_secret_basic",
    std::optional<std::string_view> scope = std::nullopt) {

    std::string body = "grant_type=" + detail::url_encode(kJwtBearerGrant)
        + "&assertion=" + detail::url_encode(assertion);

    if (scope && !scope->empty()) {
        body += "&scope=" + detail::url_encode(*scope);
    }

    httplib::Headers headers;
    if (auth_method == "client_secret_basic") {
        auto basic_payload = detail::url_encode(client_id)
            + ":" + detail::url_encode(client_secret);
        headers.emplace("Authorization",
            "Basic " + cc::utils::crypto::base64_encode(basic_payload));
    } else {
        body += "&client_id=" + detail::url_encode(client_id);
        body += "&client_secret=" + detail::url_encode(client_secret);
    }

    auto response = detail::post_form(token_endpoint, body, headers);
    if (!response) return std::unexpected(response.error());

    if (response->status < 200 || response->status >= 300) {
        std::string redacted = detail::redact_tokens(
            std::string_view(response->body).substr(0, 200));
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA: jwt-bearer grant failed: HTTP "
            + std::to_string(response->status) + ": " + redacted));
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: jwt-bearer grant returned non-JSON (captive portal?) at "
            + std::string(token_endpoint)));
    }

    auto root = parsed->root();
    if (!root.is_obj()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: jwt-bearer response did not match expected shape"));
    }

    auto access_token_val = root.get("access_token");
    if (!access_token_val.is_str() || access_token_val.as_str().empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA: jwt-bearer response missing access_token"));
    }

    XaaTokenResult result;
    result.access_token = std::string(access_token_val.as_str());

    if (auto tt_val = root.get("token_type"); tt_val.is_str()) {
        result.token_type = std::string(tt_val.as_str());
    }

    if (auto exp_val = root.get("expires_in"); exp_val.is_num()) {
        result.expires_in = static_cast<int64_t>(exp_val.as_int());
    }
    if (auto scope_val = root.get("scope"); scope_val.is_str()) {
        result.scope = std::string(scope_val.as_str());
    }
    if (auto rt_val = root.get("refresh_token"); rt_val.is_str()) {
        result.refresh_token = std::string(rt_val.as_str());
    }

    return result;
}

// ─── Layer 3: Orchestrator ────────────────────────────────────────────────

/// TS REF: xaa.ts:426-511 performCrossAppAccess()
///
/// Full XAA flow: PRM → AS metadata → token-exchange → jwt-bearer → access_token.
/// Thin composition of the four Layer-2 ops.
///
/// @param server_url  The MCP server URL (e.g. "https://mcp.example.com/mcp")
/// @param config      IdP + AS credentials
/// @param server_name Server name for debug logging
[[nodiscard]] inline Result<XaaResult> perform_cross_app_access(
    std::string_view server_url,
    const XaaConfig& config,
    std::string_view server_name = "xaa") {

    (void)server_name;  // Used for logging in TS, we skip for now

    // 1. Discover Protected Resource Metadata (RFC 9728)
    auto prm = discover_protected_resource(server_url);
    if (!prm) return std::unexpected(prm.error());

    // 2. Discover Authorization Server metadata for each advertised AS
    //    Try each in order. grant_types_supported is OPTIONAL per RFC 8414 §2.
    AuthorizationServerMetadata as_meta;
    bool found_as = false;
    std::vector<std::string> as_errors;

    for (const auto& as_url : prm->authorization_servers) {
        auto candidate = discover_authorization_server(as_url);
        if (!candidate) {
            as_errors.push_back(as_url + ": " + candidate.error().message());
            continue;
        }
        // Check if AS supports jwt-bearer (if it advertises grant types)
        if (candidate->grant_types_supported) {
            bool supports_jwt_bearer = std::ranges::any_of(
                *candidate->grant_types_supported,
                [](const auto& g) { return g == kJwtBearerGrant; });
            if (!supports_jwt_bearer) {
                std::string supported_list;
                for (const auto& g : *candidate->grant_types_supported) {
                    if (!supported_list.empty()) supported_list += ", ";
                    supported_list += g;
                }
                as_errors.push_back(as_url + ": does not advertise jwt-bearer grant (supported: "
                    + supported_list + ")");
                continue;
            }
        }
        as_meta = std::move(*candidate);
        found_as = true;
        break;
    }

    if (!found_as) {
        std::string error_msg = "XAA: no authorization server supports jwt-bearer. Tried: ";
        for (std::size_t i = 0; i < as_errors.size(); ++i) {
            if (i > 0) error_msg += "; ";
            error_msg += as_errors[i];
        }
        return std::unexpected(Error(ErrorCode::unavailable, error_msg));
    }

    // 3. Pick auth method from what the AS advertises
    std::string auth_method = "client_secret_basic";
    auto methods = as_meta.token_endpoint_auth_methods_supported;
    if (methods) {
        bool supports_basic = std::ranges::any_of(*methods,
            [](const auto& m) { return m == "client_secret_basic"; });
        bool supports_post = std::ranges::any_of(*methods,
            [](const auto& m) { return m == "client_secret_post"; });
        if (!supports_basic && supports_post) {
            auth_method = "client_secret_post";
        }
    }

    // 4. Exchange id_token for ID-JAG at the IdP
    JwtAuthGrantResult jag;
    try {
        auto jag_result = request_jwt_authorization_grant(
            config.idp_token_endpoint,
            as_meta.issuer,
            prm->resource,
            config.idp_id_token,
            config.idp_client_id,
            config.idp_client_secret
                ? std::optional<std::string_view>(*config.idp_client_secret)
                : std::nullopt,
            config.scope
                ? std::optional<std::string_view>(*config.scope)
                : std::nullopt);
        if (!jag_result) {
            return std::unexpected(jag_result.error());
        }
        jag = std::move(*jag_result);
    } catch (const XaaTokenExchangeError& e) {
        if (e.should_clear_id_token) {
            // Clear the cached id_token — it's been rejected by the IdP
            clear_idp_id_token(config.idp_issuer);
        }
        return std::unexpected(Error(ErrorCode::permission_denied, e.what()));
    }

    // 5. Exchange ID-JAG for access_token at the AS
    auto tokens = exchange_jwt_auth_grant(
        as_meta.token_endpoint,
        jag.jwt_auth_grant,
        config.client_id,
        config.client_secret,
        auth_method,
        config.scope
            ? std::optional<std::string_view>(*config.scope)
            : std::nullopt);
    if (!tokens) return std::unexpected(tokens.error());

    // 6. Return result with AS issuer URL for persistence
    XaaResult result;
    static_cast<XaaTokenResult&>(result) = std::move(*tokens);
    result.authorization_server_url = as_meta.issuer;
    return result;
}

// ─── XAA Configuration ────────────────────────────────────────────────────

namespace detail {

/// Read XAA configuration from ~/.cc-repl/xaa-idp.txt
/// Format (one key=value per line):
///   idp_url=https://idp.example.com
///   idp_issuer=https://idp.example.com
///   client_id=cc-repl-as-client
///   client_secret=...
///   idp_client_id=cc-repl-idp-client
///   idp_token_endpoint=https://idp.example.com/oauth/token
///   scope=openid profile
[[nodiscard]] inline std::optional<XaaConfig> read_xaa_config_file() {
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;
    auto path = fs::path(home) / ".cc-repl" / "xaa-idp.txt";
    if (!fs::exists(path)) return std::nullopt;

    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    XaaConfig config;
    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line.starts_with('#')) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'
                || s.back() == '\r' || s.back() == '\n')) s.pop_back();
            auto start = s.find_first_not_of(" \t");
            if (start != std::string::npos) s = s.substr(start);
        };
        trim(key);
        trim(value);

        if (key == "idp_url" || key == "idp_issuer") {
            config.idp_issuer = value;
        } else if (key == "idp_token_endpoint") {
            config.idp_token_endpoint = value;
        } else if (key == "client_id") {
            config.client_id = value;
        } else if (key == "client_secret") {
            config.client_secret = value;
        } else if (key == "idp_client_id") {
            config.idp_client_id = value;
        } else if (key == "idp_client_secret") {
            config.idp_client_secret = value;
        } else if (key == "scope") {
            config.scope = value;
        }
    }

    // Validate minimum required fields
    if (config.idp_issuer.empty() || config.client_id.empty()) {
        return std::nullopt;
    }

    // TS REF: xaaIdpLogin.ts — if idp_client_id is not separately configured,
    // fall back to using client_id for both AS and IdP (common in test setups).
    if (config.idp_client_id.empty()) {
        config.idp_client_id = config.client_id;
    }

    // If idp_token_endpoint is not in the config file, try to discover it
    // via OIDC discovery from the issuer URL. This allows the old 3-field
    // config format (idp_url, client_id, scope) to still work.
    if (config.idp_token_endpoint.empty()) {
        auto oidc = discover_oidc(config.idp_issuer);
        if (oidc) {
            config.idp_token_endpoint = oidc->token_endpoint;
        }
    }

    // If still missing token endpoint, we can't proceed.
    if (config.idp_token_endpoint.empty()) {
        return std::nullopt;
    }

    return config;
}

} // namespace detail

/// TS REF: xaaIdpLogin.ts:47-49 getXaaIdpSettings() equivalent
///
/// Get XAA configuration for a specific MCP server.
/// Reads from ~/.cc-repl/xaa-idp.txt (file-based config for now).
[[nodiscard]] inline std::optional<XaaConfig> get_xaa_config(
    std::string_view server_name) {
    (void)server_name;  // Currently single config, future: per-server
    return detail::read_xaa_config_file();
}

// ─── Public API: authenticate_xaa ────────────────────────────────────────

/// TS REF: composed flow — authenticate_xaa() is the CPP entry point that
/// composes acquire_idp_id_token() + perform_cross_app_access() into a single
/// call that returns an access_token ready for MCP use.
///
/// Flow:
///   1. Check for cached id_token (via xaa_idp_login)
///   2. If no cached id_token, run OIDC auth_code+PKCE to get one
///   3. Run full XAA 2-legged flow: id_token → ID-JAG → access_token
///   4. Cache id_token for future use
///
/// @param config        XAA configuration (AS + IdP credentials)
/// @param mcp_server_url The MCP server URL for PRM discovery
/// @param on_auth_url   Optional callback for authorization URL display
/// @param skip_browser  If true, don't auto-open browser
[[nodiscard]] inline Result<XaaResult> authenticate_xaa(
    const XaaConfig& config,
    std::string_view mcp_server_url,
    std::function<void(const std::string&)> on_auth_url = nullptr,
    bool skip_browser = false) {

    // 1. Get id_token (cached or via browser flow)
    std::string id_token = config.idp_id_token;

    if (id_token.empty()) {
        // Try to get from cache
        auto cached = get_cached_idp_id_token(config.idp_issuer);
        if (cached && !cached->empty()) {
            id_token = *cached;
        } else {
            // Need to run IdP login
            IdpLoginOptions login_opts;
            login_opts.idp_issuer = config.idp_issuer;
            login_opts.idp_client_id = config.idp_client_id;
            login_opts.idp_client_secret = config.idp_client_secret;
            login_opts.on_authorization_url = std::move(on_auth_url);
            login_opts.skip_browser_open = skip_browser;

            auto id_token_result = acquire_idp_id_token(login_opts);
            if (!id_token_result) {
                return std::unexpected(id_token_result.error());
            }
            id_token = *id_token_result;
        }
    }

    // 2. Build full config with id_token
    XaaConfig full_config = config;
    full_config.idp_id_token = id_token;

    // 3. Run XAA orchestration
    auto result = perform_cross_app_access(mcp_server_url, full_config);
    if (!result) {
        // If token exchange failed with invalid_grant, the id_token is bad.
        // Clear it so next call re-authenticates.
        // (The clearing already happens inside request_jwt_authorization_grant
        // via XaaTokenExchangeError.should_clear_id_token)
        return std::unexpected(result.error());
    }

    return result;
}

// ─── Convenience: is_xaa_enabled ─────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:32-34 isXaaEnabled()
[[nodiscard]] inline bool is_xaa_enabled() {
    const char* enabled = std::getenv("CLAUDE_CODE_ENABLE_XAA");
    return enabled && std::string_view(enabled) == "1";
}

// ─── Logout / Token Revocation ────────────────────────────────────────────

/// TS REF: implied by clearIdpIdToken + potential revocation endpoint call
///
/// Clear XAA tokens for the given IdP issuer.
/// If revocation_endpoint is available, POST to it.
inline void logout_xaa(std::string_view idp_issuer,
    std::optional<std::string_view> revocation_endpoint = std::nullopt,
    std::optional<std::string_view> token_to_revoke = std::nullopt,
    std::optional<std::string_view> client_id = std::nullopt,
    std::optional<std::string_view> client_secret = std::nullopt) {

    // Clear cached id_token
    clear_idp_id_token(idp_issuer);

    // If we have a revocation endpoint and token, try to revoke
    if (revocation_endpoint && token_to_revoke && !revocation_endpoint->empty()
        && !token_to_revoke->empty()) {
        std::string body = "token=" + detail::url_encode(*token_to_revoke)
            + "&token_type_hint=access_token";

        if (client_id && client_secret && !client_id->empty() && !client_secret->empty()) {
            body += "&client_id=" + detail::url_encode(*client_id);
            body += "&client_secret=" + detail::url_encode(*client_secret);
        }

        // Best-effort revocation — don't propagate errors
        (void)detail::post_form(*revocation_endpoint, body);
    }
}

} // namespace cc::services::mcp
