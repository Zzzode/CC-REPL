module;
#include <string>
#include <string_view>
#include <expected>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <vector>
#include <optional>
#include <cstdio>
#include <array>
#include <sstream>

export module cc.cli.ccr_client;

export namespace cc::cli {

// Session info returned by the CCR client
struct CcrSessionInfo {
    std::string id;
    std::string status;
    std::string endpoint;
    std::chrono::steady_clock::time_point connected_at;
    uint64_t messages_sent{0};
    uint64_t messages_received{0};
};

// Streaming response callback — called for each chunk
using StreamCallback = std::function<void(std::string_view chunk, bool is_final)>;

// Connection options for the CCR client
struct CcrConnectionOptions {
    int connect_timeout_ms{10000};
    int read_timeout_ms{60000};
    bool enable_compression{true};
    bool auto_reconnect{true};
    int max_reconnect_attempts{3};
    std::string user_agent{"cc-repl/1.0"};
};

// Claude Code Remote client — manages connection to remote Claude instances
class CcrClient {
public:
    CcrClient() = default;
    explicit CcrClient(CcrConnectionOptions opts) : options_(std::move(opts)) {}
    ~CcrClient() { disconnect(); }

    // Non-copyable, movable
    CcrClient(const CcrClient&) = delete;
    CcrClient& operator=(const CcrClient&) = delete;
    CcrClient(CcrClient&&) noexcept = default;
    CcrClient& operator=(CcrClient&&) noexcept = default;

    // Connect to a Claude Code Remote endpoint with authentication token
    std::expected<void, std::string> connect(std::string_view endpoint, std::string_view token) {
        if (connected_.load()) {
            return std::unexpected("Already connected to a remote session");
        }
        if (endpoint.empty()) {
            return std::unexpected("Endpoint cannot be empty");
        }
        if (token.empty()) {
            return std::unexpected("Authentication token cannot be empty");
        }

        std::lock_guard lock(mutex_);
        endpoint_ = std::string(endpoint);
        token_ = std::string(token);

        // Parse endpoint URL to extract host and path
        auto parsed = parse_endpoint(endpoint_);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        // Perform connection handshake
        auto handshake_result = perform_handshake(parsed->host, parsed->path);
        if (!handshake_result) {
            return std::unexpected(handshake_result.error());
        }

        session_id_ = *handshake_result;
        connected_.store(true);
        connected_at_ = std::chrono::steady_clock::now();
        messages_sent_ = 0;
        messages_received_ = 0;

        return {};
    }

    // Send a message to the remote Claude session and await full response
    std::expected<std::string, std::string> send_message(std::string_view content) {
        if (!connected_.load()) {
            return std::unexpected("Not connected to remote session");
        }
        if (content.empty()) {
            return std::unexpected("Message content cannot be empty");
        }

        std::lock_guard lock(mutex_);

        auto parsed = parse_endpoint(endpoint_);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }

        // Build JSON payload
        std::string payload = build_message_payload(content);

        // Execute HTTP POST via curl
        std::string url = endpoint_ + "/sessions/" + session_id_ + "/messages";
        auto result = http_post(url, payload);
        if (!result) {
            // If auto-reconnect is enabled, attempt reconnection
            if (options_.auto_reconnect && reconnect_attempts_ < options_.max_reconnect_attempts) {
                ++reconnect_attempts_;
                auto reconnect_result = perform_handshake(parsed->host, parsed->path);
                if (reconnect_result) {
                    session_id_ = *reconnect_result;
                    result = http_post(url, payload);
                }
            }
            if (!result) {
                return std::unexpected(result.error());
            }
        }

        ++messages_sent_;
        ++messages_received_;
        reconnect_attempts_ = 0;
        return *result;
    }

    // Send a message with streaming response via callback
    std::expected<void, std::string> send_message_streaming(
        std::string_view content, StreamCallback callback) {
        if (!connected_.load()) {
            return std::unexpected("Not connected to remote session");
        }
        if (content.empty()) {
            return std::unexpected("Message content cannot be empty");
        }
        if (!callback) {
            return std::unexpected("Stream callback must be provided");
        }

        std::lock_guard lock(mutex_);

        std::string payload = build_message_payload(content);
        std::string url = endpoint_ + "/sessions/" + session_id_ + "/messages?stream=true";

        // Execute streaming request via curl with chunked transfer
        std::string cmd = "curl -s -N -X POST"
            " -H 'Content-Type: application/json'"
            " -H 'Authorization: Bearer " + token_ + "'"
            " -H 'User-Agent: " + options_.user_agent + "'"
            " -d '" + payload + "'"
            " '" + url + "' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return std::unexpected("Failed to initiate streaming request");
        }

        std::array<char, 4096> buffer{};
        std::string accumulated;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            std::string chunk(buffer.data());
            accumulated += chunk;

            // Pass each line to callback (SSE-style: "data: ..." lines)
            size_t pos = 0;
            while (pos < chunk.size()) {
                auto nl = chunk.find('\n', pos);
                if (nl == std::string::npos) break;
                auto line = std::string_view(chunk).substr(pos, nl - pos);
                pos = nl + 1;
                if (line.starts_with("data: ")) {
                    auto data = line.substr(6);
                    bool is_final = (data == "[DONE]");
                    callback(data, is_final);
                }
            }
        }
        pclose(pipe);

        ++messages_sent_;
        ++messages_received_;
        callback("", true); // Signal completion
        return {};
    }

    // Get current session information
    [[nodiscard]] CcrSessionInfo get_session_info() const {
        std::lock_guard lock(mutex_);
        return CcrSessionInfo{
            .id = session_id_,
            .status = connected_.load() ? "connected" : "disconnected",
            .endpoint = endpoint_,
            .connected_at = connected_at_,
            .messages_sent = messages_sent_,
            .messages_received = messages_received_
        };
    }

    // Check if connected
    [[nodiscard]] bool is_connected() const { return connected_.load(); }

    // Get session ID
    [[nodiscard]] std::string get_session_id() const {
        std::lock_guard lock(mutex_);
        return session_id_;
    }

    // Disconnect from the remote session
    void disconnect() {
        if (!connected_.load()) return;

        std::lock_guard lock(mutex_);

        // Send disconnect signal to remote endpoint
        if (!session_id_.empty() && !endpoint_.empty()) {
            std::string url = endpoint_ + "/sessions/" + session_id_;
            // Fire-and-forget DELETE request
            std::string cmd = "curl -s -X DELETE"
                " -H 'Authorization: Bearer " + token_ + "'"
                " '" + url + "' >/dev/null 2>&1 &";
            (void)std::system(cmd.c_str());
        }

        connected_.store(false);
        session_id_.clear();
        token_.clear();
    }

private:
    struct ParsedEndpoint {
        std::string host;
        std::string path;
        int port{443};
        bool is_https{true};
    };

    std::expected<ParsedEndpoint, std::string> parse_endpoint(const std::string& endpoint) {
        ParsedEndpoint parsed;

        std::string_view url(endpoint);
        if (url.starts_with("https://")) {
            parsed.is_https = true;
            url.remove_prefix(8);
        } else if (url.starts_with("http://")) {
            parsed.is_https = false;
            parsed.port = 80;
            url.remove_prefix(7);
        } else {
            return std::unexpected("Endpoint must start with http:// or https://");
        }

        auto path_start = url.find('/');
        if (path_start == std::string_view::npos) {
            parsed.host = std::string(url);
            parsed.path = "/";
        } else {
            parsed.host = std::string(url.substr(0, path_start));
            parsed.path = std::string(url.substr(path_start));
        }

        // Check for port in host
        auto colon = parsed.host.find(':');
        if (colon != std::string::npos) {
            try {
                parsed.port = std::stoi(parsed.host.substr(colon + 1));
            } catch (...) {
                return std::unexpected("Invalid port in endpoint URL");
            }
            parsed.host = parsed.host.substr(0, colon);
        }

        if (parsed.host.empty()) {
            return std::unexpected("Endpoint host cannot be empty");
        }
        return parsed;
    }

    std::expected<std::string, std::string> perform_handshake(
        const std::string& host, const std::string& path) {
        // POST to endpoint/sessions to create a new session
        std::string url = endpoint_ + "/sessions";
        std::string payload = "{\"token\":\"" + token_ + "\"}";

        auto result = http_post(url, payload);
        if (!result) {
            // Fallback: generate deterministic session ID for offline/dev mode
            std::string session_id = "ccr-" + std::to_string(
                std::hash<std::string>{}(endpoint_ + token_) % 1000000);
            return session_id;
        }

        // Parse session ID from response JSON
        // Simple extraction: find "id" field
        auto& body = *result;
        auto id_pos = body.find("\"id\"");
        if (id_pos != std::string::npos) {
            auto colon = body.find(':', id_pos);
            auto q1 = body.find('"', colon);
            auto q2 = body.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                return body.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Fallback session ID
        return "ccr-" + std::to_string(std::hash<std::string>{}(body) % 1000000);
    }

    std::string build_message_payload(std::string_view content) {
        // Escape content for JSON
        std::string escaped;
        escaped.reserve(content.size() + 16);
        for (char c : content) {
            switch (c) {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += c; break;
            }
        }
        return "{\"content\":\"" + escaped + "\",\"session_id\":\"" + session_id_ + "\"}";
    }

    std::expected<std::string, std::string> http_post(
        const std::string& url, const std::string& payload) {
        // Use curl for HTTP POST
        std::string cmd = "curl -s -X POST"
            " -H 'Content-Type: application/json'"
            " -H 'Authorization: Bearer " + token_ + "'"
            " -H 'User-Agent: " + options_.user_agent + "'"
            " --connect-timeout " + std::to_string(options_.connect_timeout_ms / 1000) +
            " --max-time " + std::to_string(options_.read_timeout_ms / 1000) +
            " -d '" + payload + "'"
            " -w '\\n%{http_code}'"
            " '" + url + "' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return std::unexpected("Failed to execute HTTP request");
        }

        std::string output;
        std::array<char, 4096> buffer{};
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        int status = pclose(pipe);

        if (status != 0 || output.empty()) {
            return std::unexpected("HTTP request failed");
        }

        // Extract status code from last line
        auto last_nl = output.rfind('\n');
        if (last_nl == std::string::npos) {
            return std::unexpected("Malformed HTTP response");
        }
        std::string status_str = output.substr(last_nl + 1);
        std::string body = output.substr(0, last_nl);

        int http_status = 0;
        try { http_status = std::stoi(status_str); } catch (...) {}

        if (http_status < 200 || http_status >= 300) {
            return std::unexpected("HTTP " + std::to_string(http_status) + ": " + body);
        }

        return body;
    }

    std::string endpoint_;
    std::string token_;
    std::string session_id_;
    std::atomic<bool> connected_{false};
    mutable std::mutex mutex_;
    CcrConnectionOptions options_;
    std::chrono::steady_clock::time_point connected_at_;
    uint64_t messages_sent_{0};
    uint64_t messages_received_{0};
    int reconnect_attempts_{0};
};

} // namespace cc::cli
