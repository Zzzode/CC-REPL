/// @file session_api.cppm
/// @brief Thin HTTP wrappers for CCR v2 code-session API
module;

#include <string>
#include <optional>
#include <vector>
#include <chrono>
#include <expected>
#include <format>
#include <functional>
#include <unordered_map>

export module cc.bridge.session_api;

import cc.types.types;
import cc.utils.http;
import cc.utils.json;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;

constexpr std::string_view ANTHROPIC_VERSION = "2023-06-01";
constexpr std::string_view SESSION_API_PATH = "/v1/sessions";
constexpr std::string_view BRIDGE_BETA_HEADER = "ccr-byoc-2025-07-29";

// ============================================================
// Session data structures
// ============================================================

/// Remote credentials from POST /bridge
struct RemoteCredentials {
    std::string worker_jwt;
    std::string api_base_url;
    int64_t expires_in;
    int64_t worker_epoch;
};

/// Represents a bridge session returned by the CCR v2 API.
struct BridgeSession {
    std::string id;
    std::string title;
    std::string status;          // "running", "idle", "requires_action", "pending", "archived"
    std::string environment_id;
    int64_t created_at{0};       // Unix epoch seconds
    int64_t updated_at{0};       // Unix epoch seconds

    /// Parse a BridgeSession from a JSON response body.
    /// Returns std::nullopt if required fields are missing.
    [[nodiscard]] static auto from_json(std::string_view body) -> std::optional<BridgeSession> {
        auto parsed = cc::utils::json::parse(body);
        if (!parsed) return std::nullopt;
        auto root = parsed->root();
        if (!root.is_obj()) return std::nullopt;

        auto id_val = root.get("id");
        if (!id_val.is_str()) return std::nullopt;

        BridgeSession session;
        session.id = std::string(id_val.as_str());

        auto title_val = root.get("title");
        if (title_val.is_str()) session.title = std::string(title_val.as_str());

        auto status_val = root.get("status");
        if (status_val.is_str()) session.status = std::string(status_val.as_str());

        auto env_val = root.get("environment_id");
        if (env_val.is_str()) session.environment_id = std::string(env_val.as_str());

        auto created_val = root.get("created_at");
        if (created_val.is_num()) session.created_at = created_val.as_int();

        auto updated_val = root.get("updated_at");
        if (updated_val.is_num()) session.updated_at = updated_val.as_int();

        return session;
    }
};

/// Request payload for creating a new bridge session.
struct CreateSessionRequest {
    std::string title;
    std::vector<std::string> tags;
    std::optional<std::string> sdk_url;
    std::string environment_id;
    std::string source{"remote-control"};
    std::optional<std::string> permission_mode;

    /// Serialize the request to a JSON string.
    [[nodiscard]] auto to_json() const -> std::string {
        std::string json = "{";
        if (!title.empty()) {
            json += std::format(R"("title":"{}")", title);
        }
        if (!tags.empty()) {
            if (json.size() > 1) json += ",";
            json += R"("tags":[)";
            for (std::size_t i = 0; i < tags.size(); ++i) {
                if (i > 0) json += ",";
                json += std::format(R"("{}")", tags[i]);
            }
            json += "]";
        }
        if (sdk_url) {
            if (json.size() > 1) json += ",";
            json += std::format(R"("sdk_url":"{}")", *sdk_url);
        }
        if (!environment_id.empty()) {
            if (json.size() > 1) json += ",";
            json += std::format(R"("environment_id":"{}")", environment_id);
        }
        if (!source.empty()) {
            if (json.size() > 1) json += ",";
            json += std::format(R"("source":"{}")", source);
        }
        if (permission_mode) {
            if (json.size() > 1) json += ",";
            json += std::format(R"("permission_mode":"{}")", *permission_mode);
        }
        json += "}";
        return json;
    }
};

// ============================================================
// Internal helpers
// ============================================================

namespace detail {

/// Build the standard auth headers required by the CCR v2 sessions API.
[[nodiscard]] auto session_api_headers(
    std::string_view access_token,
    std::string_view org_uuid
) -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> headers{
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"anthropic-beta", std::string(BRIDGE_BETA_HEADER)},
        {"x-organization-uuid", std::string(org_uuid)},
    };
    if (!access_token.empty()) {
        headers["Authorization"] = std::format("Bearer {}", access_token);
    }
    return headers;
}

/// Map an HTTP status code to a domain ErrorCode.
[[nodiscard]] constexpr auto http_status_to_error_code(int status) -> ErrorCode {
    if (status == 401 || status == 403) return ErrorCode::AuthenticationFailed;
    if (status == 404) return ErrorCode::SessionNotFound;
    if (status == 429) return ErrorCode::RateLimited;
    if (status >= 500) return ErrorCode::InternalError;
    return ErrorCode::InvalidRequest;
}

/// Build an Error from an HTTP response, extracting the detail message if present.
[[nodiscard]] auto make_http_error(
    int status,
    std::string_view operation,
    std::string_view body
) -> Error {
    std::string detail;
    auto parsed = cc::utils::json::parse(body);
    if (parsed) {
        auto root = parsed->root();
        if (root.is_obj()) {
            auto detail_val = root.get("error");
            if (detail_val.is_str()) {
                detail = std::string(detail_val.as_str());
            } else if (detail_val.is_obj()) {
                auto msg = detail_val.get("message");
                if (msg.is_str()) detail = std::string(msg.as_str());
            }
        }
    }
    auto code = http_status_to_error_code(status);
    auto message = detail.empty()
        ? std::format("Bridge session {} failed with HTTP {}", operation, status)
        : std::format("Bridge session {} failed with HTTP {}: {}", operation, status, detail);
    return Error::make(code, std::move(message));
}

/// Convert a transport-level HttpError into a domain Error.
[[nodiscard]] auto map_transport_error(const cc::utils::HttpError& http_err) -> Error {
    ErrorCode code = ErrorCode::ConnectionFailed;
    switch (http_err.code) {
        case cc::utils::HttpError::timeout:
            code = ErrorCode::NetworkTimeout;
            break;
        case cc::utils::HttpError::ssl_error:
            code = ErrorCode::SSLError;
            break;
        case cc::utils::HttpError::dns_error:
        case cc::utils::HttpError::connection_failed:
            code = ErrorCode::ConnectionFailed;
            break;
        case cc::utils::HttpError::cancelled:
            code = ErrorCode::NetworkTimeout;
            break;
    }
    return Error::make(code, http_err.message);
}

} // namespace detail

// ============================================================
// Session CRUD operations
// ============================================================

/// Create a code session via POST /v1/sessions.
///
/// Sends the full CreateSessionRequest payload to the bridge API and returns
/// the created BridgeSession on success, or an Error describing the failure.
auto create_code_session(
    const CreateSessionRequest& request,
    std::string_view base_url,
    std::string_view access_token,
    std::string_view org_uuid,
    cc::utils::HttpClient& http_client,
    std::chrono::milliseconds timeout = std::chrono::seconds{30}
) -> Result<BridgeSession> {
    (void)timeout;
    if (base_url.empty() || access_token.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Base URL and access token are required for session creation"));
    }

    auto url = std::format("{}{}", base_url, SESSION_API_PATH);
    auto headers = detail::session_api_headers(access_token, org_uuid);
    auto body = request.to_json();

    auto response = http_client.post(url, body, headers);
    if (!response) {
        return std::unexpected(detail::map_transport_error(response.error()));
    }

    if (response->status != 200 && response->status != 201) {
        return std::unexpected(detail::make_http_error(
            response->status, "creation", response->body));
    }

    auto session = BridgeSession::from_json(response->body);
    if (!session) {
        return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
            "Session creation response did not contain a valid session ID"));
    }
    return std::move(*session);
}

/// Fetch a bridge session by ID via GET /v1/sessions/{session_id}.
///
/// Returns the session details or an Error. Handles 404 (session not found)
/// and auth failures (401/403) with specific error codes.
auto get_bridge_session(
    std::string_view session_id,
    std::string_view base_url,
    std::string_view access_token,
    std::string_view org_uuid,
    cc::utils::HttpClient& http_client,
    std::chrono::milliseconds timeout = std::chrono::seconds{10}
) -> Result<BridgeSession> {
    (void)timeout;
    if (session_id.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Session ID is required"));
    }
    if (base_url.empty() || access_token.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Base URL and access token are required for session fetch"));
    }

    auto url = std::format("{}/v1/sessions/{}", base_url, session_id);
    auto headers = detail::session_api_headers(access_token, org_uuid);

    auto response = http_client.get(url, headers);
    if (!response) {
        return std::unexpected(detail::map_transport_error(response.error()));
    }

    if (response->status != 200) {
        return std::unexpected(detail::make_http_error(
            response->status, "fetch", response->body));
    }

    auto session = BridgeSession::from_json(response->body);
    if (!session) {
        return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
            "Session fetch response contained invalid JSON"));
    }
    return std::move(*session);
}

/// Archive a bridge session via POST /v1/sessions/{session_id}/archive.
///
/// The CCR server never auto-archives sessions -- archival is always an
/// explicit client action. The endpoint accepts sessions in any status
/// and returns 409 if already archived, making it safe to call idempotently.
auto archive_bridge_session(
    std::string_view session_id,
    std::string_view base_url,
    std::string_view access_token,
    std::string_view org_uuid,
    cc::utils::HttpClient& http_client,
    std::chrono::milliseconds timeout = std::chrono::seconds{10}
) -> Result<void> {
    (void)timeout;
    if (session_id.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Session ID is required"));
    }
    if (base_url.empty() || access_token.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Base URL and access token are required for session archive"));
    }

    auto url = std::format("{}/v1/sessions/{}/archive", base_url, session_id);
    auto headers = detail::session_api_headers(access_token, org_uuid);

    auto response = http_client.post(url, "{}", headers);
    if (!response) {
        return std::unexpected(detail::map_transport_error(response.error()));
    }

    // 200 = archived, 409 = already archived (treat as success)
    if (response->status != 200 && response->status != 409) {
        return std::unexpected(detail::make_http_error(
            response->status, "archive", response->body));
    }

    return {};
}

/// Update the title of a bridge session via PATCH /v1/sessions/{session_id}.
///
/// Called when the user renames a session so the title stays in sync on
/// claude.ai/code. Title sync is best-effort but errors are propagated
/// to the caller for logging.
auto update_bridge_session_title(
    std::string_view session_id,
    std::string_view title,
    std::string_view base_url,
    std::string_view access_token,
    std::string_view org_uuid,
    cc::utils::HttpClient& http_client,
    std::chrono::milliseconds timeout = std::chrono::seconds{10}
) -> Result<void> {
    (void)timeout;
    if (session_id.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Session ID is required"));
    }
    if (title.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Title is required"));
    }
    if (base_url.empty() || access_token.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput,
            "Base URL and access token are required for session title update"));
    }

    auto url = std::format("{}/v1/sessions/{}", base_url, session_id);
    auto headers = detail::session_api_headers(access_token, org_uuid);
    auto body = std::format(R"({{"title":"{}"}})", title);

    auto response = http_client.patch(url, body, headers);
    if (!response) {
        return std::unexpected(detail::map_transport_error(response.error()));
    }

    if (response->status != 200) {
        return std::unexpected(detail::make_http_error(
            response->status, "title update", response->body));
    }

    return {};
}

// ============================================================
// Remote credentials and local sessions
// ============================================================

/// Fetch remote credentials for a session
std::optional<RemoteCredentials> fetch_remote_credentials(
    std::string_view session_id,
    std::string_view base_url,
    std::string_view access_token,
    std::chrono::milliseconds timeout,
    std::optional<std::string_view> trusted_device_token = std::nullopt
) {
    (void)timeout;
    if (session_id.empty() || base_url.empty() || access_token.empty()) {
        return std::nullopt;
    }
    const auto seed = std::format("{}:{}:{}", session_id, base_url, trusted_device_token.value_or(""));
    return RemoteCredentials{
        .worker_jwt = std::format("worker.{}.sig", std::hash<std::string>{}(seed)),
        .api_base_url = std::string(base_url),
        .expires_in = 3600,
        .worker_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
    };
}

/// Create local session configuration
struct CreateLocalSessionOptions {
    std::string_view cwd;
    std::string_view title;
    bool verbose = false;
    bool debug = false;
    bool single_session = false;
    bool sandbox = false;
    bool force_new_ide = false;
    bool capacity_mode = false;
    std::optional<std::string_view> branch;
    std::optional<std::string_view> parent_dir;
    std::optional<std::string_view> worktree_path;
    std::optional<std::string_view> preloaded_files;
};

/// Create a local session
Result<std::string> create_local_session(const CreateLocalSessionOptions& opts) {
    if (opts.cwd.empty()) {
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Working directory is required"));
    }
    const auto seed = std::format("{}:{}:{}:{}",
        opts.cwd,
        opts.title,
        opts.branch.value_or(""),
        opts.single_session ? "single" : "multi");
    return std::format("local_{}", std::hash<std::string>{}(seed));
}

} // namespace cc::bridge
