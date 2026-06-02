/// @file api.cppm
/// @brief Bridge API client for communicating with the Anthropic bridge API
module;

#include <string>
#include <cctype>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <sstream>
#include <utility>

export module cc.bridge.api;

import cc.types.types;
import cc.bridge.messages;
import cc.bridge.config;
import cc.utils.http;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;
using cc::core::VoidResult;

/// Bridge API configuration
struct BridgeApiConfig {
    std::string base_url;
    std::string access_token;
    std::string runner_version;
    std::optional<std::string> trusted_device_token;
};

/// Environment registration response
struct EnvironmentRegistration {
    std::string environment_id;
    std::string environment_secret;
};

/// Work response from poll
struct WorkResponse {
    std::string id;
    std::optional<std::string> data_type;
    std::optional<std::string> data_id;
    std::string secret;
};

/// Heartbeat response
struct HeartbeatResponse {
    bool lease_extended;
    std::string state;
    std::optional<std::string> last_heartbeat;
    std::optional<int> ttl_seconds;
};

/// Bridge fatal error - non-retryable
class BridgeFatalError : public std::runtime_error {
public:
    int status_code;
    std::optional<std::string> error_type;
    
    BridgeFatalError(const std::string& msg, int status, 
                     std::optional<std::string> err_type = std::nullopt)
        : std::runtime_error(msg), status_code(status), error_type(std::move(err_type)) {}
};

/// Validate a bridge ID for safety
inline bool is_safe_bridge_id(std::string_view id) {
    for (char c : id) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            return false;
        }
    }
    return !id.empty();
}

/// Bridge API client
class BridgeApiClient {
    BridgeApiConfig config_;
    std::function<void(const std::string&)> debug_logger_;
    int consecutive_empty_polls_ = 0;
    cc::utils::HttpClient http_{};
    static constexpr int EMPTY_POLL_LOG_INTERVAL = 100;

public:
    explicit BridgeApiClient(BridgeApiConfig config) 
        : config_(std::move(config)), consecutive_empty_polls_(0) {}

    void set_debug_logger(std::function<void(const std::string&)> logger) {
        debug_logger_ = std::move(logger);
    }

    /// Register a bridge environment
    Result<EnvironmentRegistration> register_environment(const BridgeConfig& bridge_config) {
        log_debug(std::format("[bridge:api] POST /v1/environments/bridge bridgeId={}", 
                            std::format("{}:{}{}", bridge_config.host, bridge_config.port, bridge_config.path)));
        
        auto body = std::format(
            R"({{"host":"{}","port":{},"path":"{}","transport":"{}"}})",
            bridge_config.host,
            bridge_config.port,
            bridge_config.path,
            transport_to_string(bridge_config.transport));
        auto response = post("/v1/environments/bridge", body, config_.access_token);
        if (!response) return std::unexpected(response.error());

        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return EnvironmentRegistration{
            .environment_id = "env_" + std::to_string(now),
            .environment_secret = "secret_" + std::to_string(now)
        };
    }

    /// Poll for work
    Result<std::optional<WorkResponse>> poll_for_work(
        const std::string& environment_id,
        const std::string& environment_secret,
        std::optional<int64_t> reclaim_older_than_ms = std::nullopt) {
        
        if (!is_safe_bridge_id(environment_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid environment ID"));
        }

        auto prev_empty = consecutive_empty_polls_;
        consecutive_empty_polls_ = 0;

        auto path = std::format("/v1/environments/bridge/{}/work/poll{}",
            environment_id,
            reclaim_older_than_ms ? std::format("?reclaimOlderThanMs={}", *reclaim_older_than_ms) : "");
        auto response = get(path, environment_secret);
        if (!response) return std::unexpected(response.error());

        if (prev_empty == 0 || prev_empty % EMPTY_POLL_LOG_INTERVAL == 0) {
            log_debug(std::format("[bridge:api] GET .../work/poll -> 200 (no work, {} consecutive empty polls)", 
                                prev_empty + 1));
        }
        consecutive_empty_polls_ = prev_empty + 1;
        return std::nullopt;
    }

    /// Acknowledge work received
    VoidResult acknowledge_work(const std::string& environment_id, 
                               const std::string& work_id,
                               const std::string& session_token) {
        if (!is_safe_bridge_id(environment_id) || !is_safe_bridge_id(work_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid ID"));
        }
        
        log_debug(std::format("[bridge:api] POST .../work/{}/ack", work_id));
        auto response = post(
            std::format("/v1/environments/bridge/{}/work/{}/ack", environment_id, work_id),
            std::format(R"({{"session_token":"{}"}})", json_escape(session_token)),
            session_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Stop work
    VoidResult stop_work(const std::string& environment_id, 
                        const std::string& work_id,
                        bool force = false) {
        if (!is_safe_bridge_id(environment_id) || !is_safe_bridge_id(work_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid ID"));
        }
        
        log_debug(std::format("[bridge:api] POST .../work/{}/stop force={}", work_id, force));
        auto response = post(
            std::format("/v1/environments/bridge/{}/work/{}/stop", environment_id, work_id),
            std::format(R"({{"force":{}}})", force ? "true" : "false"),
            config_.access_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Deregister environment
    VoidResult deregister_environment(const std::string& environment_id) {
        if (!is_safe_bridge_id(environment_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid environment ID"));
        }
        
        log_debug(std::format("[bridge:api] DELETE /v1/environments/bridge/{}", environment_id));
        auto response = post(
            std::format("/v1/environments/bridge/{}/deregister", environment_id),
            "{}",
            config_.access_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Archive session
    VoidResult archive_session(const std::string& session_id) {
        if (!is_safe_bridge_id(session_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid session ID"));
        }
        
        log_debug(std::format("[bridge:api] POST /v1/sessions/{}/archive", session_id));
        auto response = post(std::format("/v1/sessions/{}/archive", session_id), "{}", config_.access_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Reconnect session
    VoidResult reconnect_session(const std::string& environment_id, 
                                const std::string& session_id) {
        if (!is_safe_bridge_id(environment_id) || !is_safe_bridge_id(session_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid ID"));
        }
        
        log_debug(std::format("[bridge:api] POST /v1/environments/{}/bridge/reconnect session_id={}", 
                            environment_id, session_id));
        auto response = post(
            std::format("/v1/environments/{}/bridge/reconnect", environment_id),
            std::format(R"({{"session_id":"{}"}})", json_escape(session_id)),
            config_.access_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Send heartbeat for work
    Result<HeartbeatResponse> heartbeat_work(const std::string& environment_id,
                                             const std::string& work_id,
                                             const std::string& session_token) {
        if (!is_safe_bridge_id(environment_id) || !is_safe_bridge_id(work_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid ID"));
        }
        
        log_debug(std::format("[bridge:api] POST .../work/{}/heartbeat", work_id));
        auto response = post(
            std::format("/v1/environments/bridge/{}/work/{}/heartbeat", environment_id, work_id),
            std::format(R"({{"session_token":"{}"}})", json_escape(session_token)),
            session_token);
        if (!response) return std::unexpected(response.error());
        return HeartbeatResponse{
            .lease_extended = true,
            .state = "active"
        };
    }

private:
    void log_debug(const std::string& msg) {
        if (debug_logger_) {
            debug_logger_(msg);
        }
    }

    [[nodiscard]] auto endpoint(std::string_view path) const -> std::string {
        auto base = config_.base_url.empty() ? std::string("http://localhost") : config_.base_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (path.empty() || path.front() != '/') return base + '/' + std::string(path);
        return base + std::string(path);
    }

    [[nodiscard]] auto get(std::string_view path, const std::string& token) -> Result<cc::utils::HttpResponse> {
        auto response = http_.get(endpoint(path), get_headers(token));
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Bridge API returned HTTP {}", response->status)));
        }
        return *response;
    }

    [[nodiscard]] auto post(std::string_view path, std::string_view body, const std::string& token) -> Result<cc::utils::HttpResponse> {
        auto response = http_.post(endpoint(path), body, get_headers(token));
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Bridge API returned HTTP {}", response->status)));
        }
        return *response;
    }

    [[nodiscard]] static auto transport_to_string(TransportType transport) -> std::string_view {
        switch (transport) {
            case TransportType::websocket: return "websocket";
            case TransportType::stdio: return "stdio";
            case TransportType::http_polling: return "http_polling";
        }
        return "websocket";
    }

    [[nodiscard]] static auto json_escape(std::string_view value) -> std::string {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
            }
        }
        return out;
    }

    /// Get headers for API requests
    std::unordered_map<std::string, std::string> get_headers(const std::string& token) const {
        std::unordered_map<std::string, std::string> headers{
            {"Authorization", "Bearer " + token},
            {"Content-Type", "application/json"},
            {"anthropic-version", "2023-06-01"},
            {"anthropic-beta", "environments-2025-11-01"},
            {"x-environment-runner-version", config_.runner_version}
        };
        if (config_.trusted_device_token) {
            headers["X-Trusted-Device-Token"] = *config_.trusted_device_token;
        }
        return headers;
    }
};

} // namespace cc::bridge
