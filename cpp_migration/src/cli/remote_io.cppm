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
#include <memory>

export module cc.cli.remote_io;

import cc.cli.websocket_transport;

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

        transport_ = std::make_unique<WebSocketTransport>();
        transport_->on_message([this](std::string_view output) {
            dispatch_output(output);
        });
        auto connect_result = transport_->connect(endpoint_);
        if (!connect_result) {
            transport_.reset();
            return std::unexpected(connect_result.error());
        }
        connected_.store(true);

        // Start the output forwarding thread
        stop_requested_.store(false, std::memory_order_release);
        output_thread_ = std::thread([this] {
            run_output_loop();
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

        stop_requested_.store(true, std::memory_order_release);

        if (output_thread_.joinable()) {
            output_thread_.join();
        }

        if (transport_) {
            transport_->close();
            transport_.reset();
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
    void run_output_loop() {
        while (!stop_requested_.load(std::memory_order_acquire) && connected_.load()) {
            // Process any queued input to send to the remote endpoint
            {
                std::lock_guard lock(input_mutex_);
                while (!input_queue_.empty()) {
                    auto& msg = input_queue_.front();
                    if (transport_) {
                        auto sent = transport_->send(msg);
                        if (sent) bytes_sent_.fetch_add(msg.size(), std::memory_order_relaxed);
                    }
                    input_queue_.pop();
                }
            }
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
    std::atomic<bool> stop_requested_{false};
    std::thread output_thread_;
    std::unique_ptr<WebSocketTransport> transport_;
};

} // namespace cc::cli
