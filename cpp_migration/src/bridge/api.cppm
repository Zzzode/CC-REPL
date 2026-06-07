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
#include <initializer_list>
#include <stdexcept>
#include <unordered_map>
#include <sstream>
#include <utility>
#include <charconv>

export module cc.bridge.api;

import cc.types.types;
import cc.bridge.messages;
import cc.bridge.config;
import cc.utils.http;
import cc.utils.json;

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

/// CCR v2 worker registration response.
struct WorkerRegistration {
    int64_t worker_epoch = 0;
};

/// A control_response event sent back to a remote session.
struct PermissionResponseEvent {
    std::string request_id;
    std::string response_json{"{}"};
    std::string subtype{"success"};
    std::optional<std::string> error;
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

        auto parsed = parse_environment_registration(response->body);
        if (!parsed) return std::unexpected(parsed.error());
        log_debug(std::format(
            "[bridge:api] POST /v1/environments/bridge -> {} environment_id={}",
            response->status,
            parsed->environment_id));
        return *parsed;
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

        auto path = std::format("/v1/environments/{}/work/poll{}",
            environment_id,
            reclaim_older_than_ms ? std::format("?reclaim_older_than_ms={}", *reclaim_older_than_ms) : "");
        auto response = get(path, environment_secret);
        if (!response) return std::unexpected(response.error());

        auto work = parse_work_response(response->body);
        if (!work) return std::unexpected(work.error());
        if (*work) {
            log_debug(std::format(
                "[bridge:api] GET .../work/poll -> {} workId={} type={}",
                response->status,
                (*work)->id,
                (*work)->data_type.value_or("")));
            return *work;
        }

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
            std::format("/v1/environments/{}/work/{}/ack", environment_id, work_id),
            "{}",
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
            std::format("/v1/environments/{}/work/{}/stop", environment_id, work_id),
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
        auto response = delete_request(
            std::format("/v1/environments/bridge/{}", environment_id),
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

    /// Send one raw session event through the bridge session events API.
    VoidResult send_session_event(const std::string& session_id,
                                  std::string_view event_json,
                                  const std::string& session_token) {
        if (!is_safe_bridge_id(session_id)) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Invalid session ID"));
        }
        auto event = cc::utils::json::parse(event_json);
        if (!event || !event->root().is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "Bridge session event must be a JSON object"));
        }

        log_debug(std::format("[bridge:api] POST /v1/sessions/{}/events", session_id));
        auto response = post(
            std::format("/v1/sessions/{}/events", session_id),
            std::format(R"({{"events":[{}]}})", event_json),
            session_token);
        if (!response) return std::unexpected(response.error());
        return {};
    }

    /// Send a permission/control response event to a bridge session.
    VoidResult send_permission_response_event(const std::string& session_id,
                                              const PermissionResponseEvent& event,
                                              const std::string& session_token) {
        if (event.request_id.empty()) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "request_id is required"));
        }
        if (event.subtype == "success") {
            auto response_payload = event.response_json.empty() ? std::string("{}") : event.response_json;
            auto response = cc::utils::json::parse(response_payload);
            if (!response) {
                return std::unexpected(Error::make(ErrorCode::InvalidInput, "permission response payload must be valid JSON"));
            }
            return send_session_event(
                session_id,
                std::format(
                    R"({{"type":"control_response","response":{{"subtype":"success","request_id":"{}","response":{}}}}})",
                    json_escape(event.request_id),
                    response_payload),
                session_token);
        }

        return send_session_event(
            session_id,
            std::format(
                R"({{"type":"control_response","response":{{"subtype":"{}","request_id":"{}","error":"{}"}}}})",
                json_escape(event.subtype),
                json_escape(event.request_id),
                json_escape(event.error.value_or("Bridge permission response failed"))),
            session_token);
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
            std::format("/v1/environments/{}/work/{}/heartbeat", environment_id, work_id),
            "{}",
            session_token);
        if (!response) return std::unexpected(response.error());
        auto parsed = parse_heartbeat_response(response->body);
        if (!parsed) return std::unexpected(parsed.error());
        return *parsed;
    }

    /// Register this daemon/child as the CCR v2 worker for a code session.
    Result<WorkerRegistration> register_worker(std::string_view session_url,
                                               const std::string& session_token) {
        if (session_url.empty()) {
            return std::unexpected(Error::make(ErrorCode::InvalidInput, "session_url is required"));
        }

        auto url = strip_trailing_slashes(std::string(session_url)) + "/worker/register";
        log_debug(std::format("[bridge:api] POST {}/worker/register", redact_session_url(session_url)));
        auto response = http_.post(url, "{}", get_headers(session_token));
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(error_for_http_status(response->status));
        }
        auto parsed = parse_worker_registration(response->body);
        if (!parsed) return std::unexpected(parsed.error());
        return *parsed;
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
            return std::unexpected(error_for_http_status(response->status));
        }
        return *response;
    }

    [[nodiscard]] auto delete_request(std::string_view path, const std::string& token) -> Result<cc::utils::HttpResponse> {
        auto response = http_.delete_request(endpoint(path), get_headers(token));
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(error_for_http_status(response->status));
        }
        return *response;
    }

    [[nodiscard]] static std::optional<std::string> string_field(
        cc::utils::json::JsonVal root,
        std::initializer_list<std::string_view> keys
    ) {
        for (auto key : keys) {
            auto value = root.get(key);
            if (value.is_str()) return std::string(value.as_str());
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<int> int_field(
        cc::utils::json::JsonVal root,
        std::initializer_list<std::string_view> keys
    ) {
        for (auto key : keys) {
            auto value = root.get(key);
            if (value.is_num()) return static_cast<int>(value.as_int());
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<int64_t> int64_field(
        cc::utils::json::JsonVal root,
        std::initializer_list<std::string_view> keys
    ) {
        for (auto key : keys) {
            auto value = root.get(key);
            if (value.is_num()) return value.as_int();
            if (value.is_str()) {
                int64_t parsed = 0;
                auto text = value.as_str();
                auto* first = text.data();
                auto* last = text.data() + text.size();
                auto result = std::from_chars(first, last, parsed);
                if (result.ec == std::errc{} && result.ptr == last) return parsed;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<bool> bool_field(
        cc::utils::json::JsonVal root,
        std::initializer_list<std::string_view> keys
    ) {
        for (auto key : keys) {
            auto value = root.get(key);
            if (value.is_bool()) return value.as_bool();
        }
        return std::nullopt;
    }

    [[nodiscard]] static Result<EnvironmentRegistration> parse_environment_registration(std::string_view body) {
        auto parsed = cc::utils::json::parse(body);
        if (!parsed || !parsed->root().is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest, "Bridge registration response is not valid JSON"));
        }
        auto root = parsed->root();
        auto environment_id = string_field(root, {"environment_id", "environmentId", "id"});
        auto environment_secret = string_field(root, {"environment_secret", "environmentSecret", "secret"});
        if (!environment_id || !environment_secret) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Bridge registration response is missing environment_id or environment_secret"));
        }
        return EnvironmentRegistration{
            .environment_id = *environment_id,
            .environment_secret = *environment_secret,
        };
    }

    [[nodiscard]] static Result<std::optional<WorkResponse>> parse_work_response(std::string_view body) {
        auto trimmed = std::string(body);
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
        auto first = trimmed.begin();
        while (first != trimmed.end() && std::isspace(static_cast<unsigned char>(*first))) ++first;
        trimmed.erase(trimmed.begin(), first);
        if (trimmed.empty() || trimmed == "null") return std::optional<WorkResponse>{};

        auto parsed = cc::utils::json::parse(trimmed);
        if (!parsed || !parsed->root().is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest, "Bridge poll response is not valid work JSON"));
        }
        auto root = parsed->root();
        auto id = string_field(root, {"id", "work_id", "workId"});
        auto secret = string_field(root, {"secret", "session_token", "sessionToken", "worker_jwt", "workerJwt"});
        auto data = root.get("data");
        std::optional<std::string> data_type;
        std::optional<std::string> data_id;
        if (data.is_obj()) {
            data_type = string_field(data, {"type", "data_type", "dataType"});
            data_id = string_field(data, {"id", "session_id", "sessionId", "data_id", "dataId"});
        }
        if (!data_type) data_type = string_field(root, {"data_type", "dataType", "type"});
        if (!data_id) data_id = string_field(root, {"data_id", "dataId", "session_id", "sessionId"});
        if (!id || !secret) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Bridge poll response is missing work id or session token"));
        }
        return std::optional<WorkResponse>{WorkResponse{
            .id = *id,
            .data_type = std::move(data_type),
            .data_id = std::move(data_id),
            .secret = *secret,
        }};
    }

    [[nodiscard]] static Result<HeartbeatResponse> parse_heartbeat_response(std::string_view body) {
        auto parsed = cc::utils::json::parse(body);
        if (!parsed || !parsed->root().is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest, "Bridge heartbeat response is not valid JSON"));
        }
        auto root = parsed->root();
        auto lease_extended = bool_field(root, {"lease_extended", "leaseExtended"});
        auto state = string_field(root, {"state"});
        if (!lease_extended || !state) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Bridge heartbeat response is missing lease_extended or state"));
        }
        return HeartbeatResponse{
            .lease_extended = *lease_extended,
            .state = *state,
            .last_heartbeat = string_field(root, {"last_heartbeat", "lastHeartbeat"}),
            .ttl_seconds = int_field(root, {"ttl_seconds", "ttlSeconds"}),
        };
    }

    [[nodiscard]] static Result<WorkerRegistration> parse_worker_registration(std::string_view body) {
        auto parsed = cc::utils::json::parse(body);
        if (!parsed || !parsed->root().is_obj()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest, "Bridge worker registration response is not valid JSON"));
        }
        auto root = parsed->root();
        auto worker_epoch = int64_field(root, {"worker_epoch", "workerEpoch"});
        if (!worker_epoch) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Bridge worker registration response is missing worker_epoch"));
        }
        return WorkerRegistration{.worker_epoch = *worker_epoch};
    }

    [[nodiscard]] auto post(std::string_view path, std::string_view body, const std::string& token) -> Result<cc::utils::HttpResponse> {
        auto response = http_.post(endpoint(path), body, get_headers(token));
        if (!response) {
            return std::unexpected(Error::make(ErrorCode::ConnectionFailed, response.error().message));
        }
        if (!response->is_ok()) {
            return std::unexpected(error_for_http_status(response->status));
        }
        return *response;
    }

    [[nodiscard]] static Error error_for_http_status(int status) {
        if (status == 401 || status == 403) {
            return Error::make(
                ErrorCode::AuthenticationFailed,
                std::format("Bridge API authentication failed (HTTP {})", status));
        }
        if (status == 429) {
            return Error::make(ErrorCode::RateLimited, "Bridge API returned HTTP 429");
        }
        return Error::make(
            ErrorCode::InvalidRequest,
            std::format("Bridge API returned HTTP {}", status));
    }

    [[nodiscard]] static auto transport_to_string(TransportType transport) -> std::string_view {
        switch (transport) {
            case TransportType::websocket: return "websocket";
            case TransportType::stdio: return "stdio";
            case TransportType::http_polling: return "http_polling";
        }
        return "websocket";
    }

    [[nodiscard]] static std::string strip_trailing_slashes(std::string value) {
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value;
    }

    [[nodiscard]] static std::string redact_session_url(std::string_view url) {
        auto text = std::string(url);
        auto marker = text.find("/v1/code/sessions/");
        if (marker == std::string::npos) return text;
        auto id_start = marker + std::string_view("/v1/code/sessions/").size();
        auto id_end = text.find('/', id_start);
        if (id_end == std::string::npos) id_end = text.size();
        text.replace(id_start, id_end - id_start, "<session>");
        return text;
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
