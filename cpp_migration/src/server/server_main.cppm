module;
#include <string>
#include <expected>
#include <optional>
#include <functional>
#include <map>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <httplib.h>

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
        std::lock_guard lock(mutex_);
        if (running_.load()) {
            return std::unexpected("Server is already running");
        }

        if (config.port == 0) {
            return std::unexpected("Port must be specified");
        }

        auto server = std::make_unique<httplib::Server>();
        server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        server->Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(get_url(), "text/plain");
        });

        if (!server->bind_to_port(config.host, static_cast<int>(config.port))) {
            return std::unexpected("Failed to bind HTTP server");
        }

        config_ = std::move(config);
        server_ = std::move(server);
        running_.store(true);
        server_thread_ = std::jthread([this](std::stop_token) {
            if (server_) {
                server_->listen_after_bind();
            }
        });
        return {};
    }

    // Stop the server
    auto stop() -> void {
        std::jthread worker;
        {
            std::lock_guard lock(mutex_);
            if (server_) {
                server_->stop();
            }
            worker = std::move(server_thread_);
            server_.reset();
        }
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
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
    std::unique_ptr<httplib::Server> server_;
    std::jthread server_thread_;
    std::mutex mutex_;
};

} // namespace cc::server
