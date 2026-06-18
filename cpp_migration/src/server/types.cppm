/// @file types.cppm
/// @brief Canonical server-facing DTOs used by the direct-connect HTTP + streaming
///        API surface.  All structs provide JSON round-trip helpers (to_json /
///        from_json) so routes can ser/de request/response bodies uniformly.
///
/// Note on server_routes.cppm overlap:
///   cc::server::detail already contains DirectQueryRequest / DirectQueryResult /
///   DirectPermissionRequest / DirectPermissionRule / DirectPermissionDirectory /
///   DirectPermissionSessionState as route-local helpers.  Those structs are
///   intentionally kept internal to cc.server.server_routes (detail namespace,
///   different shape — they carry cancel flags, filesystem paths, etc.).  The types
///   below are the canonical HTTP DTOs.  Future refactors are expected to make
///   server_routes convert between its internal helpers and these public structs.

module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.server.types;

import cc.utils.json;

export namespace cc::server {

// ─── Enums ──────────────────────────────────────────────────────────────────

/// Authorization role assigned to an authenticated server session.
enum class Role : uint8_t {
    /// No credentials provided or anonymous.
    Guest,
    /// Normal user (default for human interactive sessions).
    User,
    /// Administrative user (server-wide configuration, permission rule edits).
    Admin,
    /// Machine-to-machine service account.
    Service,
    /// Break-glass super-user role (reserved for emergency ops).
    SuperAdmin,
};

/// Decision that a permission rule / request resolves to.  Mirrors a subset of
/// cc.hooks.permission but is reproduced here so the server module does not need
/// to pull in the full hooks dependency tree.
enum class PermissionDecision : uint8_t {
    /// Ask the user interactively for this invocation.
    Ask,
    /// Allow once; re-prompt on the next invocation of the same pattern.
    AllowOnce,
    /// Always allow matching patterns without prompting.
    AlwaysAllow,
    /// Deny this single invocation.
    Deny,
    /// Remember this deny permanently.
    AlwaysDeny,
    /// Abort the enclosing operation entirely (e.g. hard-deny + drop session).
    Abort,
};

/// Discriminant for DirectQueryStreamChunk.
enum class StreamChunkKind {
    /// Plain text delta appended to the assistant's response.
    ContentDelta,
    /// End-of-stream marker with stop reason (e.g. "end_turn", "tool_use").
    StopReason,
    /// A tool-use block has been started (tool_use_id, tool_name, tool_input_json).
    ToolUseBegin,
    /// A tool-use block has completed (tool_use_id, tool_output_json).
    ToolUseEnd,
    /// Irrecoverable error.  No further chunks will be sent for this stream.
    Error,
    /// Periodic keep-alive.  All other fields are empty.
    Heartbeat,
};

// ─── Session ─────────────────────────────────────────────────────────────────

/// Server-side representation of a direct-connect session.  The token is opaque
/// (opaque bearer token; JWT is produced by the bridge for IDE connections) and
/// scopes are simple string tags matched by has_scope().
struct ServerSession {
    std::string id;
    std::string token;
    Role role = Role::Guest;
    std::string user_id;
    std::string user_agent;
    std::string client_ip;
    std::vector<std::string> scopes;
    /// Epoch-millisecond timestamps (std::chrono::system_clock origin).
    int64_t created_ms = 0;
    int64_t expires_ms = 0;
    int64_t last_active_ms = 0;
    uint64_t request_count = 0;
    bool revoked = false;

    /// Case-sensitive exact scope match.  An empty required scope is trivially
    /// satisfied (returns true).
    [[nodiscard]] bool has_scope(std::string_view s) const {
        if (s.empty()) return true;
        return std::find(scopes.begin(), scopes.end(), s) != scopes.end();
    }

    /// True when the session has passed its expiry wall-clock or has been revoked.
    /// Passing a custom `now` lets callers inject a test clock.
    [[nodiscard]] bool is_expired(std::optional<int64_t> now = std::nullopt) const {
        if (revoked) return true;
        if (expires_ms == 0) return false;
        const int64_t t = now.value_or([] {
            using namespace std::chrono;
            return duration_cast<milliseconds>(
                       system_clock::now().time_since_epoch())
                .count();
        }());
        return t >= expires_ms;
    }
};

// ─── Direct-connect handshake ────────────────────────────────────────────────

/// Initial HTTP POST body that clients use to establish a direct-connect session.
struct DirectConnectRequest {
    /// Existing session id (optional; empty for "new anonymous").
    std::string session_id;
    /// Bearer token produced by the authentication layer (OAuth / JWT / API key).
    std::string auth_token;
    /// Opaque client identifier used for logging / rate-limit bucketing.
    std::string client_id;
    /// Desired role requested by the caller; server may downgrade.
    std::optional<Role> desired_role;
    /// Session lifetime in seconds.  0 → server default (3600).
    int64_t ttl_seconds = 3600;
    /// OAuth-like scope tags the caller wants; server may narrow them.
    std::vector<std::string> requested_scopes;
};

/// HTTP response to DirectConnectRequest.
struct DirectConnectResponse {
    bool ok = false;
    std::string session_id;
    /// Bearer token the client must present on subsequent calls.
    std::string token;
    /// Non-empty on failure (human-readable).
    std::string error;
    Role assigned_role = Role::Guest;
    /// Exact expiry timestamp the server committed to (epoch ms).
    int64_t expires_ms = 0;
};

// ─── Direct query (synchronous + streaming) ──────────────────────────────────

/// Body of POST /v1/direct/query.
struct DirectQueryRequest {
    std::string session_id;
    /// User prompt (raw text).  Callers may also pass richer messages via JSON; the
    /// server normalises both.
    std::string prompt;
    /// Desired model id.  If empty the server picks the session's default model.
    std::string model;
    /// Optional override of the server's built-in system prompt.
    std::string system_prompt_override;
    /// Conversation id for stateful continuation (new chat if empty).
    std::string conversation_id;
    /// Hard cap on output tokens (0 → server default applies).
    int max_tokens = 0;
    /// Sampling temperature.  < 0 → server default.
    double temperature = 0;
    /// If true the response is streamed via chunked transfer encoding.
    bool stream = false;
    /// Tool allowlist (empty = server default, ["none"] = no tools, ["*"] = all).
    std::vector<std::string> allowed_tools;
};

/// Final JSON response to a non-streamed direct query.
struct DirectQueryResult {
    bool ok = false;
    std::string conversation_id;
    /// The assistant reply body (plain text).
    std::string response_text;
    /// Actual model used (may differ from request due to fallbacks / overrides).
    std::string model_used;
    /// Non-empty on failure.
    std::string error;
    uint64_t prompt_tokens = 0;
    uint64_t completion_tokens = 0;
    /// Approximate USD cost (zero if cost lookup is unavailable).
    double cost_usd = 0;
    /// HTTP status code mirror (used for logging / retries).
    int status_code = 0;
};

// ─── Permission rules & state ────────────────────────────────────────────────

/// Body of POST /v1/direct/permission (one-off prompt).
struct DirectPermissionRequest {
    std::string session_id;
    /// E.g. "bash", "file_write", "mcp:server:tool".
    std::string tool_name;
    /// Human-readable description of the action being approved.
    std::string action;
    /// Affected filesystem / resource paths (used for path-pattern matching).
    std::vector<std::string> affected_paths;
    /// What the caller would like to happen (defaults to Ask).
    PermissionDecision requested = PermissionDecision::Ask;
    /// Free-form context shown to the approving user.
    std::string reason;
};

/// A persisted rule matching (tool, path) pairs to a decision.
struct DirectPermissionRule {
    std::string id;
    /// Glob-style match on the tool_name (e.g. "bash", "file_*", "*").
    std::string tool_pattern;
    /// Glob-style match on affected_paths (e.g. "/etc/*", "**/.env").
    std::string path_pattern;
    /// User id of who created this rule (for auditing).
    std::string created_by;
    PermissionDecision decision = PermissionDecision::Ask;
    /// Epoch ms timestamps; expires_ms=0 means "never expires".
    int64_t created_ms = 0;
    int64_t expires_ms = 0;
};

/// A named directory of rules — typically per-user, per-team, or a global policy.
struct DirectPermissionDirectory {
    std::vector<DirectPermissionRule> rules;
};

/// Permission state attached to a particular ServerSession.
struct DirectPermissionSessionState {
    std::string session_id;
    /// Rules that persist across sessions (e.g. user said "always allow").
    DirectPermissionDirectory permanent_rules;
    /// Rules that only live for this session (e.g. "allow once").
    std::vector<DirectPermissionRule> session_only_rules;
    /// Tool-name / pattern keys recently denied; used to suppress repeat prompts.
    std::vector<std::string> recently_denied;
};

// ─── Streaming chunks ────────────────────────────────────────────────────────

/// One server-sent event in the direct-query streaming protocol.
struct DirectQueryStreamChunk {
    std::string conversation_id;
    StreamChunkKind kind = StreamChunkKind::Heartbeat;
    /// Populated for ContentDelta.
    std::string delta;
    /// Populated for StopReason.
    std::string stop_reason;
    /// Populated for ToolUseBegin / ToolUseEnd.
    std::string tool_use_id;
    std::string tool_name;
    std::string tool_input_json;
    std::string tool_output_json;
    /// Populated for Error.
    std::string error_message;
};

// ─────────────────────────────────────────────────────────────────────────────
// Ser/de helpers
//
// These use the yyjson-backed cc.utils.json module.  For each type T we emit
//   std::string        T_to_json(const T&);
//   std::expected<T, std::string> T_from_json(std::string_view);
// plus a free-function to_json / from_json overload inside the cc::server
// namespace so ADL callers can say `to_json(x)` uniformly.
// ─────────────────────────────────────────────────────────────────────────────

namespace detail_serde {

inline auto role_to_str(Role r) -> std::string_view {
    switch (r) {
        case Role::Guest:      return "guest";
        case Role::User:       return "user";
        case Role::Admin:      return "admin";
        case Role::Service:    return "service";
        case Role::SuperAdmin: return "super_admin";
    }
    return "guest";
}
inline auto role_from_str(std::string_view s) -> std::optional<Role> {
    if (s == "guest")       return Role::Guest;
    if (s == "user")        return Role::User;
    if (s == "admin")       return Role::Admin;
    if (s == "service")     return Role::Service;
    if (s == "super_admin") return Role::SuperAdmin;
    return std::nullopt;
}

inline auto decision_to_str(PermissionDecision d) -> std::string_view {
    switch (d) {
        case PermissionDecision::Ask:         return "ask";
        case PermissionDecision::AllowOnce:   return "allow_once";
        case PermissionDecision::AlwaysAllow: return "always_allow";
        case PermissionDecision::Deny:        return "deny";
        case PermissionDecision::AlwaysDeny:  return "always_deny";
        case PermissionDecision::Abort:       return "abort";
    }
    return "ask";
}
inline auto decision_from_str(std::string_view s) -> std::optional<PermissionDecision> {
    if (s == "ask")          return PermissionDecision::Ask;
    if (s == "allow_once")   return PermissionDecision::AllowOnce;
    if (s == "always_allow") return PermissionDecision::AlwaysAllow;
    if (s == "deny")         return PermissionDecision::Deny;
    if (s == "always_deny")  return PermissionDecision::AlwaysDeny;
    if (s == "abort")        return PermissionDecision::Abort;
    return std::nullopt;
}

inline auto chunk_kind_to_str(StreamChunkKind k) -> std::string_view {
    switch (k) {
        case StreamChunkKind::ContentDelta: return "content_delta";
        case StreamChunkKind::StopReason:   return "stop_reason";
        case StreamChunkKind::ToolUseBegin: return "tool_use_begin";
        case StreamChunkKind::ToolUseEnd:   return "tool_use_end";
        case StreamChunkKind::Error:        return "error";
        case StreamChunkKind::Heartbeat:    return "heartbeat";
    }
    return "heartbeat";
}
inline auto chunk_kind_from_str(std::string_view s) -> std::optional<StreamChunkKind> {
    if (s == "content_delta") return StreamChunkKind::ContentDelta;
    if (s == "stop_reason")   return StreamChunkKind::StopReason;
    if (s == "tool_use_begin") return StreamChunkKind::ToolUseBegin;
    if (s == "tool_use_end")  return StreamChunkKind::ToolUseEnd;
    if (s == "error")         return StreamChunkKind::Error;
    if (s == "heartbeat")     return StreamChunkKind::Heartbeat;
    return std::nullopt;
}

/// Populate a string-vector JSON array from a JSON value, into v.  Returns false
/// if `val` exists but is not an array; a missing key returns true without touching
/// v (preserves default values of the struct).
inline bool read_string_vec(cc::utils::json::JsonVal obj, std::string_view key,
                            std::vector<std::string>& out) {
    cc::utils::json::JsonVal arr = obj.get(key);
    if (!arr.valid() || arr.is_null()) return true;
    if (!arr.is_arr()) return false;
    arr.iter([&](cc::utils::json::JsonVal el) {
        if (el.is_str()) out.emplace_back(el.as_str());
    });
    return true;
}

}  // namespace detail_serde

// ─── ServerSession ───────────────────────────────────────────────────────────

[[nodiscard]] inline auto ServerSession_to_json(const ServerSession& s) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto obj = doc.object();
    obj.add("id", doc.string(s.id));
    obj.add("token", doc.string(s.token));
    obj.add("role", doc.string(std::string{detail_serde::role_to_str(s.role)}));
    obj.add("user_id", doc.string(s.user_id));
    obj.add("user_agent", doc.string(s.user_agent));
    obj.add("client_ip", doc.string(s.client_ip));
    auto sc = doc.array();
    for (const auto& x : s.scopes) sc.append(doc.string(x));
    obj.add("scopes", std::move(sc));
    obj.add("created_ms", doc.number(static_cast<int64_t>(s.created_ms)));
    obj.add("expires_ms", doc.number(static_cast<int64_t>(s.expires_ms)));
    obj.add("last_active_ms", doc.number(static_cast<int64_t>(s.last_active_ms)));
    obj.add("request_count", doc.number(static_cast<int64_t>(static_cast<int64_t>(s.request_count))));
    obj.add("revoked", doc.boolean(s.revoked));
    doc.set_root(std::move(obj));
    return doc.to_string();
}
[[nodiscard]] inline auto ServerSession_from_json(std::string_view raw)
    -> std::expected<ServerSession, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    ServerSession s;
    s.id = std::string{o.get("id").as_str()};
    s.token = std::string{o.get("token").as_str()};
    if (auto r = detail_serde::role_from_str(o.get("role").as_str())) s.role = *r;
    s.user_id = std::string{o.get("user_id").as_str()};
    s.user_agent = std::string{o.get("user_agent").as_str()};
    s.client_ip = std::string{o.get("client_ip").as_str()};
    if (!detail_serde::read_string_vec(o, "scopes", s.scopes)) {
        return std::unexpected<std::string>{"scopes: expected array"};
    }
    s.created_ms = o.get("created_ms").as_int();
    s.expires_ms = o.get("expires_ms").as_int();
    s.last_active_ms = o.get("last_active_ms").as_int();
    s.request_count = static_cast<uint64_t>(o.get("request_count").as_int());
    s.revoked = o.get("revoked").as_bool();
    return s;
}
inline auto to_json(const ServerSession& s) -> std::string { return ServerSession_to_json(s); }
inline auto from_json(std::string_view v, ServerSession*)
    -> std::expected<ServerSession, std::string> {
    return ServerSession_from_json(v);
}

// ─── DirectConnectRequest / Response ─────────────────────────────────────────

[[nodiscard]] inline auto DirectConnectRequest_to_json(const DirectConnectRequest& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("session_id", doc.string(r.session_id));
    o.add("auth_token", doc.string(r.auth_token));
    o.add("client_id", doc.string(r.client_id));
    if (r.desired_role.has_value()) {
        o.add("desired_role", doc.string(std::string{detail_serde::role_to_str(*r.desired_role)}));
    }
    o.add("ttl_seconds", doc.number(static_cast<int64_t>(r.ttl_seconds)));
    auto sc = doc.array();
    for (const auto& s : r.requested_scopes) sc.append(doc.string(s));
    o.add("requested_scopes", std::move(sc));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectConnectRequest_from_json(std::string_view raw)
    -> std::expected<DirectConnectRequest, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectConnectRequest r;
    r.session_id = std::string{o.get("session_id").as_str()};
    r.auth_token = std::string{o.get("auth_token").as_str()};
    r.client_id = std::string{o.get("client_id").as_str()};
    JsonVal dr = o.get("desired_role");
    if (dr.valid() && !dr.is_null()) {
        if (auto v = detail_serde::role_from_str(dr.as_str())) r.desired_role = *v;
    }
    r.ttl_seconds = o.get("ttl_seconds").as_int();
    if (r.ttl_seconds <= 0) r.ttl_seconds = 3600;
    if (!detail_serde::read_string_vec(o, "requested_scopes", r.requested_scopes)) {
        return std::unexpected<std::string>{"requested_scopes: expected array"};
    }
    return r;
}
inline auto to_json(const DirectConnectRequest& r) -> std::string { return DirectConnectRequest_to_json(r); }
inline auto from_json(std::string_view v, DirectConnectRequest*)
    -> std::expected<DirectConnectRequest, std::string> {
    return DirectConnectRequest_from_json(v);
}

[[nodiscard]] inline auto DirectConnectResponse_to_json(const DirectConnectResponse& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("ok", doc.boolean(r.ok));
    o.add("session_id", doc.string(r.session_id));
    o.add("token", doc.string(r.token));
    o.add("error", doc.string(r.error));
    o.add("assigned_role", doc.string(std::string{detail_serde::role_to_str(r.assigned_role)}));
    o.add("expires_ms", doc.number(static_cast<int64_t>(r.expires_ms)));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectConnectResponse_from_json(std::string_view raw)
    -> std::expected<DirectConnectResponse, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectConnectResponse r;
    r.ok = o.get("ok").as_bool();
    r.session_id = std::string{o.get("session_id").as_str()};
    r.token = std::string{o.get("token").as_str()};
    r.error = std::string{o.get("error").as_str()};
    if (auto v = detail_serde::role_from_str(o.get("assigned_role").as_str())) r.assigned_role = *v;
    r.expires_ms = o.get("expires_ms").as_int();
    return r;
}
inline auto to_json(const DirectConnectResponse& r) -> std::string { return DirectConnectResponse_to_json(r); }
inline auto from_json(std::string_view v, DirectConnectResponse*)
    -> std::expected<DirectConnectResponse, std::string> {
    return DirectConnectResponse_from_json(v);
}

// ─── DirectQueryRequest / Result ─────────────────────────────────────────────

[[nodiscard]] inline auto DirectQueryRequest_to_json(const DirectQueryRequest& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("session_id", doc.string(r.session_id));
    o.add("prompt", doc.string(r.prompt));
    o.add("model", doc.string(r.model));
    o.add("system_prompt_override", doc.string(r.system_prompt_override));
    o.add("conversation_id", doc.string(r.conversation_id));
    o.add("max_tokens", doc.number(static_cast<int64_t>(r.max_tokens)));
    o.add("temperature", doc.number(static_cast<double>(r.temperature)));
    o.add("stream", doc.boolean(r.stream));
    auto at = doc.array();
    for (const auto& s : r.allowed_tools) at.append(doc.string(s));
    o.add("allowed_tools", std::move(at));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectQueryRequest_from_json(std::string_view raw)
    -> std::expected<DirectQueryRequest, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectQueryRequest r;
    r.session_id = std::string{o.get("session_id").as_str()};
    r.prompt = std::string{o.get("prompt").as_str()};
    r.model = std::string{o.get("model").as_str()};
    r.system_prompt_override = std::string{o.get("system_prompt_override").as_str()};
    r.conversation_id = std::string{o.get("conversation_id").as_str()};
    r.max_tokens = static_cast<int>(o.get("max_tokens").as_int());
    r.temperature = o.get("temperature").as_double();
    r.stream = o.get("stream").as_bool();
    if (!detail_serde::read_string_vec(o, "allowed_tools", r.allowed_tools)) {
        return std::unexpected<std::string>{"allowed_tools: expected array"};
    }
    return r;
}
inline auto to_json(const DirectQueryRequest& r) -> std::string { return DirectQueryRequest_to_json(r); }
inline auto from_json(std::string_view v, DirectQueryRequest*)
    -> std::expected<DirectQueryRequest, std::string> {
    return DirectQueryRequest_from_json(v);
}

[[nodiscard]] inline auto DirectQueryResult_to_json(const DirectQueryResult& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("ok", doc.boolean(r.ok));
    o.add("conversation_id", doc.string(r.conversation_id));
    o.add("response_text", doc.string(r.response_text));
    o.add("model_used", doc.string(r.model_used));
    o.add("error", doc.string(r.error));
    o.add("prompt_tokens", doc.number(static_cast<int64_t>(static_cast<int64_t>(r.prompt_tokens))));
    o.add("completion_tokens", doc.number(static_cast<int64_t>(static_cast<int64_t>(r.completion_tokens))));
    o.add("cost_usd", doc.number(static_cast<double>(r.cost_usd)));
    o.add("status_code", doc.number(static_cast<int64_t>(r.status_code)));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectQueryResult_from_json(std::string_view raw)
    -> std::expected<DirectQueryResult, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectQueryResult r;
    r.ok = o.get("ok").as_bool();
    r.conversation_id = std::string{o.get("conversation_id").as_str()};
    r.response_text = std::string{o.get("response_text").as_str()};
    r.model_used = std::string{o.get("model_used").as_str()};
    r.error = std::string{o.get("error").as_str()};
    r.prompt_tokens = static_cast<uint64_t>(o.get("prompt_tokens").as_int());
    r.completion_tokens = static_cast<uint64_t>(o.get("completion_tokens").as_int());
    r.cost_usd = o.get("cost_usd").as_double();
    r.status_code = static_cast<int>(o.get("status_code").as_int());
    return r;
}
inline auto to_json(const DirectQueryResult& r) -> std::string { return DirectQueryResult_to_json(r); }
inline auto from_json(std::string_view v, DirectQueryResult*)
    -> std::expected<DirectQueryResult, std::string> {
    return DirectQueryResult_from_json(v);
}

// ─── DirectPermissionRequest ─────────────────────────────────────────────────

[[nodiscard]] inline auto DirectPermissionRequest_to_json(const DirectPermissionRequest& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("session_id", doc.string(r.session_id));
    o.add("tool_name", doc.string(r.tool_name));
    o.add("action", doc.string(r.action));
    auto ap = doc.array();
    for (const auto& s : r.affected_paths) ap.append(doc.string(s));
    o.add("affected_paths", std::move(ap));
    o.add("requested", doc.string(std::string{detail_serde::decision_to_str(r.requested)}));
    o.add("reason", doc.string(r.reason));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectPermissionRequest_from_json(std::string_view raw)
    -> std::expected<DirectPermissionRequest, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectPermissionRequest r;
    r.session_id = std::string{o.get("session_id").as_str()};
    r.tool_name = std::string{o.get("tool_name").as_str()};
    r.action = std::string{o.get("action").as_str()};
    if (!detail_serde::read_string_vec(o, "affected_paths", r.affected_paths)) {
        return std::unexpected<std::string>{"affected_paths: expected array"};
    }
    if (auto d = detail_serde::decision_from_str(o.get("requested").as_str())) r.requested = *d;
    r.reason = std::string{o.get("reason").as_str()};
    return r;
}
inline auto to_json(const DirectPermissionRequest& r) -> std::string { return DirectPermissionRequest_to_json(r); }
inline auto from_json(std::string_view v, DirectPermissionRequest*)
    -> std::expected<DirectPermissionRequest, std::string> {
    return DirectPermissionRequest_from_json(v);
}

// ─── DirectPermissionRule ────────────────────────────────────────────────────

[[nodiscard]] inline auto DirectPermissionRule_to_json(const DirectPermissionRule& r) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("id", doc.string(r.id));
    o.add("tool_pattern", doc.string(r.tool_pattern));
    o.add("path_pattern", doc.string(r.path_pattern));
    o.add("created_by", doc.string(r.created_by));
    o.add("decision", doc.string(std::string{detail_serde::decision_to_str(r.decision)}));
    o.add("created_ms", doc.number(static_cast<int64_t>(r.created_ms)));
    o.add("expires_ms", doc.number(static_cast<int64_t>(r.expires_ms)));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectPermissionRule_from_json(std::string_view raw)
    -> std::expected<DirectPermissionRule, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectPermissionRule r;
    r.id = std::string{o.get("id").as_str()};
    r.tool_pattern = std::string{o.get("tool_pattern").as_str()};
    r.path_pattern = std::string{o.get("path_pattern").as_str()};
    r.created_by = std::string{o.get("created_by").as_str()};
    if (auto d = detail_serde::decision_from_str(o.get("decision").as_str())) r.decision = *d;
    r.created_ms = o.get("created_ms").as_int();
    r.expires_ms = o.get("expires_ms").as_int();
    return r;
}
inline auto to_json(const DirectPermissionRule& r) -> std::string { return DirectPermissionRule_to_json(r); }
inline auto from_json(std::string_view v, DirectPermissionRule*)
    -> std::expected<DirectPermissionRule, std::string> {
    return DirectPermissionRule_from_json(v);
}

// ─── DirectPermissionDirectory ───────────────────────────────────────────────

[[nodiscard]] inline auto DirectPermissionDirectory_to_json(const DirectPermissionDirectory& d) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto arr = doc.array();
    for (const auto& r : d.rules) {
        auto parsed = parse(DirectPermissionRule_to_json(r));
        if (parsed) {
            arr.append(doc.copy_val(parsed->root()));
        }
    }
    auto obj = doc.object();
    obj.add("rules", std::move(arr));
    doc.set_root(std::move(obj));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectPermissionDirectory_from_json(std::string_view raw)
    -> std::expected<DirectPermissionDirectory, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectPermissionDirectory d;
    JsonVal arr = o.get("rules");
    if (!arr.valid() || arr.is_null()) return d;
    if (!arr.is_arr()) return std::unexpected<std::string>{"rules: expected array"};
    bool ok = true;
    std::string err;
    arr.iter([&](JsonVal el) {
        if (!ok) return;
        JsonMutDoc tmp;
        tmp.set_root(tmp.copy_val(el));
        auto rule = DirectPermissionRule_from_json(tmp.to_string());
        if (!rule) { ok = false; err = rule.error(); return; }
        d.rules.push_back(std::move(*rule));
    });
    if (!ok) return std::unexpected(err);
    return d;
}
inline auto to_json(const DirectPermissionDirectory& d) -> std::string { return DirectPermissionDirectory_to_json(d); }
inline auto from_json(std::string_view v, DirectPermissionDirectory*)
    -> std::expected<DirectPermissionDirectory, std::string> {
    return DirectPermissionDirectory_from_json(v);
}

// ─── DirectPermissionSessionState ────────────────────────────────────────────

[[nodiscard]] inline auto DirectPermissionSessionState_to_json(const DirectPermissionSessionState& s) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("session_id", doc.string(s.session_id));
    auto perm_parsed = parse(DirectPermissionDirectory_to_json(s.permanent_rules));
    if (perm_parsed) o.add("permanent_rules", doc.copy_val(perm_parsed->root()));
    auto sess_arr = doc.array();
    for (const auto& r : s.session_only_rules) {
        auto parsed = parse(DirectPermissionRule_to_json(r));
        if (parsed) sess_arr.append(doc.copy_val(parsed->root()));
    }
    o.add("session_only_rules", std::move(sess_arr));
    auto den = doc.array();
    for (const auto& x : s.recently_denied) den.append(doc.string(x));
    o.add("recently_denied", std::move(den));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectPermissionSessionState_from_json(std::string_view raw)
    -> std::expected<DirectPermissionSessionState, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectPermissionSessionState s;
    s.session_id = std::string{o.get("session_id").as_str()};
    JsonVal pr = o.get("permanent_rules");
    if (pr.valid() && !pr.is_null()) {
        JsonMutDoc tmp;
        tmp.set_root(tmp.copy_val(pr));
        auto d = DirectPermissionDirectory_from_json(tmp.to_string());
        if (!d) return std::unexpected(d.error());
        s.permanent_rules = std::move(*d);
    }
    JsonVal sess = o.get("session_only_rules");
    if (sess.valid() && sess.is_arr()) {
        bool ok = true;
        std::string err;
        sess.iter([&](JsonVal el) {
            if (!ok) return;
            JsonMutDoc tmp;
            tmp.set_root(tmp.copy_val(el));
            auto r = DirectPermissionRule_from_json(tmp.to_string());
            if (!r) { ok = false; err = r.error(); return; }
            s.session_only_rules.push_back(std::move(*r));
        });
        if (!ok) return std::unexpected(err);
    }
    if (!detail_serde::read_string_vec(o, "recently_denied", s.recently_denied)) {
        return std::unexpected<std::string>{"recently_denied: expected array"};
    }
    return s;
}
inline auto to_json(const DirectPermissionSessionState& s) -> std::string { return DirectPermissionSessionState_to_json(s); }
inline auto from_json(std::string_view v, DirectPermissionSessionState*)
    -> std::expected<DirectPermissionSessionState, std::string> {
    return DirectPermissionSessionState_from_json(v);
}

// ─── DirectQueryStreamChunk ──────────────────────────────────────────────────

[[nodiscard]] inline auto DirectQueryStreamChunk_to_json(const DirectQueryStreamChunk& c) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto o = doc.object();
    o.add("conversation_id", doc.string(c.conversation_id));
    o.add("kind", doc.string(std::string{detail_serde::chunk_kind_to_str(c.kind)}));
    o.add("delta", doc.string(c.delta));
    o.add("stop_reason", doc.string(c.stop_reason));
    o.add("tool_use_id", doc.string(c.tool_use_id));
    o.add("tool_name", doc.string(c.tool_name));
    o.add("tool_input_json", doc.string(c.tool_input_json));
    o.add("tool_output_json", doc.string(c.tool_output_json));
    o.add("error_message", doc.string(c.error_message));
    doc.set_root(std::move(o));
    return doc.to_string();
}
[[nodiscard]] inline auto DirectQueryStreamChunk_from_json(std::string_view raw)
    -> std::expected<DirectQueryStreamChunk, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal o = parsed->root();
    if (!o.is_obj()) return std::unexpected<std::string>{"expected object"};
    DirectQueryStreamChunk c;
    c.conversation_id = std::string{o.get("conversation_id").as_str()};
    if (auto k = detail_serde::chunk_kind_from_str(o.get("kind").as_str())) c.kind = *k;
    c.delta = std::string{o.get("delta").as_str()};
    c.stop_reason = std::string{o.get("stop_reason").as_str()};
    c.tool_use_id = std::string{o.get("tool_use_id").as_str()};
    c.tool_name = std::string{o.get("tool_name").as_str()};
    c.tool_input_json = std::string{o.get("tool_input_json").as_str()};
    c.tool_output_json = std::string{o.get("tool_output_json").as_str()};
    c.error_message = std::string{o.get("error_message").as_str()};
    return c;
}
inline auto to_json(const DirectQueryStreamChunk& c) -> std::string { return DirectQueryStreamChunk_to_json(c); }
inline auto from_json(std::string_view v, DirectQueryStreamChunk*)
    -> std::expected<DirectQueryStreamChunk, std::string> {
    return DirectQueryStreamChunk_from_json(v);
}

}  // namespace cc::server
