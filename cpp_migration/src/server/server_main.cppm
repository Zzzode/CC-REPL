module;
#include <string>
#include <expected>
#include <optional>
#include <functional>
#include <map>
#include <cstdint>
#include <atomic>

export module cc.server.server_main;

export namespace cc::server {

// Configuration for the HTTP server
struct ServerConfig {
    uint16_t port = 3000;
    std::string host = "127.0.0.1";
    bool cors = false;
    std::optional<std::string> auth_token;
};

// A simple HTTP server for the CC-REPL API
class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer() { stop(); }

    // Prevent copy
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Start the HTTP server with the given configuration
    auto start(ServerConfig config) -> std::expected<void, std::string> {
        if (running_.load()) {
            return std::unexpected("Server is already running");
        }

        if (config.port == 0) {
            return std::unexpected("Port must be specified");
        }

        config_ = std::move(config);
        running_.store(true);

        // In a real implementation, this would bind and listen
        // For now, just mark as running
        return {};
    }

    // Stop the server
    auto stop() -> void {
        running_.store(false);
    }

    // Check if the server is running
    [[nodiscard]] auto is_running() const -> bool {
        return running_.load();
    }

    // Get the full URL of the running server
    [[nodiscard]] auto get_url() const -> std::string {
        if (!running_.load()) return "";
        return "http://" + config_.host + ":" + std::to_string(config_.port);
    }

    // Get the current configuration
    [[nodiscard]] auto get_config() const -> const ServerConfig& {
        return config_;
    }

private:
    ServerConfig config_;
    std::atomic<bool> running_{false};
};

} // namespace cc::server
