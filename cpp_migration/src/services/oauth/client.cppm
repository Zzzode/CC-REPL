// OAuth 2.0 Client - Authorization code flow with PKCE, token management, keychain
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

#include <httplib.h>

export module cc.services.oauth.client;


export namespace cc::services::oauth {

// OAuth error types
enum class OAuthError {
    AuthorizationFailed,
    TokenExchangeFailed,
    TokenRefreshFailed,
    TokenExpired,
    TokenInvalid,
    NetworkError,
    KeychainError,
    CallbackServerError,
    PkceError,
    InvalidState,
    InvalidGrant,
};

// OAuth token pair
struct TokenPair {
    std::string access_token;
    std::string refresh_token;
    std::string token_type = "Bearer";
    int expires_in = 3600; // seconds
    std::chrono::system_clock::time_point issued_at;
    std::string scope;

    // Check if the token has expired (with 5 minute buffer)
    [[nodiscard]] bool is_expired() const {
        auto now = std::chrono::system_clock::now();
        auto expiry = issued_at + std::chrono::seconds(expires_in) - std::chrono::minutes(5);
        return now >= expiry;
    }

    // Get remaining lifetime in seconds
    [[nodiscard]] int remaining_seconds() const {
        auto now = std::chrono::system_clock::now();
        auto expiry = issued_at + std::chrono::seconds(expires_in);
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(expiry - now);
        return std::max(0, static_cast<int>(remaining.count()));
    }
};

// PKCE (Proof Key for Code Exchange) parameters
struct PkceChallenge {
    std::string code_verifier;    // Random 43-128 character string
    std::string code_challenge;   // S256 hash of verifier
    std::string method = "S256";  // Always S256
};

// OAuth client configuration
struct OAuthConfig {
    std::string client_id;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string redirect_uri = "http://localhost:19485/callback";
    std::vector<std::string> scopes;
    std::string keychain_service = "claude-code-oauth";
    int callback_port = 19485;
    std::chrono::seconds auth_timeout{300}; // 5 minutes to complete auth
};

// Authorization request state
struct AuthorizationRequest {
    std::string state;         // CSRF protection nonce
    PkceChallenge pkce;        // PKCE challenge/verifier pair
    std::string redirect_uri;
    std::chrono::steady_clock::time_point created_at;
};

struct CallbackResult {
    std::string code;
    std::string state;
};

// Keychain storage interface (macOS Security framework)
class KeychainStore {
public:
    explicit KeychainStore(std::string service_name)
        : service_name_(std::move(service_name)) {}

    // Store a token pair in the system keychain
    std::expected<void, OAuthError> store(const std::string& account, const TokenPair& token) {
        auto data = serialize_token(token);
        // macOS: SecItemAdd / SecItemUpdate via Security framework
        // Simplified: store to file-based fallback
        auto path = token_path(account);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return std::unexpected(OAuthError::KeychainError);
        file.write(data.c_str(), static_cast<std::streamsize>(data.size()));
        // Set restrictive permissions (owner read/write only)
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | 
                                           std::filesystem::perms::owner_write);
        return {};
    }

    // Retrieve a stored token pair
    std::expected<TokenPair, OAuthError> retrieve(const std::string& account) {
        auto path = token_path(account);
        if (!std::filesystem::exists(path)) {
            return std::unexpected(OAuthError::TokenInvalid);
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return std::unexpected(OAuthError::KeychainError);

        std::string data((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
        return deserialize_token(data);
    }

    // Delete stored token
    std::expected<void, OAuthError> remove(const std::string& account) {
        auto path = token_path(account);
        if (std::filesystem::exists(path)) {
            std::filesystem::remove(path);
        }
        return {};
    }

private:
    [[nodiscard]] std::filesystem::path token_path(const std::string& account) const {
        auto home = std::getenv("HOME");
        return std::filesystem::path(home ? home : "/tmp") / ".claude" / "tokens" / account;
    }

    [[nodiscard]] static std::string serialize_token(const TokenPair& token) {
        auto issued_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            token.issued_at.time_since_epoch()).count();
        return std::format("{}|{}|{}|{}|{}|{}",
            token.access_token, token.refresh_token, token.token_type,
            token.expires_in, issued_epoch, token.scope);
    }

    [[nodiscard]] static std::expected<TokenPair, OAuthError> deserialize_token(const std::string& data) {
        TokenPair token;
        // Parse pipe-delimited fields
        std::vector<std::string> fields;
        std::istringstream stream(data);
        std::string field;
        while (std::getline(stream, field, '|')) {
            fields.push_back(field);
        }
        if (fields.size() < 6) return std::unexpected(OAuthError::TokenInvalid);

        token.access_token = fields[0];
        token.refresh_token = fields[1];
        token.token_type = fields[2];
        token.expires_in = std::stoi(fields[3]);
        auto epoch = std::stoll(fields[4]);
        token.issued_at = std::chrono::system_clock::time_point(std::chrono::seconds(epoch));
        token.scope = fields[5];
        return token;
    }

    std::string service_name_;
};

// PKCE generator utility
class PkceGenerator {
public:
    // Generate a new PKCE challenge pair
    [[nodiscard]] static PkceChallenge generate() {
        PkceChallenge challenge;
        challenge.code_verifier = generate_verifier();
        challenge.code_challenge = compute_s256_challenge(challenge.code_verifier);
        challenge.method = "S256";
        return challenge;
    }

private:
    // Generate a cryptographically random verifier (43-128 chars, base64url)
    [[nodiscard]] static std::string generate_verifier() {
        static constexpr std::string_view charset =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dist(0, charset.size() - 1);

        std::string verifier;
        verifier.reserve(64);
        for (int i = 0; i < 64; ++i) {
            verifier += charset[dist(gen)];
        }
        return verifier;
    }

    // Compute S256 challenge: BASE64URL(SHA256(verifier))
    [[nodiscard]] static std::string compute_s256_challenge(const std::string& verifier) {
        // Compute SHA-256 digest
        std::array<unsigned char, 32> digest{};
#ifdef __APPLE__
        CC_SHA256(verifier.data(), static_cast<CC_LONG>(verifier.size()), digest.data());
#else
        SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
               verifier.size(), digest.data());
#endif
        // Base64url encode (no padding)
        return base64url_encode(digest.data(), digest.size());
    }

    // Base64url encoding (RFC 4648 §5, no padding)
    [[nodiscard]] static std::string base64url_encode(const unsigned char* data, size_t len) {
        static constexpr std::string_view table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string result;
        result.reserve((len * 4 + 2) / 3);

        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = static_cast<uint32_t>(data[i]) << 16;
            if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
            if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

            result += table[(n >> 18) & 0x3F];
            result += table[(n >> 12) & 0x3F];
            if (i + 1 < len) result += table[(n >> 6) & 0x3F];
            if (i + 2 < len) result += table[n & 0x3F];
        }
        return result;
    }
};

// Local HTTP callback server for OAuth redirect
class CallbackServer {
public:
    explicit CallbackServer(int port) : port_(port) {}

    // Start listening for the OAuth callback
    std::expected<void, OAuthError> start() {
        // Create TCP socket
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return std::unexpected(OAuthError::CallbackServerError);

        // Allow port reuse
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(server_fd_);
            return std::unexpected(OAuthError::CallbackServerError);
        }

        if (listen(server_fd_, 1) < 0) {
            close(server_fd_);
            return std::unexpected(OAuthError::CallbackServerError);
        }

        running_ = true;
        return {};
    }

    // Wait for the callback and extract the authorization response.
    std::expected<CallbackResult, OAuthError> wait_for_callback(std::chrono::seconds timeout) {
        if (!running_) return std::unexpected(OAuthError::CallbackServerError);

        // Set socket timeout
        struct timeval tv{};
        tv.tv_sec = timeout.count();
        setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Accept incoming connection
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            stop();
            return std::unexpected(OAuthError::AuthorizationFailed);
        }

        // Read HTTP request
        char buffer[4096];
        auto bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            close(client_fd);
            stop();
            return std::unexpected(OAuthError::AuthorizationFailed);
        }
        buffer[bytes_read] = '\0';

        // Send success response to browser
        constexpr auto response = 
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<html><body><h1>Authorization successful!</h1>"
            "<p>You can close this window.</p></body></html>";
        write(client_fd, response, strlen(response));
        close(client_fd);
        stop();

        return extract_callback(std::string_view(buffer, static_cast<size_t>(bytes_read)));
    }

    // Wait for the callback and extract only the authorization code.
    std::expected<std::string, OAuthError> wait_for_code(std::chrono::seconds timeout) {
        auto callback = wait_for_callback(timeout);
        if (!callback) return std::unexpected(callback.error());
        return callback->code;
    }

    void stop() {
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        running_ = false;
    }

    ~CallbackServer() { stop(); }

private:
    // Extract authorization code and state from the callback URL.
    [[nodiscard]] static std::expected<CallbackResult, OAuthError> extract_callback(std::string_view request) {
        // Parse "GET /callback?code=xxx&state=yyy HTTP/1.1"
        if (request.find("error=") != std::string_view::npos) {
            return std::unexpected(OAuthError::AuthorizationFailed);
        }
        auto code = extract_param(request, "code");
        if (!code || code->empty()) return std::unexpected(OAuthError::InvalidGrant);
        auto state = extract_param(request, "state");
        if (!state || state->empty()) return std::unexpected(OAuthError::InvalidState);
        return CallbackResult{.code = *code, .state = *state};
    }

    [[nodiscard]] static std::optional<std::string> extract_param(
        std::string_view request, std::string_view key) {
        auto query_start = request.find('?');
        if (query_start == std::string_view::npos) return std::nullopt;
        auto query_end = request.find(' ', query_start);
        auto query = request.substr(
            query_start + 1,
            query_end == std::string_view::npos ? std::string_view::npos : query_end - query_start - 1);

        std::string pattern(key);
        pattern.push_back('=');
        size_t pos = 0;
        while (pos < query.size()) {
            auto next = query.find('&', pos);
            auto part = query.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
            if (part.starts_with(pattern)) {
                return url_decode(part.substr(pattern.size()));
            }
            if (next == std::string_view::npos) break;
            pos = next + 1;
        }
        return std::nullopt;
    }

    [[nodiscard]] static int hex_value(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    }

    [[nodiscard]] static std::string url_decode(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '+' ) {
                out.push_back(' ');
            } else if (value[i] == '%' && i + 2 < value.size()) {
                int hi = hex_value(value[i + 1]);
                int lo = hex_value(value[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                } else {
                    out.push_back(value[i]);
                }
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    int port_;
    int server_fd_ = -1;
    bool running_ = false;
};

// Main OAuth 2.0 Client
class OAuthClient {
public:
    explicit OAuthClient(OAuthConfig config)
        : config_(std::move(config))
        , keychain_(config_.keychain_service) {}

    // Initiate the authorization flow - returns the URL to open in browser
    [[nodiscard]] std::expected<std::string, OAuthError> start_authorization() {
        // Generate PKCE challenge
        auto pkce = PkceGenerator::generate();

        // Generate state parameter for CSRF protection
        auto state = generate_state();

        // Store pending request
        pending_request_ = AuthorizationRequest{
            .state = state,
            .pkce = pkce,
            .redirect_uri = config_.redirect_uri,
            .created_at = std::chrono::steady_clock::now(),
        };

        // Build authorization URL
        std::string scope_str;
        for (size_t i = 0; i < config_.scopes.size(); ++i) {
            if (i > 0) scope_str += " ";
            scope_str += config_.scopes[i];
        }

        auto url = std::format(
            "{}?response_type=code&client_id={}&redirect_uri={}"
            "&scope={}&state={}&code_challenge={}&code_challenge_method=S256",
            config_.authorization_endpoint, form_encode(config_.client_id),
            form_encode(config_.redirect_uri), form_encode(scope_str),
            form_encode(state), form_encode(pkce.code_challenge));

        return url;
    }

    // Complete the authorization flow - exchange code for tokens
    std::expected<TokenPair, OAuthError> complete_authorization(const std::string& code, const std::string& state) {
        // Verify state matches
        if (!pending_request_ || pending_request_->state != state) {
            return std::unexpected(OAuthError::InvalidState);
        }

        // Exchange authorization code for tokens
        auto token = exchange_code(code, pending_request_->pkce.code_verifier);
        if (!token) return token;

        // Store in keychain
        auto store_result = keychain_.store(config_.client_id, *token);
        if (!store_result) return std::unexpected(OAuthError::KeychainError);

        pending_request_.reset();
        return token;
    }

    // Run the full authorization flow with local callback server
    std::expected<TokenPair, OAuthError> authorize_interactive() {
        // Start callback server
        CallbackServer server(config_.callback_port);
        auto start_result = server.start();
        if (!start_result) return std::unexpected(start_result.error());

        // Get authorization URL and open it in the user's browser.
        auto url_result = start_authorization();
        if (!url_result) return std::unexpected(url_result.error());
        auto opened = open_authorization_url(*url_result);
        if (!opened) return std::unexpected(opened.error());

        // Wait for callback
        auto callback = server.wait_for_callback(config_.auth_timeout);
        if (!callback) return std::unexpected(callback.error());

        // Complete the flow
        return complete_authorization(callback->code, callback->state);
    }

    // Get a valid access token (refreshing if needed)
    std::expected<std::string, OAuthError> get_valid_token() {
        auto stored = keychain_.retrieve(config_.client_id);
        if (!stored) return std::unexpected(OAuthError::TokenInvalid);

        if (stored->is_expired()) {
            // Attempt refresh
            auto refreshed = refresh_token(stored->refresh_token);
            if (!refreshed) return std::unexpected(refreshed.error());
            return refreshed->access_token;
        }
        return stored->access_token;
    }

    // Refresh an expired token
    std::expected<TokenPair, OAuthError> refresh_token(const std::string& refresh_tok) {
        // Build token refresh request
        auto body = std::format(
            "grant_type=refresh_token&refresh_token={}&client_id={}",
            form_encode(refresh_tok), form_encode(config_.client_id));

        // POST to token endpoint
        auto [scheme, host, port, path] = parse_url(config_.token_endpoint);
        httplib::Client cli(scheme + "://" + host, port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);

        auto res = cli.Post(path, body, "application/x-www-form-urlencoded");
        if (!res || res->status != 200) {
            return std::unexpected(OAuthError::TokenRefreshFailed);
        }

        auto token = parse_token_response(res->body);
        if (!token) return token;

        // Preserve refresh token if server didn't issue a new one
        if (token->refresh_token.empty()) {
            token->refresh_token = refresh_tok;
        }

        auto store_result = keychain_.store(config_.client_id, *token);
        if (!store_result) return std::unexpected(OAuthError::KeychainError);

        return token;
    }

    // Revoke stored tokens
    std::expected<void, OAuthError> logout() {
        return keychain_.remove(config_.client_id);
    }

    // Check if user is authenticated
    [[nodiscard]] bool is_authenticated() {
        auto stored = keychain_.retrieve(config_.client_id);
        return stored.has_value();
    }

private:
    // Exchange authorization code for token pair
    std::expected<TokenPair, OAuthError> exchange_code(
        const std::string& code, const std::string& code_verifier) {
        // Build form-encoded body
        auto body = std::format(
            "grant_type=authorization_code&code={}&redirect_uri={}"
            "&client_id={}&code_verifier={}",
            form_encode(code), form_encode(config_.redirect_uri),
            form_encode(config_.client_id), form_encode(code_verifier));

        // POST to token endpoint
        auto [scheme, host, port, path] = parse_url(config_.token_endpoint);
        httplib::Client cli(scheme + "://" + host, port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);

        auto res = cli.Post(path, body, "application/x-www-form-urlencoded");
        if (!res || res->status != 200) {
            return std::unexpected(OAuthError::TokenExchangeFailed);
        }

        return parse_token_response(res->body);
    }

    // Generate random state parameter
    [[nodiscard]] static std::string generate_state() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 15);
        static constexpr std::string_view hex_chars = "0123456789abcdef";

        std::string state;
        state.reserve(32);
        for (int i = 0; i < 32; ++i) {
            state += hex_chars[static_cast<size_t>(dist(gen))];
        }
        return state;
    }

    [[nodiscard]] static std::string shell_quote(std::string_view value) {
        std::string out = "'";
        for (char ch : value) {
            if (ch == '\'') {
                out += "'\\''";
            } else {
                out.push_back(ch);
            }
        }
        out.push_back('\'');
        return out;
    }

    [[nodiscard]] static std::expected<void, OAuthError> open_authorization_url(const std::string& url) {
        std::cout << "Opening browser for OAuth authorization.\n";
        std::cout << "If the browser does not open, visit this URL:\n" << url << "\n";
#if defined(__APPLE__)
        auto command = std::string("open ") + shell_quote(url);
#elif defined(_WIN32)
        auto command = std::string("rundll32 url.dll,FileProtocolHandler ") + shell_quote(url);
#else
        auto command = std::string("xdg-open ") + shell_quote(url);
#endif
        (void)std::system(command.c_str());
        return {};
    }

    [[nodiscard]] static std::string form_encode(std::string_view value) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
            bool safe =
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~';
            if (safe) {
                out.push_back(static_cast<char>(ch));
            } else if (ch == ' ') {
                out.push_back('+');
            } else {
                out.push_back('%');
                out.push_back(hex[(ch >> 4) & 0x0F]);
                out.push_back(hex[ch & 0x0F]);
            }
        }
        return out;
    }

    // Parse a URL into (scheme, host, port, path) components
    struct UrlParts { std::string scheme; std::string host; int port; std::string path; };
    [[nodiscard]] static UrlParts parse_url(const std::string& url) {
        UrlParts parts;
        size_t scheme_end = url.find("://");
        if (scheme_end != std::string::npos) {
            parts.scheme = url.substr(0, scheme_end);
            scheme_end += 3;
        } else {
            parts.scheme = "https";
            scheme_end = 0;
        }

        auto path_start = url.find('/', scheme_end);
        std::string authority;
        if (path_start != std::string::npos) {
            authority = url.substr(scheme_end, path_start - scheme_end);
            parts.path = url.substr(path_start);
        } else {
            authority = url.substr(scheme_end);
            parts.path = "/";
        }

        auto colon_pos = authority.find(':');
        if (colon_pos != std::string::npos) {
            parts.host = authority.substr(0, colon_pos);
            parts.port = std::stoi(authority.substr(colon_pos + 1));
        } else {
            parts.host = authority;
            parts.port = (parts.scheme == "https") ? 443 : 80;
        }
        return parts;
    }

    // Parse JSON token response body into TokenPair
    [[nodiscard]] static std::expected<TokenPair, OAuthError> parse_token_response(const std::string& body) {
        TokenPair token;
        token.issued_at = std::chrono::system_clock::now();

        // Simple JSON field extraction (fields are simple string/int values)
        auto extract_string = [&](const std::string& key) -> std::string {
            auto pattern = "\"" + key + "\"";
            auto pos = body.find(pattern);
            if (pos == std::string::npos) return {};
            pos = body.find(':', pos + pattern.size());
            if (pos == std::string::npos) return {};
            auto quote_start = body.find('"', pos + 1);
            if (quote_start == std::string::npos) return {};
            auto quote_end = body.find('"', quote_start + 1);
            if (quote_end == std::string::npos) return {};
            return body.substr(quote_start + 1, quote_end - quote_start - 1);
        };

        auto extract_int = [&](const std::string& key) -> int {
            auto pattern = "\"" + key + "\"";
            auto pos = body.find(pattern);
            if (pos == std::string::npos) return 0;
            pos = body.find(':', pos + pattern.size());
            if (pos == std::string::npos) return 0;
            // Skip whitespace
            pos++;
            while (pos < body.size() && body[pos] == ' ') pos++;
            return std::atoi(body.c_str() + pos);
        };

        token.access_token = extract_string("access_token");
        if (token.access_token.empty()) {
            return std::unexpected(OAuthError::TokenExchangeFailed);
        }
        token.refresh_token = extract_string("refresh_token");
        token.token_type = extract_string("token_type");
        if (token.token_type.empty()) token.token_type = "Bearer";
        token.scope = extract_string("scope");
        auto expires = extract_int("expires_in");
        token.expires_in = (expires > 0) ? expires : 3600;

        return token;
    }

    OAuthConfig config_;
    KeychainStore keychain_;
    std::optional<AuthorizationRequest> pending_request_;
};

} // namespace cc::services::oauth
