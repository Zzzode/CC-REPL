module;
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <array>

export module cc.cli.worker_state_uploader;

export namespace cc::cli {

// Periodically uploads worker state to a remote endpoint
class WorkerStateUploader {
public:
    WorkerStateUploader() = default;
    ~WorkerStateUploader() { stop(); }

    /// Set the endpoint URL for state uploads
    void set_endpoint(std::string_view endpoint) {
        std::lock_guard lock(mutex_);
        endpoint_ = std::string(endpoint);
    }

    // Upload state data as key-value pairs to the configured endpoint
    std::expected<void, std::string> upload_state(std::map<std::string, std::string> state) {
        if (state.empty()) {
            return std::unexpected("Cannot upload empty state");
        }

        std::lock_guard lock(mutex_);
        current_state_ = std::move(state);

        // Perform the actual upload (HTTP POST in production)
        auto result = perform_upload();
        if (result.has_value()) {
            last_status_ = "success";
        } else {
            last_status_ = "failed: " + result.error();
        }

        return result;
    }

    // Set the interval between automatic uploads
    void set_upload_interval(std::chrono::seconds interval) {
        upload_interval_ = interval;
    }

    // Start the periodic upload loop
    void start() {
        if (running_.load()) return;
        running_.store(true);

        upload_thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested() && running_.load()) {
                {
                    std::lock_guard lock(mutex_);
                    if (!current_state_.empty()) {
                        auto result = perform_upload();
                        last_status_ = result.has_value() ? "success" : "failed: " + result.error();
                    }
                }
                // Sleep for the configured interval, checking stop periodically
                auto end_time = std::chrono::steady_clock::now() + upload_interval_;
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < end_time) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    }

    // Stop the periodic upload loop
    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (upload_thread_.joinable()) {
            upload_thread_.request_stop();
            upload_thread_.join();
        }
    }

    // Get the status of the last upload attempt
    std::string get_last_upload_status() const {
        std::lock_guard lock(mutex_);
        return last_status_.empty() ? "no uploads yet" : last_status_;
    }

private:
    // Perform the actual HTTP upload of current state
    std::expected<void, std::string> perform_upload() {
        if (current_state_.empty()) {
            return std::unexpected("No state to upload");
        }

        if (endpoint_.empty()) {
            return std::unexpected("No endpoint configured");
        }

        // Build JSON payload
        std::string payload = "{";
        bool first = true;
        for (const auto& [key, value] : current_state_) {
            if (!first) payload += ",";
            // Simple JSON escaping for values
            std::string escaped_value;
            for (char c : value) {
                switch (c) {
                    case '"': escaped_value += "\\\""; break;
                    case '\\': escaped_value += "\\\\"; break;
                    case '\n': escaped_value += "\\n"; break;
                    default: escaped_value += c; break;
                }
            }
            payload += "\"" + key + "\":\"" + escaped_value + "\"";
            first = false;
        }
        payload += "}";

        // Execute HTTP POST via curl
        std::string cmd = "curl -sS -X POST '" + endpoint_ + "' "
            "-H 'Content-Type: application/json' "
            "-d '" + payload + "' "
            "-o /dev/null -w '%{http_code}' 2>&1";

        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) {
            return std::unexpected("Failed to execute upload request");
        }

        std::string output;
        std::array<char, 256> buffer{};
        while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }

        int status = ::pclose(pipe);
        if (status != 0) {
            return std::unexpected("Upload request failed: " + output);
        }

        // Check HTTP status code (curl -w gives us just the code)
        if (!output.empty()) {
            int http_code = 0;
            try { http_code = std::stoi(output); } catch (...) {}
            if (http_code >= 200 && http_code < 300) {
                return {};
            }
            return std::unexpected("Upload failed with HTTP " + output);
        }

        return {};
    }

    std::string endpoint_;
    std::map<std::string, std::string> current_state_;
    std::chrono::seconds upload_interval_{30};
    std::atomic<bool> running_{false};
    std::string last_status_;
    std::jthread upload_thread_;
    mutable std::mutex mutex_;
};

} // namespace cc::cli
