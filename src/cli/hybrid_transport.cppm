module;
#include <string>
#include <string_view>
#include <functional>
#include <expected>
#include <memory>
#include <map>
#include <mutex>
#include <format>
#include <unordered_map>

export module cc.cli.hybrid_transport;

import cc.cli.sse_transport;
import cc.cli.websocket_transport;
import cc.utils.http;

export namespace cc::cli {

// HybridTransport auto-selects between SSE and WebSocket based on URL scheme
class HybridTransport {
public:
    HybridTransport() = default;
    ~HybridTransport() = default;

    // Connect to the endpoint, auto-detecting transport type from URL
    std::expected<void, std::string> connect(std::string_view url) {
        url_ = std::string(url);

        // Determine transport type based on URL scheme
        if (url.starts_with("ws://") || url.starts_with("wss://")) {
            transport_type_ = "websocket";
            ws_transport_ = std::make_unique<WebSocketTransport>();
            return ws_transport_->connect(url);
        } else if (url.starts_with("http://") || url.starts_with("https://")) {
            transport_type_ = "sse";
            sse_transport_ = std::make_unique<SSETransport>();
            // SSE connects with empty headers by default
            return sse_transport_->connect(url, {});
        }

        return std::unexpected("Unsupported URL scheme: must be http(s) or ws(s)");
    }

    // Send a message (only applicable for WebSocket; SSE is receive-only)
    std::expected<void, std::string> send(std::string_view message) {
        if (transport_type_ == "websocket" && ws_transport_) {
            return ws_transport_->send(message);
        }

        if (transport_type_ == "sse") {
            const auto endpoint = sse_send_url_.empty() ? url_ : sse_send_url_;
            cc::utils::HttpClient client;
            auto response = client.post(endpoint, message, std::unordered_map<std::string, std::string>{
                {"Content-Type", "application/json"},
            });
            if (!response) {
                return std::unexpected(response.error().message);
            }
            if (!response->is_ok()) {
                return std::unexpected(std::format(
                    "SSE send POST failed with status {}: {}",
                    response->status, response->body));
            }
            return {};
        }

        return std::unexpected("No transport connected");
    }

    // Register callback for incoming messages from either transport
    void on_message(std::function<void(std::string_view)> callback) {
        std::lock_guard lock(mutex_);
        message_callback_ = std::move(callback);

        if (transport_type_ == "websocket" && ws_transport_) {
            ws_transport_->on_message(message_callback_);
        } else if (transport_type_ == "sse" && sse_transport_) {
            // Wrap SSE events into a unified message callback
            sse_transport_->on_event([cb = message_callback_](const auto& event) {
                if (cb) cb(event.data);
            });
        }
    }

    // Get the active transport type
    std::string get_transport_type() const {
        return transport_type_;
    }

    void set_sse_send_endpoint(std::string endpoint) {
        std::lock_guard lock(mutex_);
        sse_send_url_ = std::move(endpoint);
    }

    // Reconnect using the same URL and transport type
    std::expected<void, std::string> reconnect() {
        if (url_.empty()) {
            return std::unexpected("No previous connection to reconnect");
        }

        // Close existing connection
        if (transport_type_ == "websocket" && ws_transport_) {
            ws_transport_->close();
            ws_transport_.reset();
        } else if (transport_type_ == "sse" && sse_transport_) {
            sse_transport_->close();
            sse_transport_.reset();
        }

        // Re-establish connection
        auto result = connect(url_);
        if (result.has_value() && message_callback_) {
            // Re-register message callback after reconnection
            on_message(message_callback_);
        }

        return result;
    }

private:
    std::string url_;
    std::string sse_send_url_;
    std::string transport_type_;
    std::unique_ptr<SSETransport> sse_transport_;
    std::unique_ptr<WebSocketTransport> ws_transport_;
    std::function<void(std::string_view)> message_callback_;
    std::mutex mutex_;
};

} // namespace cc::cli
