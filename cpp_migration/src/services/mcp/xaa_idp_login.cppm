/// @file xaa_idp_login.cppm
/// @brief XAA IdP Login: acquires an OIDC id_token from an enterprise IdP via
///        authorization_code + PKCE flow, then caches it by IdP issuer.
///
/// TS REF: src/services/mcp/xaaIdpLogin.ts
///
/// This is the "one browser pop" in the XAA value prop: one IdP login → N silent
/// MCP server auths. The id_token is cached in secure storage and reused until
/// expiry.
module;
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <httplib.h>

export module cc.services.mcp.xaa_idp_login;

import cc.utils.crypto;
import cc.utils.error;
import cc.utils.json;
import cc.utils.hyperlink;
import cc.services.mcp.oauth_port;

export namespace cc::services::mcp {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::open_browser;
using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonVal;
namespace fs = std::filesystem;

// Forward declaration — defined later in this file, used by save_idp_id_token_from_jwt
[[nodiscard]] std::optional<int64_t> jwt_exp(std::string_view jwt);

// ─── Constants ────────────────────────────────────────────────────────────

// TS REF: xaaIdpLogin.ts:51
inline constexpr int kIdpLoginTimeoutMs = 5 * 60 * 1000;      // 5 minutes
inline constexpr int kIdpRequestTimeoutMs = 30000;             // 30 seconds
inline constexpr int kIdTokenExpiryBufferS = 60;               // 1 minute safety margin

// ─── Types ────────────────────────────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:55-76 IdpLoginOptions
struct IdpLoginOptions {
    std::string idp_issuer;
    std::string idp_client_id;
    std::optional<std::string> idp_client_secret;
    std::optional<uint16_t> callback_port;
    std::function<void(const std::string&)> on_authorization_url;
    bool skip_browser_open = false;
};

/// TS REF: OIDC discovery metadata (subset used by XAA)
struct OidcMetadata {
    std::string issuer;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::optional<std::string> userinfo_endpoint;
    std::optional<std::string> revocation_endpoint;
    std::optional<std::vector<std::string>> grant_types_supported;
    std::optional<std::vector<std::string>> token_endpoint_auth_methods_supported;
};

/// Result returned to auth.cppm (backward-compatible interface)
struct XaaLoginResult {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    std::optional<std::string> org_id;
    std::string authorization_server_url;
};

// ─── Issuer Key Normalization ─────────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:84-93 issuerKey()
/// Normalize an IdP issuer URL for use as a cache key.
[[nodiscard]] inline std::string issuer_key(std::string_view issuer) {
    // Try to parse as URL and normalize (lowercase host, strip trailing slash)
    std::string s(issuer);
    auto scheme_end = s.find("://");
    if (scheme_end == std::string::npos) {
        // Not a URL, just strip trailing slashes
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
    std::string path = path_start == std::string::npos ? "" : s.substr(path_start);
    // Strip trailing slashes from path
    while (!path.empty() && path.back() == '/') path.pop_back();
    std::string scheme = s.substr(0, scheme_end);
    for (auto& c : scheme) c = static_cast<char>(std::tolower(c));
    return scheme + "://" + host + path;
}

// ─── Secure Storage for IdP id_token ──────────────────────────────────────

namespace detail {

/// TS REF: xaaIdpLogin.ts:27 getSecureStorage()
/// Storage path for XAA IdP tokens: ~/.config/cc-repl/xaa/idp_tokens.json
[[nodiscard]] inline fs::path idp_token_storage_path() {
    const char* home = std::getenv("HOME");
    if (!home) return fs::temp_directory_path() / "cc-repl" / "xaa_idp_tokens.json";
    return fs::path(home) / ".config" / "cc-repl" / "xaa" / "idp_tokens.json";
}

/// TS REF: xaaIdpLogin.ts:99-107 getCachedIdpIdToken()
/// Read cached id_token for the given IdP issuer from secure storage.
/// Returns nullopt if missing or within expiry buffer.
struct CachedIdpToken {
    std::string id_token;
    int64_t expires_at_ms;  // epoch milliseconds
};

[[nodiscard]] inline std::optional<CachedIdpToken> read_cached_idp_token(
    std::string_view idp_issuer) {
    auto path = idp_token_storage_path();
    if (!fs::exists(path)) return std::nullopt;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed) return std::nullopt;

    auto root = parsed->root();
    if (!root.is_obj()) return std::nullopt;

    auto entries = root.get("mcpXaaIdp");
    if (!entries.is_obj()) return std::nullopt;

    auto key = issuer_key(idp_issuer);
    auto entry = entries.get(key);
    if (!entry.is_obj()) return std::nullopt;

    auto token_val = entry.get("idToken");
    auto expires_val = entry.get("expiresAt");
    if (!token_val.is_str() || !expires_val.is_num()) return std::nullopt;

    CachedIdpToken cached;
    cached.id_token = std::string(token_val.as_str());
    cached.expires_at_ms = static_cast<int64_t>(expires_val.as_int());

    // Check if still valid (with buffer)
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto remaining_ms = cached.expires_at_ms - now_ms;
    if (remaining_ms <= static_cast<int64_t>(kIdTokenExpiryBufferS) * 1000) {
        return std::nullopt;
    }
    return cached;
}

/// TS REF: xaaIdpLogin.ts:109-123 saveIdpIdToken()
inline void write_cached_idp_token(
    std::string_view idp_issuer,
    std::string_view id_token,
    int64_t expires_at_ms) {
    auto path = idp_token_storage_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // Read existing data
    JsonMutDoc doc;
    auto root = doc.object();

    auto existing = cc::utils::json::parse_file(path);
    if (existing && existing->root().is_obj()) {
        root = doc.copy_val(existing->root());
        doc.set_root(root);
    }

    // Ensure mcpXaaIdp object exists
    auto mcp_xaa = root.ensure_object("mcpXaaIdp");

    // Set the entry for this issuer
    auto key = issuer_key(idp_issuer);
    auto entry = doc.object();
    entry.add("idToken", doc.string(id_token));
    entry.add("expiresAt", doc.number(expires_at_ms));
    mcp_xaa.add(key, entry);

    // Write to file
    std::ofstream file(path);
    if (file.is_open()) {
        file << doc.to_string();
    }
}

/// TS REF: xaaIdpLogin.ts:143-150 clearIdpIdToken()
inline void remove_cached_idp_token(std::string_view idp_issuer) {
    auto path = idp_token_storage_path();
    if (!fs::exists(path)) return;

    auto existing = cc::utils::json::parse_file(path);
    if (!existing || !existing->root().is_obj()) return;

    JsonMutDoc doc;
    auto root = doc.copy_val(existing->root());
    doc.set_root(root);

    auto mcp_xaa = root.get("mcpXaaIdp");
    if (!mcp_xaa.is_obj()) return;

    auto key = issuer_key(idp_issuer);
    (void)mcp_xaa.remove(key);

    std::ofstream file(path);
    if (file.is_open()) {
        file << doc.to_string();
    }
}

/// TS REF: xaaIdpLogin.ts:159-172 saveIdpClientSecret() / 177-181 getIdpClientSecret()
inline void write_idp_client_secret(
    std::string_view idp_issuer,
    std::string_view client_secret) {
    auto path = idp_token_storage_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    JsonMutDoc doc;
    auto root = doc.object();

    auto existing = cc::utils::json::parse_file(path);
    if (existing && existing->root().is_obj()) {
        root = doc.copy_val(existing->root());
        doc.set_root(root);
    }

    auto config = root.ensure_object("mcpXaaIdpConfig");
    auto key = issuer_key(idp_issuer);
    auto entry = doc.object();
    entry.add("clientSecret", doc.string(client_secret));
    config.add(key, entry);

    std::ofstream file(path);
    if (file.is_open()) {
        file << doc.to_string();
    }
}

[[nodiscard]] inline std::optional<std::string> read_idp_client_secret(
    std::string_view idp_issuer) {
    auto path = idp_token_storage_path();
    if (!fs::exists(path)) return std::nullopt;

    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed) return std::nullopt;

    auto root = parsed->root();
    auto config = root.get("mcpXaaIdpConfig");
    if (!config.is_obj()) return std::nullopt;

    auto key = issuer_key(idp_issuer);
    auto entry = config.get(key);
    if (!entry.is_obj()) return std::nullopt;

    auto secret = entry.get("clientSecret");
    if (!secret.is_str()) return std::nullopt;
    std::string val(secret.as_str());
    if (val.empty()) return std::nullopt;
    return val;
}

/// TS REF: xaaIdpLogin.ts — clear client secret entry for an issuer.
inline void remove_idp_client_secret(std::string_view idp_issuer) {
    auto path = idp_token_storage_path();
    if (!fs::exists(path)) return;

    auto existing = cc::utils::json::parse_file(path);
    if (!existing || !existing->root().is_obj()) return;

    JsonMutDoc doc;
    auto root = doc.copy_val(existing->root());
    doc.set_root(root);

    auto config = root.get("mcpXaaIdpConfig");
    if (!config.is_obj()) return;

    auto key = issuer_key(idp_issuer);
    (void)config.remove(key);

    std::ofstream file(path);
    if (file.is_open()) {
        file << doc.to_string();
    }
}

} // namespace detail

// ─── Public IdP token storage API ─────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:99-107
[[nodiscard]] inline std::optional<std::string> get_cached_idp_id_token(
    std::string_view idp_issuer) {
    auto cached = detail::read_cached_idp_token(idp_issuer);
    if (!cached) return std::nullopt;
    return cached->id_token;
}

/// TS REF: xaaIdpLogin.ts:133-141 saveIdpIdTokenFromJwt()
/// Save an externally-obtained id_token. Parses JWT exp for TTL.
/// Returns the computed expiresAt in ms.
inline int64_t save_idp_id_token_from_jwt(
    std::string_view idp_issuer,
    std::string_view id_token) {
    auto exp_from_jwt = jwt_exp(id_token);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t expires_at_ms = exp_from_jwt
        ? *exp_from_jwt * 1000
        : now_ms + 3600 * 1000;
    detail::write_cached_idp_token(idp_issuer, id_token, expires_at_ms);
    return expires_at_ms;
}

/// TS REF: xaaIdpLogin.ts:143-150
inline void clear_idp_id_token(std::string_view idp_issuer) {
    detail::remove_cached_idp_token(idp_issuer);
}

/// TS REF: xaaIdpLogin.ts:177-181 getIdpClientSecret()
[[nodiscard]] inline std::optional<std::string> get_idp_client_secret(
    std::string_view idp_issuer) {
    return detail::read_idp_client_secret(idp_issuer);
}

/// TS REF: xaaIdpLogin.ts:159-172 saveIdpClientSecret()
inline void save_idp_client_secret(
    std::string_view idp_issuer,
    std::string_view client_secret) {
    detail::write_idp_client_secret(idp_issuer, client_secret);
}

/// Clear the client secret for an issuer.
inline void clear_idp_client_secret(std::string_view idp_issuer) {
    detail::remove_idp_client_secret(idp_issuer);
}

// ─── JWT Utilities ────────────────────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:252-263 jwtExp()
/// Decode the exp claim from a JWT without verifying its signature.
/// Returns nullopt if parsing fails or exp is absent.
[[nodiscard]] inline std::optional<int64_t> jwt_exp(std::string_view jwt) {
    // Split by '.'
    auto first_dot = jwt.find('.');
    if (first_dot == std::string_view::npos) return std::nullopt;
    auto second_dot = jwt.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos) return std::nullopt;

    auto payload_b64 = jwt.substr(first_dot + 1, second_dot - first_dot - 1);

    // base64url decode the payload
    // The crypto base64_decode handles both standard and URL-safe chars
    auto decoded = cc::utils::crypto::base64_decode(payload_b64);
    if (!decoded) return std::nullopt;

    std::string payload_str(decoded->begin(), decoded->end());
    auto parsed = cc::utils::json::parse(payload_str);
    if (!parsed) return std::nullopt;

    auto exp_val = parsed->root().get("exp");
    if (!exp_val.is_num()) return std::nullopt;
    return static_cast<int64_t>(exp_val.as_int());
}

// ─── OIDC Discovery ───────────────────────────────────────────────────────

namespace detail {

/// Parse a string array from a JSON value.
[[nodiscard]] inline std::optional<std::vector<std::string>> parse_string_array(
    JsonVal val) {
    if (!val.is_arr()) return std::nullopt;
    std::vector<std::string> result;
    val.iter([&](JsonVal item) {
        if (item.is_str()) result.emplace_back(item.as_str());
    });
    return result;
}

/// Parse URL into base + path components for httplib.
struct ParsedUrl {
    std::string base;   // "https://host:port"
    std::string path;   // "/path"
};

[[nodiscard]] inline std::expected<ParsedUrl, Error> parse_url(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected(Error(ErrorCode::invalid_argument,
            "URL must include scheme"));
    }
    auto authority_start = scheme_end + 3;
    auto path_start = url.find('/', authority_start);
    ParsedUrl result;
    result.base = path_start == std::string_view::npos
        ? std::string(url)
        : std::string(url.substr(0, path_start));
    result.path = path_start == std::string_view::npos
        ? "/"
        : std::string(url.substr(path_start));
    return result;
}

} // namespace detail

/// TS REF: xaaIdpLogin.ts:202-237 discoverOidc()
///
/// OIDC Discovery §4.1: {issuer}/.well-known/openid-configuration
/// Path APPEND, not replace — trailing-slash base + relative path is correct.
[[nodiscard]] inline Result<OidcMetadata> discover_oidc(
    std::string_view idp_issuer) {
    // Build discovery URL: append .well-known/openid-configuration to issuer
    std::string base(idp_issuer);
    if (!base.empty() && base.back() != '/') base += '/';
    std::string discovery_url = base + ".well-known/openid-configuration";

    auto parsed = detail::parse_url(discovery_url);
    if (!parsed) return std::unexpected(parsed.error());

    httplib::Client client(parsed->base);
    client.set_follow_location(true);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(kIdpRequestTimeoutMs / 1000, (kIdpRequestTimeoutMs % 1000) * 1000);

    auto response = client.Get(parsed->path, httplib::Headers{
        {"Accept", "application/json"}});
    if (!response) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA IdP: OIDC discovery request failed for " + discovery_url));
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA IdP: OIDC discovery failed: HTTP " + std::to_string(response->status)
            + " at " + discovery_url));
    }

    // Captive portals and proxy auth pages return 200 with HTML.
    auto body_parsed = cc::utils::json::parse(response->body);
    if (!body_parsed) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA IdP: OIDC discovery returned non-JSON at " + discovery_url
            + " (captive portal or proxy?)"));
    }

    auto root = body_parsed->root();
    if (!root.is_obj()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA IdP: invalid OIDC metadata: not a JSON object"));
    }

    OidcMetadata meta;
    meta.issuer = root.get_string("issuer");
    meta.authorization_endpoint = root.get_string("authorization_endpoint");
    meta.token_endpoint = root.get_string("token_endpoint");

    if (meta.token_endpoint.empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA IdP: invalid OIDC metadata: missing token_endpoint"));
    }

    // Validate HTTPS for token endpoint
    if (!meta.token_endpoint.starts_with("https://")
        && !meta.token_endpoint.starts_with("http://localhost:")
        && !meta.token_endpoint.starts_with("http://127.0.0.1:")) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA IdP: refusing non-HTTPS token endpoint: " + meta.token_endpoint));
    }

    if (auto val = root.get("userinfo_endpoint"); val.is_str()) {
        meta.userinfo_endpoint = std::string(val.as_str());
    }
    if (auto val = root.get("revocation_endpoint"); val.is_str()) {
        meta.revocation_endpoint = std::string(val.as_str());
    }
    meta.grant_types_supported = detail::parse_string_array(root.get("grant_types_supported"));
    meta.token_endpoint_auth_methods_supported =
        detail::parse_string_array(root.get("token_endpoint_auth_methods_supported"));

    return meta;
}

// ─── URL Encoding ─────────────────────────────────────────────────────────

namespace detail {

/// TS REF: standard URL percent-encoding
[[nodiscard]] inline std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return encoded;
}

/// Generate a random state string for CSRF protection.
[[nodiscard]] inline std::string generate_state() {
    auto bytes = cc::utils::crypto::random_bytes(32);
    return cc::utils::crypto::base64_encode(bytes);
}

} // namespace detail

// ─── Callback Server ──────────────────────────────────────────────────────

namespace detail {

/// Result from the OAuth callback: the authorization code, or an error.
struct CallbackOutcome {
    std::string code;
    std::string state;
};

/// RAII wrapper for a listening socket.
class CallbackServer {
public:
    explicit CallbackServer(uint16_t port) : port_(port) {}
    ~CallbackServer() { stop(); }

    CallbackServer(const CallbackServer&) = delete;
    CallbackServer& operator=(const CallbackServer&) = delete;

    /// Start listening on 127.0.0.1:{port_}. Returns the actual bound port.
    std::expected<uint16_t, std::string> start() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return std::unexpected("Failed to create socket");

        int opt = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);

        if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return std::unexpected(std::string("Failed to bind port ")
                + std::to_string(port_) + ": " + std::strerror(errno));
        }

        // If port was 0, get the assigned port
        if (port_ == 0) {
            struct sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            ::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&bound), &len);
            port_ = ntohs(bound.sin_port);
        }

        if (::listen(fd_, 1) < 0) {
            ::close(fd_);
            fd_ = -1;
            return std::unexpected("Failed to listen");
        }
        return port_;
    }

    /// Wait for the /callback request. Returns the code and state from query params.
    /// Timeout is in milliseconds.
    std::expected<CallbackOutcome, std::string> wait_for_callback(int timeout_ms) {
        if (fd_ < 0) return std::unexpected("Server not running");

        // Set receive timeout on the accept socket
        struct timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        // Use select() for timeout on accept
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd_, &readfds);

        int ret = ::select(fd_ + 1, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            return std::unexpected(std::string("select() failed: ") + std::strerror(errno));
        }
        if (ret == 0) {
            return std::unexpected("XAA IdP: login timed out");
        }

        int client_fd = ::accept(fd_, nullptr, nullptr);
        if (client_fd < 0) {
            return std::unexpected(std::string("accept() failed: ") + std::strerror(errno));
        }

        // Read the HTTP request
        std::vector<char> buf(8192);
        auto n = ::recv(client_fd, buf.data(), buf.size() - 1, 0);
        if (n <= 0) {
            ::close(client_fd);
            return std::unexpected("Failed to read callback request");
        }
        buf[n] = '\0';
        std::string request(buf.data());

        // Parse the request line: GET /callback?code=...&state=... HTTP/1.1
        auto path_start = request.find(' ');
        if (path_start == std::string::npos) {
            send_error_response(client_fd, 400, "Bad Request");
            ::close(client_fd);
            return std::unexpected("Malformed callback request");
        }
        auto path_end = request.find(' ', path_start + 1);
        if (path_end == std::string::npos) {
            send_error_response(client_fd, 400, "Bad Request");
            ::close(client_fd);
            return std::unexpected("Malformed callback request");
        }

        std::string full_path = request.substr(path_start + 1, path_end - path_start - 1);

        // Parse path and query
        auto query_start = full_path.find('?');
        std::string path = query_start == std::string::npos
            ? full_path : full_path.substr(0, query_start);

        if (path != "/callback") {
            send_error_response(client_fd, 404, "Not Found");
            ::close(client_fd);
            return std::unexpected("Callback received on wrong path: " + path);
        }

        // Parse query parameters
        std::unordered_map<std::string, std::string> params;
        if (query_start != std::string::npos) {
            std::string query = full_path.substr(query_start + 1);
            std::istringstream qs(query);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                auto eq = pair.find('=');
                if (eq != std::string::npos) {
                    params[pair.substr(0, eq)] = pair.substr(eq + 1);
                }
            }
        }

        // Check for error
        auto it_err = params.find("error");
        if (it_err != params.end()) {
            std::string err_msg = "XAA IdP: " + it_err->second;
            auto it_desc = params.find("error_description");
            if (it_desc != params.end()) {
                err_msg += " — " + it_desc->second;
            }
            send_html_response(client_fd, 400,
                "<html><body><h3>IdP login failed</h3><p>"
                + it_err->second + "</p></body></html>");
            ::close(client_fd);
            return std::unexpected(err_msg);
        }

        // Validate state
        auto it_state = params.find("state");
        auto it_code = params.find("code");

        if (it_code == params.end() || it_code->second.empty()) {
            send_html_response(client_fd, 400,
                "<html><body><h3>Missing code</h3></body></html>");
            ::close(client_fd);
            return std::unexpected("XAA IdP: callback missing code");
        }

        // Send success response
        send_html_response(client_fd, 200,
            "<html><body><h3>IdP login complete — you can close this window.</h3></body></html>");
        ::close(client_fd);

        CallbackOutcome result;
        result.code = it_code->second;
        result.state = it_state != params.end() ? it_state->second : "";
        return result;
    }

    void stop() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] uint16_t port() const { return port_; }

private:
    static void send_html_response(int fd, int status, std::string_view body) {
        std::string status_text = (status == 200) ? "OK" : "Error";
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + std::string(body);
        ::send(fd, response.data(), response.size(), 0);
    }

    static void send_error_response(int fd, int status, std::string_view text) {
        send_html_response(fd, status,
            "<html><body><h3>" + std::string(text) + "</h3></body></html>");
    }

    int fd_{-1};
    uint16_t port_;
};

} // namespace detail

// ─── Main: acquireIdpIdToken ──────────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts:401-487 acquireIdpIdToken()
///
/// Acquire an id_token from the IdP: return cached if valid, otherwise run
/// the full OIDC authorization_code + PKCE flow (one browser pop).
[[nodiscard]] inline Result<std::string> acquire_idp_id_token(
    const IdpLoginOptions& opts) {
    // 1. Check cache first
    auto cached = get_cached_idp_id_token(opts.idp_issuer);
    if (cached && !cached->empty()) {
        return *cached;
    }

    // 2. OIDC discovery
    auto metadata = discover_oidc(opts.idp_issuer);
    if (!metadata) return std::unexpected(metadata.error());

    // 3. Select callback port
    uint16_t port;
    if (opts.callback_port) {
        port = *opts.callback_port;
    } else {
        auto port_result = find_available_oauth_port();
        if (!port_result) {
            return std::unexpected(Error(ErrorCode::unavailable,
                "XAA IdP: " + port_result.error()));
        }
        port = *port_result;
    }

    std::string redirect_uri = "http://localhost:" + std::to_string(port) + "/callback";

    // 4. Generate PKCE parameters and state
    auto code_verifier = cc::utils::crypto::generate_code_verifier();
    auto code_challenge = cc::utils::crypto::generate_code_challenge(code_verifier);
    auto state = detail::generate_state();

    // 5. Build authorization URL
    std::string auth_url = metadata->authorization_endpoint
        + "?response_type=code"
        + "&client_id=" + detail::url_encode(opts.idp_client_id)
        + "&redirect_uri=" + detail::url_encode(redirect_uri)
        + "&scope=openid"
        + "&code_challenge=" + detail::url_encode(code_challenge)
        + "&code_challenge_method=S256"
        + "&state=" + detail::url_encode(state);

    // 6. Start callback server
    detail::CallbackServer server(port);
    auto bound_port = server.start();
    if (!bound_port) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA IdP: failed to start callback server: " + bound_port.error()));
    }

    // 7. Notify caller and open browser (after server is bound)
    if (opts.on_authorization_url) {
        opts.on_authorization_url(auth_url);
    }
    if (!opts.skip_browser_open) {
        (void)open_browser(auth_url);
    }

    // 8. Wait for callback
    auto callback = server.wait_for_callback(kIdpLoginTimeoutMs);
    if (!callback) {
        return std::unexpected(Error(ErrorCode::timeout,
            "XAA IdP: " + callback.error()));
    }

    // 9. Validate state
    if (callback->state != state) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA IdP: state mismatch (possible CSRF)"));
    }

    // 10. Exchange authorization code for tokens
    // Determine auth method: if client_secret provided, check what IdP supports
    std::string body = std::string("grant_type=authorization_code")
        + "&code=" + detail::url_encode(callback->code)
        + "&redirect_uri=" + detail::url_encode(redirect_uri)
        + "&code_verifier=" + detail::url_encode(code_verifier)
        + "&client_id=" + detail::url_encode(opts.idp_client_id);

    httplib::Headers headers{
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"}};

    // If client_secret is available and IdP supports basic auth, use it
    if (opts.idp_client_secret && !opts.idp_client_secret->empty()) {
        auto methods = metadata->token_endpoint_auth_methods_supported;
        bool supports_basic = !methods || std::ranges::any_of(*methods,
            [](const auto& m) { return m == "client_secret_basic"; });
        bool supports_post = methods && std::ranges::any_of(*methods,
            [](const auto& m) { return m == "client_secret_post"; });

        if (supports_basic || !methods) {
            // Default to basic auth
            auto basic_payload = detail::url_encode(opts.idp_client_id)
                + ":" + detail::url_encode(*opts.idp_client_secret);
            headers.emplace("Authorization",
                "Basic " + cc::utils::crypto::base64_encode(basic_payload));
        } else if (supports_post) {
            body += "&client_secret=" + detail::url_encode(*opts.idp_client_secret);
        }
    }

    auto token_endpoint = detail::parse_url(metadata->token_endpoint);
    if (!token_endpoint) return std::unexpected(token_endpoint.error());

    httplib::Client token_client(token_endpoint->base);
    token_client.set_follow_location(true);
    token_client.set_connection_timeout(10, 0);
    token_client.set_read_timeout(kIdpRequestTimeoutMs / 1000,
        (kIdpRequestTimeoutMs % 1000) * 1000);

    auto token_response = token_client.Post(
        token_endpoint->path, headers, body,
        "application/x-www-form-urlencoded");
    if (!token_response) {
        return std::unexpected(Error(ErrorCode::network_error,
            "XAA IdP: token exchange request failed"));
    }
    if (token_response->status < 200 || token_response->status >= 300) {
        return std::unexpected(Error(ErrorCode::permission_denied,
            "XAA IdP: token exchange failed: HTTP "
            + std::to_string(token_response->status) + ": "
            + token_response->body.substr(0, 200)));
    }

    auto token_parsed = cc::utils::json::parse(token_response->body);
    if (!token_parsed) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA IdP: token exchange returned non-JSON"));
    }

    auto token_root = token_parsed->root();
    auto id_token_val = token_root.get("id_token");
    if (!id_token_val.is_str() || id_token_val.as_str().empty()) {
        return std::unexpected(Error(ErrorCode::parse_error,
            "XAA IdP: token response missing id_token (check scope=openid)"));
    }

    std::string id_token(id_token_val.as_str());

    // 11. Compute expiry and cache
    auto exp_from_jwt = jwt_exp(id_token);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t expires_at_ms;
    if (exp_from_jwt) {
        expires_at_ms = *exp_from_jwt * 1000;
    } else {
        auto expires_in_val = token_root.get("expires_in");
        int64_t expires_in = expires_in_val.is_num()
            ? static_cast<int64_t>(expires_in_val.as_int()) : 3600;
        expires_at_ms = now_ms + expires_in * 1000;
    }

    detail::write_cached_idp_token(opts.idp_issuer, id_token, expires_at_ms);

    // 12. Save client secret if provided
    if (opts.idp_client_secret && !opts.idp_client_secret->empty()) {
        detail::write_idp_client_secret(opts.idp_issuer, *opts.idp_client_secret);
    }

    return id_token;
}

// ─── Backward-Compatible Wrapper ──────────────────────────────────────────

/// TS REF: xaaIdpLogin.ts acquireIdpIdToken() — convenience wrapper for the
/// /mcp xaa login command which just does the IdP login and caches the id_token.
///
/// This is called from mcp_cmd.cppm's execute_xaa_login(). The full MCP flow
/// (id_token → access_token via token exchange + jwt bearer) is handled by
/// authenticate_xaa() in xaa.cppm which calls acquire_idp_id_token() internally.
///
/// @param idp_url   The IdP issuer URL
/// @param client_id The IdP client ID
/// @param scope     Optional scope (defaults to "openid")
[[nodiscard]] inline std::expected<XaaLoginResult, std::string> perform_xaa_login(
    std::string_view idp_url,
    std::string_view client_id,
    std::optional<std::string_view> scope = std::nullopt) {

    IdpLoginOptions login_opts;
    login_opts.idp_issuer = std::string(idp_url);
    login_opts.idp_client_id = std::string(client_id);
    if (scope && !scope->empty()) {
        // scope is passed to the authorization URL; for IdP login it's
        // typically "openid" which is already the default in acquire_idp_id_token.
        // We don't have a direct scope field in IdpLoginOptions — the scope is
        // hardcoded to "openid" in the auth URL. For custom scopes, callers
        // should use acquire_idp_id_token() directly.
        (void)scope;
    }

    // Try to get id_token (may trigger browser flow)
    auto id_token_result = acquire_idp_id_token(login_opts);
    if (!id_token_result) {
        return std::unexpected(id_token_result.error().message());
    }

    XaaLoginResult result;
    result.id_token = *id_token_result;
    result.access_token = *id_token_result;  // for backward compat
    result.authorization_server_url = std::string(idp_url);
    return result;
}

} // namespace cc::services::mcp
