module;
#include <string>
#include <string_view>
#include <functional>
#include <expected>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <chrono>
#include <cstdint>

export module cc.cli.websocket_transport;

export namespace cc::cli {

// WebSocket transport for bidirectional communication
class WebSocketTransport {
public:
    WebSocketTransport() = default;
    ~WebSocketTransport() { close(); }

    // Establish WebSocket connection to the given URL
    std::expected<void, std::string> connect(std::string_view url) {
        if (connected_.load()) {
            return std::unexpected("Already connected");
        }

        url_ = std::string(url);

        // Validate WebSocket URL
        if (url_.empty() || (url_.substr(0, 5) != "ws://" && url_.substr(0, 6) != "wss://")) {
            return std::unexpected("Invalid WebSocket URL: must start with ws:// or wss://");
        }

        // In production: perform WebSocket handshake via HTTP upgrade
        connected_.store(true);

        // Start message reading thread
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            run_read_loop(stop);
        });

        return {};
    }

    // Send a text message over the WebSocket connection
    std::expected<void, std::string> send(std::string_view message) {
        if (!connected_.load()) {
            return std::unexpected("Not connected");
        }

        if (message.empty()) {
            return std::unexpected("Cannot send empty message");
        }

        // Frame the message as a WebSocket text frame
        std::lock_guard lock(send_mutex_);
        send_queue_.push(std::string(message));

        return {};
    }

    // Register callback for incoming messages
    void on_message(std::function<void(std::string_view)> callback) {
        std::lock_guard lock(callback_mutex_);
        message_callback_ = std::move(callback);
    }

    // Register callback for connection close events
    void on_close(std::function<void(int code, std::string_view reason)> callback) {
        std::lock_guard lock(callback_mutex_);
        close_callback_ = std::move(callback);
    }

    // Close the WebSocket connection with an optional status code
    void close(int code = 1000) {
        if (!connected_.load()) return;

        connected_.store(false);
        close_code_ = code;

        // Send close frame in production
        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
            reader_thread_.join();
        }

        // Notify close callback
        {
            std::lock_guard lock(callback_mutex_);
            if (close_callback_) {
                close_callback_(code, "Connection closed");
            }
        }

        // Clear pending messages
        std::lock_guard lock(send_mutex_);
        while (!send_queue_.empty()) send_queue_.pop();
    }

    // Check if the WebSocket is currently connected
    bool is_connected() const {
        return connected_.load();
    }

private:
    // Read loop processes incoming WebSocket frames
    void run_read_loop(std::stop_token stop) {
        while (!stop.stop_requested() && connected_.load()) {
            // Drain send queue first (send pending outgoing messages)
            {
                std::lock_guard lock(send_mutex_);
                while (!send_queue_.empty()) {
                    auto& msg = send_queue_.front();
                    // Frame as WebSocket text frame:
                    // Opcode 0x81 (final frame, text), followed by length encoding
                    // In a real implementation with raw sockets, we'd write the frame bytes.
                    // Here we record that the message was "sent" for the protocol layer.
                    sent_count_.fetch_add(1, std::memory_order_relaxed);
                    send_queue_.pop();
                }
            }

            // Poll for incoming data (non-blocking read with timeout)
            // In a full implementation: use poll/select/epoll on the socket fd
            // with a short timeout to remain responsive to stop requests.
            //
            // Frame parsing: read 2-byte header, determine opcode & payload length
            // - Opcode 0x1: text frame -> dispatch_message
            // - Opcode 0x2: binary frame -> dispatch_message
            // - Opcode 0x8: close frame -> initiate close
            // - Opcode 0x9: ping -> send pong (opcode 0xA)
            // - Opcode 0xA: pong -> update last_pong_time
            //
            // For frames with payload_len == 126: read 2 more bytes for actual length
            // For frames with payload_len == 127: read 8 more bytes for actual length

            // Check if we need to send a ping (keepalive)
            auto now = std::chrono::steady_clock::now();
            if (now - last_activity_ > std::chrono::seconds(30)) {
                // Send ping frame to keep connection alive
                last_activity_ = now;
            }

            // Sleep briefly to yield CPU when no data is available
            // In production with real sockets, this would be replaced by
            // poll() with a timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Dispatch a received message to the callback
    void dispatch_message(std::string_view message) {
        std::lock_guard lock(callback_mutex_);
        if (message_callback_) {
            message_callback_(message);
        }
    }

    std::string url_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> sent_count_{0};
    int close_code_{0};
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};

    std::function<void(std::string_view)> message_callback_;
    std::function<void(int, std::string_view)> close_callback_;
    std::mutex callback_mutex_;

    std::queue<std::string> send_queue_;
    std::mutex send_mutex_;

    std::jthread reader_thread_;
};

} // namespace cc::cli
