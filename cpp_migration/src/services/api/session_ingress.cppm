module;
#include <cstdlib>
#include <expected>
#include <format>
#include <initializer_list>
#include <charconv>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
export module cc.services.api.session_ingress;

import cc.utils.http;
import cc.utils.json;

export namespace cc::services::api {

// Configuration for session ingress connection
struct IngressConfig {
    std::string endpoint;
    std::string session_id;
    std::string auth_token;
    std::optional<std::string> organization_uuid;
    bool use_code_sessions = false;
    std::optional<int64_t> worker_epoch;
};

namespace detail {
    inline bool ingress_active = false;
    inline IngressConfig active_config;

    [[nodiscard]] inline std::string strip_trailing_slashes(std::string value) {
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value;
    }

    [[nodiscard]] inline bool starts_with(std::string_view text, std::string_view prefix) {
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    [[nodiscard]] inline std::optional<std::string> env_value(const char* name) {
        if (const char* value = std::getenv(name); value && *value) return std::string(value);
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<std::string> first_env(std::initializer_list<const char*> names) {
        for (auto* name : names) {
            if (auto value = env_value(name)) return value;
        }
        return std::nullopt;
    }

    [[nodiscard]] inline bool truthy(std::string_view value) {
        return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
    }

    [[nodiscard]] inline std::optional<int64_t> env_int64(const char* name) {
        auto value = env_value(name);
        if (!value) return std::nullopt;
        int64_t parsed = 0;
        auto* first = value->data();
        auto* last = value->data() + value->size();
        auto result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;
        return parsed;
    }

    [[nodiscard]] inline std::string json_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char ch : value) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(ch); break;
            }
        }
        return out;
    }

    [[nodiscard]] inline std::unordered_map<std::string, std::string> auth_headers(const IngressConfig& config) {
        std::unordered_map<std::string, std::string> headers{
            {"Content-Type", "application/json"},
            {"Accept", "application/json"},
        };
        if (config.auth_token.empty()) return headers;

        if (starts_with(config.auth_token, "sk-ant-sid")) {
            headers["Cookie"] = "sessionKey=" + config.auth_token;
            auto org_uuid = config.organization_uuid;
            if (!org_uuid) org_uuid = env_value("CLAUDE_CODE_ORGANIZATION_UUID");
            if (org_uuid && !org_uuid->empty()) {
                headers["X-Organization-Uuid"] = *org_uuid;
            }
            return headers;
        }

        headers["Authorization"] = "Bearer " + config.auth_token;
        return headers;
    }

    [[nodiscard]] inline std::string session_events_url(const IngressConfig& config) {
        return std::format(
            "{}/v1/sessions/{}/events",
            strip_trailing_slashes(config.endpoint),
            config.session_id);
    }

    [[nodiscard]] inline std::string worker_events_url(const IngressConfig& config) {
        return std::format(
            "{}/v1/code/sessions/{}/worker/events",
            strip_trailing_slashes(config.endpoint),
            config.session_id);
    }

    [[nodiscard]] inline std::string worker_url(const IngressConfig& config) {
        return std::format(
            "{}/v1/code/sessions/{}/worker",
            strip_trailing_slashes(config.endpoint),
            config.session_id);
    }

    [[nodiscard]] inline std::string worker_heartbeat_url(const IngressConfig& config) {
        return worker_url(config) + "/heartbeat";
    }

    [[nodiscard]] inline std::string worker_delivery_url(const IngressConfig& config) {
        return worker_events_url(config) + "/delivery";
    }

    [[nodiscard]] inline std::string session_events_body(std::string_view message) {
        return std::format(R"({{"events":[{}]}})", message);
    }

    [[nodiscard]] inline std::string worker_events_body(const IngressConfig& config, std::string_view message) {
        return std::format(
            R"({{"worker_epoch":{},"events":[{{"payload":{}}}]}})",
            *config.worker_epoch,
            message);
    }

    [[nodiscard]] inline bool worker_active(const IngressConfig& config) {
        return config.use_code_sessions && config.worker_epoch.has_value();
    }

    [[nodiscard]] inline std::string worker_state_body(
        const IngressConfig& config,
        std::string_view status,
        bool clear_metadata
    ) {
        std::string body = std::format(
            R"({{"worker_epoch":{},"worker_status":"{}","requires_action_details":null)",
            *config.worker_epoch,
            json_escape(status));
        if (clear_metadata) {
            body += R"(,"external_metadata":{"pending_action":null,"task_summary":null})";
        }
        body += '}';
        return body;
    }

    [[nodiscard]] inline std::string worker_heartbeat_body(const IngressConfig& config) {
        return std::format(
            R"({{"session_id":"{}","worker_epoch":{}}})",
            json_escape(config.session_id),
            *config.worker_epoch);
    }

    [[nodiscard]] inline std::string worker_delivery_body(
        const IngressConfig& config,
        std::string_view event_id,
        std::string_view status
    ) {
        return std::format(
            R"({{"worker_epoch":{},"updates":[{{"event_id":"{}","status":"{}"}}]}})",
            *config.worker_epoch,
            json_escape(event_id),
            json_escape(status));
    }
} // namespace detail

// Create a new ingress connection for session streaming
auto create_ingress(IngressConfig config) -> std::expected<void, std::string> {
    if (config.endpoint.empty()) {
        return std::unexpected("Ingress endpoint is required");
    }
    if (config.session_id.empty()) {
        return std::unexpected("Session ID is required");
    }
    if (config.auth_token.empty()) {
        return std::unexpected("Ingress auth token is required");
    }
    config.endpoint = detail::strip_trailing_slashes(std::move(config.endpoint));
    detail::active_config = std::move(config);
    detail::ingress_active = true;
    return {};
}

auto create_ingress_from_environment() -> std::expected<bool, std::string> {
    auto endpoint = detail::first_env({
        "CLAUDE_CODE_REMOTE_API_BASE_URL",
        "CC_REPL_REMOTE_API_BASE_URL",
        "CLAUDE_CODE_SESSION_INGRESS_URL",
        "CC_REPL_SESSION_INGRESS_URL",
    });
    auto session_id = detail::first_env({
        "CC_REMOTE_SESSION_ID",
        "CLAUDE_CODE_REMOTE_SESSION_ID",
    });
    auto auth_token = detail::env_value("CLAUDE_CODE_SESSION_ACCESS_TOKEN");
    auto worker_epoch = detail::env_int64("CLAUDE_CODE_WORKER_EPOCH");
    auto use_code_sessions = detail::first_env({
        "CLAUDE_CODE_USE_CCR_V2",
        "CLAUDE_CODE_USE_CODE_SESSIONS",
    });

    if (!endpoint && !session_id && !auth_token) return false;

    std::vector<std::string> missing;
    if (!endpoint) missing.push_back("endpoint");
    if (!session_id) missing.push_back("session_id");
    if (!auth_token) missing.push_back("auth_token");
    if (!missing.empty()) {
        std::string message = "Session ingress environment incomplete: missing ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) message += ", ";
            message += missing[i];
        }
        return std::unexpected(message);
    }

    auto created = create_ingress(IngressConfig{
        .endpoint = *endpoint,
        .session_id = *session_id,
        .auth_token = *auth_token,
        .organization_uuid = detail::env_value("CLAUDE_CODE_ORGANIZATION_UUID"),
        .use_code_sessions = use_code_sessions ? detail::truthy(*use_code_sessions) : false,
        .worker_epoch = worker_epoch,
    });
    if (!created) return std::unexpected(created.error());
    return true;
}

// Send a message through the ingress channel
auto send_ingress_message(std::string_view message) -> std::expected<void, std::string> {
    if (!detail::ingress_active) {
        return std::unexpected("No active ingress connection");
    }
    if (message.empty()) {
        return std::unexpected("Cannot send empty message");
    }
    auto parsed = cc::utils::json::parse(message);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Ingress message must be a JSON object");
    }

    cc::utils::HttpClient http;
    const bool use_worker_events = detail::worker_active(detail::active_config);
    auto response = http.post(
        use_worker_events
            ? detail::worker_events_url(detail::active_config)
            : detail::session_events_url(detail::active_config),
        use_worker_events
            ? detail::worker_events_body(detail::active_config, message)
            : detail::session_events_body(message),
        detail::auth_headers(detail::active_config));
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Session ingress returned HTTP {}", response->status));
    }
    return {};
}

auto is_worker_lifecycle_active() -> bool {
    return detail::ingress_active && detail::worker_active(detail::active_config);
}

auto send_worker_state(std::string_view status, bool clear_metadata = false) -> std::expected<void, std::string> {
    if (!detail::ingress_active) return std::unexpected("No active ingress connection");
    if (!detail::worker_active(detail::active_config)) return {};
    if (status.empty()) return std::unexpected("Worker status is required");

    cc::utils::HttpClient http;
    auto response = http.put(
        detail::worker_url(detail::active_config),
        detail::worker_state_body(detail::active_config, status, clear_metadata),
        detail::auth_headers(detail::active_config));
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Worker state returned HTTP {}", response->status));
    }
    return {};
}

auto send_worker_heartbeat() -> std::expected<void, std::string> {
    if (!detail::ingress_active) return std::unexpected("No active ingress connection");
    if (!detail::worker_active(detail::active_config)) return {};

    cc::utils::HttpClient http(cc::utils::HttpConfig{.timeout_ms = 5'000});
    auto response = http.post(
        detail::worker_heartbeat_url(detail::active_config),
        detail::worker_heartbeat_body(detail::active_config),
        detail::auth_headers(detail::active_config));
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Worker heartbeat returned HTTP {}", response->status));
    }
    return {};
}

auto send_worker_delivery(std::string_view event_id, std::string_view status) -> std::expected<void, std::string> {
    if (!detail::ingress_active) return std::unexpected("No active ingress connection");
    if (!detail::worker_active(detail::active_config)) return {};
    if (event_id.empty()) return std::unexpected("Delivery event_id is required");
    if (status.empty()) return std::unexpected("Delivery status is required");

    cc::utils::HttpClient http;
    auto response = http.post(
        detail::worker_delivery_url(detail::active_config),
        detail::worker_delivery_body(detail::active_config, event_id, status),
        detail::auth_headers(detail::active_config));
    if (!response) return std::unexpected(response.error().message);
    if (!response->is_ok()) {
        return std::unexpected(std::format("Worker delivery returned HTTP {}", response->status));
    }
    return {};
}

auto send_ingress_lifecycle_event(
    std::string_view status,
    std::optional<std::string_view> bridge_work_id = std::nullopt
) -> std::expected<void, std::string> {
    if (status.empty()) return std::unexpected("Lifecycle status is required");
    std::string body = std::format(
        R"({{"type":"session_lifecycle","status":"{}")",
        detail::json_escape(status));
    if (bridge_work_id && !bridge_work_id->empty()) {
        body += std::format(
            R"(,"bridge_work_id":"{}")",
            detail::json_escape(*bridge_work_id));
    }
    body += '}';
    return send_ingress_message(body);
}

[[nodiscard]] auto is_ingress_active() -> bool {
    return detail::ingress_active;
}

// Close the active ingress connection
auto close_ingress() -> void {
    detail::ingress_active = false;
    detail::active_config = IngressConfig{};
}

} // namespace cc::services::api
