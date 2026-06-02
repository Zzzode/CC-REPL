module;
#include <string>
#include <string_view>
#include <functional>
#include <expected>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <chrono>
#include <cstddef>

export module cc.cli.remote_io;

export namespace cc::cli {

// RemoteIO bridges local I/O to a remote endpoint for headless operation
class RemoteIO {
public:
    RemoteIO() = default;
    ~RemoteIO() { close(); }

    // Connect to a remote I/O endpoint
    std::expected<void, std::string> connect(std::string_view endpoint) {
        if (endpoint.empty()) {
            return std::unexpected("Endpoint cannot be empty");
        }

        if (connected_.load()) {
            return std::unexpected("Already connected");
        }

        std::lock_guard lock(mutex_);
        endpoint_ = std::string(endpoint);

        // In production: establish WebSocket or HTTP/2 connection to remote endpoint
        // This enables IDE integrations to receive Claude's output remotely
        connected_.store(true);

        // Start the output forwarding thread
        output_thread_ = std::jthread([this](std::stop_token stop) {
            run_output_loop(stop);
        });

        return {};
    }

    // Send input to the remote session (simulates user typing)
    void send_input(std::string_view input) {
        if (!connected_.load()) return;

        std::lock_guard lock(input_mutex_);
        input_queue_.push(std::string(input));
    }

    // Register callback for output received from the remote session
    void on_output(std::function<void(std::string_view)> callback) {
        std::lock_guard lock(mutex_);
        output_callback_ = std::move(callback);
    }

    // Close the remote I/O connection
    void close() {
        if (!connected_.load()) return;

        connected_.store(false);

        if (output_thread_.joinable()) {
            output_thread_.request_stop();
            output_thread_.join();
        }

        std::lock_guard lock(mutex_);
        output_callback_ = nullptr;
        endpoint_.clear();

        // Drain input queue
        std::lock_guard input_lock(input_mutex_);
        while (!input_queue_.empty()) input_queue_.pop();
    }

private:
    // Output loop forwards any pending output to the registered callback
    // Also drains the input queue and forwards to remote endpoint
    void run_output_loop(std::stop_token stop) {
        while (!stop.stop_requested() && connected_.load()) {
            // Process any queued input to send to the remote endpoint
            {
                std::lock_guard lock(input_mutex_);
                while (!input_queue_.empty()) {
                    auto& msg = input_queue_.front();
                    // In a full implementation: write to the remote socket/HTTP2 stream
                    // For now, track that input was forwarded
                    bytes_sent_.fetch_add(msg.size(), std::memory_order_relaxed);
                    input_queue_.pop();
                }
            }

            // Poll for incoming data from the remote endpoint
            // In a full implementation with real network I/O:
            // - Use poll()/select() on the connection socket fd
            // - Read available data into a buffer
            // - Parse framing (newline-delimited JSON or length-prefixed)
            // - Dispatch complete messages via dispatch_output()
            //
            // The remote endpoint sends output from the Claude session
            // (assistant responses, tool results, status updates) which
            // we forward to the local IDE/editor integration.

            // Brief yield to avoid busy-waiting when no data is available
            // In production: replaced by blocking poll() with timeout
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Dispatch output to the registered callback
    void dispatch_output(std::string_view output) {
        std::lock_guard lock(mutex_);
        if (output_callback_) {
            output_callback_(output);
        }
    }

    std::string endpoint_;
    std::atomic<bool> connected_{false};
    std::atomic<std::size_t> bytes_sent_{0};
    std::function<void(std::string_view)> output_callback_;
    std::queue<std::string> input_queue_;
    std::mutex mutex_;
    std::mutex input_mutex_;
    std::jthread output_thread_;
};

} // namespace cc::cli
