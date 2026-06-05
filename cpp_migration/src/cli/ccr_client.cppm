module;
#include <string>
#include <string_view>
#include <expected>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cstdio>
#include <array>
#include <sstream>

export module cc.cli.ccr_client;

import cc.utils.http;

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
    bool allow_offline_session_fallback{false};
};

struct CcrHttpRequest {
    std::string method;
    std::string url;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    int connect_timeout_ms{10000};
    int read_timeout_ms{60000};
    StreamCallback stream_callback;
};

struct CcrHttpResponse {
    int status{0};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

using CcrHttpTransport = std::function<std::expected<CcrHttpResponse, std::string>(const CcrHttpRequest&)>;

inline std::expected<CcrHttpResponse, std::string> default_ccr_http_transport(const CcrHttpRequest& request) {
    cc::utils::HttpConfig config;
    config.timeout_ms = static_cast<uint32_t>(std::max(request.connect_timeout_ms, request.read_timeout_ms));
    config.max_retries = 0;
    if (auto it = request.headers.find("User-Agent"); it != request.headers.end()) {
        config.user_agent = it->second;
    }

    cc::utils::HttpClient client(std::move(config));
    std::expected<cc::utils::HttpResponse, cc::utils::HttpError> response;
    if (request.method == "POST") {
        if (request.stream_callback) {
            auto streamed = client.post_stream_sse(
                request.url,
                request.body,
                request.headers,
                [&](const cc::utils::SseEvent& event) {
                    const bool is_final = event.data == "[DONE]";
                    request.stream_callback(event.data, is_final);
                });
            if (!streamed) {
                return std::unexpected(streamed.error().message);
            }
            return CcrHttpResponse{.status = 200, .headers = {}, .body = {}};
        }
        response = client.post(request.url, request.body, request.headers);
    } else if (request.method == "DELETE") {
        response = client.delete_request(request.url, request.headers);
    } else {
        return std::unexpected("Unsupported CCR HTTP method: " + request.method);
    }

    if (!response) {
        return std::unexpected(response.error().message);
    }
    return CcrHttpResponse{
        .status = response->status,
        .headers = std::move(response->headers),
        .body = std::move(response->body),
    };
}

// Claude Code Remote client — manages connection to remote Claude instances
class CcrClient {
public:
    CcrClient() = default;
    explicit CcrClient(CcrConnectionOptions opts) : options_(std::move(opts)) {}
    ~CcrClient() { disconnect(); }

    // Non-copyable; the connection owns mutex and atomic state.
    CcrClient(const CcrClient&) = delete;
    CcrClient& operator=(const CcrClient&) = delete;
    CcrClient(CcrClient&&) = delete;
    CcrClient& operator=(CcrClient&&) = delete;

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

    void set_http_transport(CcrHttpTransport transport) {
        std::lock_guard lock(mutex_);
        http_transport_ = std::move(transport);
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
        auto payload = build_message_payload(content);
        auto message_url = [&]() {
            return endpoint_ + "/sessions/" + session_id_ + "/messages";
        };

        // Execute HTTP POST through the configured transport.
        std::string url = message_url();
        auto result = http_post(url, payload);
        if (!result) {
            // If auto-reconnect is enabled, attempt reconnection
            if (options_.auto_reconnect && reconnect_attempts_ < options_.max_reconnect_attempts) {
                ++reconnect_attempts_;
                auto reconnect_result = perform_handshake(parsed->host, parsed->path);
                if (reconnect_result) {
                    session_id_ = *reconnect_result;
                    payload = build_message_payload(content);
                    result = http_post(message_url(), payload);
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

        auto payload = build_message_payload(content);
        auto message_url = [&]() {
            return endpoint_ + "/sessions/" + session_id_ + "/messages?stream=true";
        };

        bool saw_final = false;
        auto streaming_callback = [&](std::string_view chunk, bool is_final) {
            if (is_final) saw_final = true;
            callback(chunk, is_final);
        };

        auto result = http_stream_post(message_url(), payload, streaming_callback);
        if (!result) {
            auto parsed = parse_endpoint(endpoint_);
            if (!parsed) return std::unexpected(parsed.error());
            if (options_.auto_reconnect && reconnect_attempts_ < options_.max_reconnect_attempts) {
                ++reconnect_attempts_;
                auto reconnect_result = perform_handshake(parsed->host, parsed->path);
                if (reconnect_result) {
                    session_id_ = *reconnect_result;
                    payload = build_message_payload(content);
                    result = http_stream_post(message_url(), payload, streaming_callback);
                }
            }
            if (!result) {
                return std::unexpected(result.error());
            }
        }

        ++messages_sent_;
        ++messages_received_;
        reconnect_attempts_ = 0;
        if (!saw_final) {
            callback("", true);
        }
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
            (void)http_delete(url);
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
        (void)host;
        (void)path;
        // POST to endpoint/sessions to create a new session
        std::string url = endpoint_ + "/sessions";
        std::string payload = "{\"token\":\"" + token_ + "\"}";

        auto result = http_post(url, payload);
        if (!result) {
            if (options_.allow_offline_session_fallback) {
                return "ccr-" + std::to_string(
                    std::hash<std::string>{}(endpoint_ + token_) % 1000000);
            }
            return std::unexpected("Remote session handshake failed: " + result.error());
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

        if (options_.allow_offline_session_fallback) {
            return "ccr-" + std::to_string(std::hash<std::string>{}(body) % 1000000);
        }
        return std::unexpected("Remote session handshake response did not include an id");
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
        auto response = http_request("POST", url, payload);
        if (!response) return std::unexpected(response.error());
        return response->body;
    }

    std::expected<void, std::string> http_delete(const std::string& url) {
        auto response = http_request("DELETE", url, "");
        if (!response) return std::unexpected(response.error());
        return {};
    }

    std::expected<void, std::string> http_stream_post(
        const std::string& url,
        const std::string& payload,
        StreamCallback callback) {
        auto response = http_request("POST", url, payload, std::move(callback));
        if (!response) return std::unexpected(response.error());
        return {};
    }

    std::expected<CcrHttpResponse, std::string> http_request(
        std::string method,
        const std::string& url,
        const std::string& payload,
        StreamCallback stream_callback = nullptr) {
        CcrHttpRequest request{
            .method = std::move(method),
            .url = url,
            .body = payload,
            .headers = {
                {"Content-Type", "application/json"},
                {"Authorization", "Bearer " + token_},
                {"User-Agent", options_.user_agent},
            },
            .connect_timeout_ms = options_.connect_timeout_ms,
            .read_timeout_ms = options_.read_timeout_ms,
            .stream_callback = std::move(stream_callback),
        };

        auto transport = http_transport_ ? http_transport_ : default_ccr_http_transport;
        auto response = transport(request);
        if (!response) return std::unexpected(response.error());
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected("HTTP " + std::to_string(response->status) + ": " + response->body);
        }
        return response;
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
    CcrHttpTransport http_transport_{default_ccr_http_transport};
};

} // namespace cc::cli
