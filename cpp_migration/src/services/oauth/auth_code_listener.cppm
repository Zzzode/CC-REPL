module;
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

export module cc.services.oauth.auth_code_listener;

export namespace cc::services::oauth {

struct AuthCodeCallback {
    std::string code;
    std::string state;
};

// Local HTTP server listener for OAuth authorization code callback.
// Lightweight alternative that binds a socket and extracts the auth code.
class AuthCodeListener {
public:
    explicit AuthCodeListener(uint16_t port = 0) : port_(port) {}

    ~AuthCodeListener() { stop(); }

    // Non-copyable
    AuthCodeListener(const AuthCodeListener&) = delete;
    AuthCodeListener& operator=(const AuthCodeListener&) = delete;

    // Start listening
    std::expected<void, std::string> start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return std::unexpected("Failed to create socket");

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return std::unexpected("Failed to bind port");
        }

        // If port was 0, retrieve the assigned port
        if (port_ == 0) {
            struct sockaddr_in bound_addr{};
            socklen_t len = sizeof(bound_addr);
            getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&bound_addr), &len);
            port_ = ntohs(bound_addr.sin_port);
        }

        if (listen(server_fd_, 1) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return std::unexpected("Failed to listen");
        }

        running_ = true;
        return {};
    }

    // Get the redirect URI (call after start() to get actual port)
    [[nodiscard]] auto get_redirect_uri() -> std::string {
        return "http://localhost:" + std::to_string(port_) + "/oauth/callback";
    }

    // Wait for the authorization callback with timeout.
    auto wait_for_callback(std::chrono::seconds timeout = std::chrono::seconds{120})
        -> std::expected<AuthCodeCallback, std::string> {
        if (!running_ || server_fd_ < 0) {
            return std::unexpected("Server not running");
        }

        // Set socket timeout
        struct timeval tv{};
        tv.tv_sec = timeout.count();
        setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Accept incoming connection from browser redirect
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            stop();
            return std::unexpected("Timeout waiting for authorization code");
        }

        // Read HTTP request
        char buffer[4096];
        auto bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            close(client_fd);
            stop();
            return std::unexpected("Failed to read request");
        }
        buffer[bytes_read] = '\0';

        // Send success response to browser
        constexpr auto response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<html><body><h1>Authorization successful!</h1>"
            "<p>You can close this window and return to the terminal.</p></body></html>";
        write(client_fd, response, std::strlen(response));
        close(client_fd);
        stop();

        // Extract authorization callback parameters.
        std::string_view request(buffer, static_cast<size_t>(bytes_read));
        if (request.find("error=") != std::string_view::npos) {
            return std::unexpected("Authorization denied by user");
        }

        auto code = extract_param(request, "code");
        if (!code || code->empty()) return std::unexpected("No authorization code in callback");
        auto state = extract_param(request, "state");
        if (!state || state->empty()) return std::unexpected("No OAuth state in callback");
        return AuthCodeCallback{.code = *code, .state = *state};
    }

    // Wait for the authorization code with timeout.
    auto wait_for_code(std::chrono::seconds timeout = std::chrono::seconds{120})
        -> std::expected<std::string, std::string> {
        auto callback = wait_for_callback(timeout);
        if (!callback) return std::unexpected(callback.error());
        return callback->code;
    }

    // Stop the listener server
    auto stop() -> void {
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        running_ = false;
    }

private:
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
            if (value[i] == '+') {
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

    uint16_t port_;
    int server_fd_ = -1;
    bool running_{false};
};

} // namespace cc::services::oauth
