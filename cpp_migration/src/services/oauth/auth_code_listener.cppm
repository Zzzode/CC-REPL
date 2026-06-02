module;
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

export module cc.services.oauth.auth_code_listener;

export namespace cc::services::oauth {

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

    // Wait for the authorization code with timeout
    auto wait_for_code(std::chrono::seconds timeout = std::chrono::seconds{120})
        -> std::expected<std::string, std::string> {
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

        // Extract authorization code from GET parameters
        std::string_view request(buffer, static_cast<size_t>(bytes_read));
        auto code_pos = request.find("code=");
        if (code_pos == std::string_view::npos) {
            if (request.find("error=") != std::string_view::npos) {
                return std::unexpected("Authorization denied by user");
            }
            return std::unexpected("No authorization code in callback");
        }

        code_pos += 5; // Skip "code="
        auto code_end = request.find_first_of("& ", code_pos);
        if (code_end == std::string_view::npos) code_end = request.size();
        return std::string(request.substr(code_pos, code_end - code_pos));
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
    uint16_t port_;
    int server_fd_ = -1;
    bool running_{false};
};

} // namespace cc::services::oauth
