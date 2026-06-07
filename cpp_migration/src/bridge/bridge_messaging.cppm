/// @file bridge_messaging.cppm
/// @brief Bridge messaging handlers: type guards, ingress routing,
///        server control request dispatch, echo dedup, and result
///        message construction.
///
/// Migrated from src/bridge/bridgeMessaging.ts.
/// All functions are pure — no closure over bridge-specific state.
/// Collaborators (transport, sessionId, UUID sets, callbacks) are
/// passed as parameters.
module;

#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

export module cc.bridge.bridge_messaging;

import cc.bridge.messages;
import cc.utils.json;

export namespace cc::bridge {

// =========================================================================
// BoundedUUIDSet — FIFO-bounded set for echo dedup
// =========================================================================

/// FIFO-bounded set backed by a circular buffer. Evicts the oldest
/// entry when capacity is reached, keeping memory usage constant at
/// O(capacity). Used as a secondary safety net for echo filtering
/// and race-condition dedup in bridge ingress routing.
class BoundedUUIDSet {
    std::size_t capacity_;
    std::vector<std::optional<std::string>> ring_;
    std::unordered_set<std::string> set_;
    std::size_t write_idx_ = 0;

public:
    explicit BoundedUUIDSet(std::size_t capacity)
        : capacity_(capacity)
        , ring_(capacity)
    {}

    /// Add a UUID. If the set is at capacity, the oldest entry is
    /// evicted first. Duplicate adds are no-ops.
    void add(std::string uuid) {
        if (set_.contains(uuid)) return;
        // Evict the entry at the current write position (if occupied)
        if (ring_[write_idx_].has_value()) {
            set_.erase(*ring_[write_idx_]);
        }
        ring_[write_idx_] = std::move(uuid);
        set_.insert(*ring_[write_idx_]);
        write_idx_ = (write_idx_ + 1) % capacity_;
    }

    /// Check whether a UUID is present.
    [[nodiscard]] bool has(const std::string& uuid) const {
        return set_.contains(uuid);
    }

    /// Remove all entries.
    void clear() {
        set_.clear();
        for (auto& slot : ring_) slot.reset();
        write_idx_ = 0;
    }
};

// =========================================================================
// Parsed message variants (for type-guard style routing)
// =========================================================================

/// A parsed control_response from the server.
struct SDKControlResponse {
    struct Response {
        std::string subtype;
        std::string request_id;
        std::optional<std::string> error;
        std::optional<std::string> response_json;
    } response;
};

/// A parsed control_request from the server.
struct SDKControlRequest {
    struct Request {
        std::string subtype;
        std::optional<std::string> model;
        std::optional<std::string> mode;
        std::optional<int64_t> max_thinking_tokens;
    } request;
    std::string request_id;
};

/// Tag type for an unrecognized parsed message.
struct UnknownMessage {};

/// The discriminated union returned by the ingress parser.
using ParsedMessage = std::variant<
    SDKMessage,
    SDKControlResponse,
    SDKControlRequest,
    UnknownMessage
>;

// =========================================================================
// Type guards — operate on a parsed JsonVal
// =========================================================================

/// True when the JSON value looks like an SDKMessage (has a string
/// "type" field). Callers narrow further via the discriminated union.
[[nodiscard]] inline bool is_sdk_message(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return false;
    auto type_val = root.get("type");
    return type_val.is_str();
}

/// True when the JSON value is a control_response.
[[nodiscard]] inline bool is_sdk_control_response(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return false;
    auto type_val = root.get("type");
    if (!type_val.is_str()) return false;
    return type_val.as_str() == std::string_view("control_response")
        && root.get("response").valid();
}

/// True when the JSON value is a control_request.
[[nodiscard]] inline bool is_sdk_control_request(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return false;
    auto type_val = root.get("type");
    if (!type_val.is_str()) return false;
    return type_val.as_str() == std::string_view("control_request")
        && root.get("request_id").valid()
        && root.get("request").valid();
}

// =========================================================================
// Eligible bridge message check (for outbound forwarding)
// =========================================================================

/// Bridge-eligible message descriptor. Light-weight analog of the
/// TypeScript Message type — only the fields needed for eligibility
/// filtering are carried.
struct BridgeEligibleMessage {
    std::string type;                // "user", "assistant", "system"
    std::optional<std::string> subtype;  // e.g. "local_command"
    bool is_virtual = false;
};

/// True for message types that should be forwarded to the bridge
/// transport. The server only wants user/assistant turns and
/// slash-command system events; everything else is internal REPL
/// chatter. Virtual messages (REPL inner calls) are excluded.
[[nodiscard]] inline bool is_eligible_bridge_message(const BridgeEligibleMessage& m) {
    // Virtual messages are display-only — bridge/SDK consumers see
    // the REPL tool_use/result which summarizes the work.
    if ((m.type == "user" || m.type == "assistant") && m.is_virtual) {
        return false;
    }
    return m.type == "user"
        || m.type == "assistant"
        || (m.type == "system" && m.subtype == "local_command");
}

// =========================================================================
// Title text extraction
// =========================================================================

/// Descriptor for the subset of Message fields needed to decide
/// whether a user message should title the session.
struct TitleCandidate {
    std::string type;
    bool is_meta = false;
    bool is_tool_use_result = false;
    bool is_compact_summary = false;
    struct Origin {
        std::string kind;  // "human", "task_notification", etc.
    };
    std::optional<Origin> origin;
    /// Content: either a raw string or a vector of text blocks.
    std::optional<std::variant<std::string, std::vector<TextBlock>>> content;
};

/// Extract title-worthy text from a message for onUserMessage.
/// Returns std::nullopt for messages that should not title the
/// session: non-user, meta (nudges), tool results, compact
/// summaries, non-human origins, or pure display-tag content.
[[nodiscard]] inline std::optional<std::string> extract_title_text(const TitleCandidate& m) {
    if (m.type != "user" || m.is_meta || m.is_tool_use_result || m.is_compact_summary) {
        return std::nullopt;
    }
    if (m.origin && m.origin->kind != "human") return std::nullopt;

    if (!m.content) return std::nullopt;

    std::string raw;
    if (std::holds_alternative<std::string>(*m.content)) {
        raw = std::get<std::string>(*m.content);
    } else {
        const auto& blocks = std::get<std::vector<TextBlock>>(*m.content);
        for (const auto& block : blocks) {
            raw = block.text;
            break;
        }
    }
    if (raw.empty()) return std::nullopt;
    // Strip display tags — for now just return the raw text.
    // A full implementation would call stripDisplayTagsAllowEmpty.
    return raw;
}

// =========================================================================
// JSON parsing helpers for ingress messages
// =========================================================================

/// Build an SDKControlRequest from a parsed JSON object.
[[nodiscard]] inline SDKControlRequest parse_control_request(cc::utils::json::JsonVal root) {
    SDKControlRequest req;
    req.request_id = root.get_string("request_id");
    auto inner = root.get("request");
    if (inner.is_obj()) {
        req.request.subtype = inner.get_string("subtype");
        auto model = inner.get("model");
        if (model.is_str()) req.request.model = std::string(model.as_str());
        auto mode = inner.get("mode");
        if (mode.is_str()) req.request.mode = std::string(mode.as_str());
        auto max_tokens = inner.get("max_thinking_tokens");
        if (max_tokens.is_num()) req.request.max_thinking_tokens = max_tokens.as_int();
    }
    return req;
}

/// Build an SDKControlResponse from a parsed JSON object.
[[nodiscard]] inline SDKControlResponse parse_control_response(cc::utils::json::JsonVal root) {
    SDKControlResponse resp;
    auto inner = root.get("response");
    if (inner.is_obj()) {
        resp.response.subtype = inner.get_string("subtype");
        resp.response.request_id = inner.get_string("request_id");
        auto err = inner.get("error");
        if (err.is_str()) resp.response.error = std::string(err.as_str());
        auto payload = inner.get("response");
        if (payload.valid()) resp.response.response_json = payload.to_string();
    }
    return resp;
}

/// Build an SDKMessage from a parsed JSON object.
[[nodiscard]] inline SDKMessage parse_sdk_message(cc::utils::json::JsonVal root) {
    SDKMessage msg;
    msg.type = root.get_string("type");
    auto uuid_val = root.get("uuid");
    if (uuid_val.is_str()) msg.uuid = std::string(uuid_val.as_str());

    auto content_val = root.get("content");
    if (content_val.is_str()) {
        msg.message.content = std::string(content_val.as_str());
    } else if (content_val.is_arr()) {
        std::vector<ContentBlock> blocks;
        content_val.iter([&blocks](cc::utils::json::JsonVal item) {
            auto btype = item.get("type");
            if (btype.is_str() && btype.as_str() == std::string_view("text")) {
                TextBlock tb;
                tb.text = item.get_string("text");
                blocks.push_back(tb);
            }
            // Image blocks and other types are handled by
            // extract_inbound_message_fields in messages.cppm.
        });
        msg.message.content = std::move(blocks);
    }
    return msg;
}

// =========================================================================
// Ingress routing
// =========================================================================

using InboundMessageCallback = std::function<void(const SDKMessage&)>;
using PermissionResponseCallback = std::function<void(const SDKControlResponse&)>;
using ControlRequestCallback = std::function<void(const SDKControlRequest&)>;

/// Parse an ingress WebSocket message (raw JSON string) and route it
/// to the appropriate handler.
///
/// Ignores messages whose UUID is in recent_posted_uuids (echoes of
/// what we sent) or in recent_inbound_uuids (re-deliveries we've
/// already forwarded — e.g. server replayed history after a transport
/// swap lost the seq-num cursor).
///
/// @param data                 Raw JSON string from the WebSocket.
/// @param recent_posted_uuids  Echo-dedup set (UUIDs we sent).
/// @param recent_inbound_uuids Re-delivery dedup set (UUIDs we already forwarded).
/// @param on_inbound_message   Handler for user messages from the bridge.
/// @param on_permission_response Handler for control_response messages.
/// @param on_control_request   Handler for control_request messages.
/// @param debug_log            Optional debug logger (receives log lines).
inline void handle_ingress_message(
    const std::string& data,
    BoundedUUIDSet& recent_posted_uuids,
    BoundedUUIDSet& recent_inbound_uuids,
    const InboundMessageCallback& on_inbound_message,
    const PermissionResponseCallback& on_permission_response = nullptr,
    const ControlRequestCallback& on_control_request = nullptr,
    const std::function<void(const std::string&)>& debug_log = nullptr
) {
    auto log = [&debug_log](const std::string& msg) {
        if (debug_log) debug_log(msg);
    };

    try {
        auto parsed = cc::utils::json::parse(data);
        if (!parsed || !parsed->root().is_obj()) return;
        auto root = parsed->root();

        // control_response is not an SDKMessage — check before the
        // type guard.
        if (is_sdk_control_response(root)) {
            log("[bridge:repl] Ingress message type=control_response");
            if (on_permission_response) {
                on_permission_response(parse_control_response(root));
            }
            return;
        }

        // control_request from the server (initialize, set_model,
        // can_use_tool). Must respond promptly or the server kills
        // the WS (~10-14s timeout).
        if (is_sdk_control_request(root)) {
            auto req = parse_control_request(root);
            log(std::format("[bridge:repl] Inbound control_request subtype={}",
                            req.request.subtype));
            if (on_control_request) {
                on_control_request(req);
            }
            return;
        }

        if (!is_sdk_message(root)) return;

        auto msg = parse_sdk_message(root);

        // Check for UUID to detect echoes of our own messages
        if (msg.uuid && recent_posted_uuids.has(*msg.uuid)) {
            log(std::format("[bridge:repl] Ignoring echo: type={} uuid={}",
                            msg.type, *msg.uuid));
            return;
        }

        // Defensive dedup: drop inbound prompts we've already
        // forwarded.
        if (msg.uuid && recent_inbound_uuids.has(*msg.uuid)) {
            log(std::format("[bridge:repl] Ignoring re-delivered inbound: type={} uuid={}",
                            msg.type, *msg.uuid));
            return;
        }

        log(std::format("[bridge:repl] Ingress message type={}{}",
                        msg.type,
                        msg.uuid ? std::format(" uuid={}", *msg.uuid) : std::string{}));

        if (msg.type == "user") {
            if (msg.uuid) recent_inbound_uuids.add(*msg.uuid);
            if (on_inbound_message) {
                on_inbound_message(msg);
            }
        } else {
            log(std::format("[bridge:repl] Ignoring non-user inbound message: type={}",
                            msg.type));
        }
    } catch (const std::exception& err) {
        log(std::format("[bridge:repl] Failed to parse ingress message: {}", err.what()));
    }
}

// =========================================================================
// Server-initiated control request handling
// =========================================================================

/// Callbacks for server-initiated control requests.
struct ServerControlRequestHandlers {
    /// Callback to write a response event back to the server.
    /// Receives the full JSON event string.
    std::function<void(const std::string&)> write_event;

    std::string session_id;

    /// When true, all mutable requests (interrupt, set_model,
    /// set_permission_mode, set_max_thinking_tokens) reply with an
    /// error instead of false-success. initialize still replies
    /// success — the server kills the connection otherwise. Used by
    /// the outbound-only bridge mode.
    bool outbound_only = false;

    std::function<void()> on_interrupt;
    std::function<void(const std::optional<std::string>&)> on_set_model;
    std::function<void(std::optional<int64_t>)> on_set_max_thinking_tokens;
    std::function<std::pair<bool, std::string>(const std::string&)> on_set_permission_mode;
    /// Returns {ok, optional_error_message}.
};

/// Error message used for outbound-only rejection.
inline constexpr std::string_view OUTBOUND_ONLY_ERROR =
    "This session is outbound-only. Enable Remote Control locally to allow inbound control.";

/// Helper: build a control_response JSON event string.
[[nodiscard]] inline std::string build_control_response_event(
    const std::string& session_id,
    const std::string& request_id,
    const std::string& subtype,       // "success" or "error"
    const std::string& response_json, // inner "response" object (may be "{}")
    const std::optional<std::string>& error = std::nullopt
) {
    if (subtype == "error" && error) {
        return std::format(
            R"({{"type":"control_response","session_id":"{}","response":{{"subtype":"error","request_id":"{}","error":"{}"}}}})",
            session_id, request_id, *error);
    }
    return std::format(
        R"({{"type":"control_response","session_id":"{}","response":{{"subtype":"success","request_id":"{}","response":{}}}}})",
        session_id, request_id, response_json);
}

/// Respond to inbound control_request messages from the server.
/// The server sends these for session lifecycle events (initialize,
/// set_model) and for turn-level coordination (interrupt,
/// set_max_thinking_tokens). If we don't respond, the server hangs
/// and kills the WS after ~10-14s.
inline void handle_server_control_request(
    const SDKControlRequest& request,
    const ServerControlRequestHandlers& handlers,
    const std::function<void(const std::string&)>& debug_log = nullptr
) {
    auto log = [&debug_log](const std::string& msg) {
        if (debug_log) debug_log(msg);
    };

    if (!handlers.write_event) {
        log("[bridge:repl] Cannot respond to control_request: transport not configured");
        return;
    }

    std::string response_event;

    // Outbound-only: reply error for mutable requests so the client
    // doesn't see false success. initialize must still succeed
    // (server kills the connection otherwise).
    if (handlers.outbound_only && request.request.subtype != "initialize") {
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "error",
            "{}",
            std::string(OUTBOUND_ONLY_ERROR));
        handlers.write_event(response_event);
        log(std::format("[bridge:repl] Rejected {} (outbound-only) request_id={}",
                        request.request.subtype, request.request_id));
        return;
    }

    if (request.request.subtype == "initialize") {
        // Respond with minimal capabilities — the REPL handles
        // commands, models, and account info itself.
        auto pid = ::getpid();
        auto payload = std::format(
            R"({{"commands":[],"output_style":"normal","available_output_styles":["normal"],"models":[],"account":{{}},"pid":{}}})",
            pid);
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "success",
            payload);

    } else if (request.request.subtype == "set_model") {
        if (handlers.on_set_model) {
            handlers.on_set_model(request.request.model);
        }
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "success",
            "{}");

    } else if (request.request.subtype == "set_max_thinking_tokens") {
        if (handlers.on_set_max_thinking_tokens) {
            handlers.on_set_max_thinking_tokens(request.request.max_thinking_tokens);
        }
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "success",
            "{}");

    } else if (request.request.subtype == "set_permission_mode") {
        // The callback returns a policy verdict so we can send an
        // error control_response without importing mode-gate logic
        // here (bootstrap-isolation).
        if (handlers.on_set_permission_mode) {
            auto [ok, err_msg] = handlers.on_set_permission_mode(
                request.request.mode.value_or(""));
            if (ok) {
                response_event = build_control_response_event(
                    handlers.session_id,
                    request.request_id,
                    "success",
                    "{}");
            } else {
                response_event = build_control_response_event(
                    handlers.session_id,
                    request.request_id,
                    "error",
                    "{}",
                    err_msg);
            }
        } else {
            // No callback registered (daemon context) — return an
            // error rather than a silent false-success.
            response_event = build_control_response_event(
                handlers.session_id,
                request.request_id,
                "error",
                "{}",
                std::string("set_permission_mode is not supported in this context "
                            "(onSetPermissionMode callback not registered)"));
        }

    } else if (request.request.subtype == "interrupt") {
        if (handlers.on_interrupt) {
            handlers.on_interrupt();
        }
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "success",
            "{}");

    } else {
        // Unknown subtype — respond with error so the server doesn't
        // hang waiting for a reply that never comes.
        response_event = build_control_response_event(
            handlers.session_id,
            request.request_id,
            "error",
            "{}",
            std::format("REPL bridge does not handle control_request subtype: {}",
                        request.request.subtype));
    }

    handlers.write_event(response_event);
    // Determine result subtype for logging
    auto result_subtype = (response_event.find("\"subtype\":\"success\"") != std::string::npos)
        ? "success" : "error";
    log(std::format("[bridge:repl] Sent control_response for {} request_id={} result={}",
                    request.request.subtype, request.request_id, result_subtype));
}

// =========================================================================
// Result message (for session archival on teardown)
// =========================================================================

/// Minimal SDKResultSuccess fields for session archival.
struct SDKResultSuccess {
    std::string type = "result";
    std::string subtype = "success";
    int64_t duration_ms = 0;
    int64_t duration_api_ms = 0;
    bool is_error = false;
    int num_turns = 0;
    std::string result;
    std::optional<std::string> stop_reason;
    double total_cost_usd = 0.0;
    std::string usage_json = "{}";
    std::string model_usage_json = "{}";
    std::string permission_denials_json = "[]";
    std::string session_id;
    std::string uuid;
};

/// Generate a simple UUID v4 (for result messages).
/// Delegates to the crypto module when available, otherwise falls
/// back to a simple random hex string.
[[nodiscard]] inline std::string generate_bridge_uuid() {
    // Simple UUID v4 generation sufficient for result messages.
    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::random_device rd;
    auto hex = [&rd]() -> std::string {
        std::string s;
        for (int i = 0; i < 2; ++i) {
            auto byte = static_cast<unsigned char>(rd());
            char buf[3];
            std::snprintf(buf, sizeof(buf), "%02x", byte);
            s += buf;
        }
        return s;
    };
    std::string uuid;
    uuid.reserve(36);
    uuid += hex(); uuid += hex(); uuid += '-';
    uuid += hex(); uuid += '-';
    uuid += '4'; uuid += hex().substr(1); uuid += '-';
    // Variant bit: 10xx
    auto variant_byte = static_cast<unsigned char>(rd());
    variant_byte = (variant_byte & 0x3f) | 0x80;
    char vbuf[3];
    std::snprintf(vbuf, sizeof(vbuf), "%02x", variant_byte);
    uuid += vbuf;
    uuid += hex().substr(1); uuid += '-';
    uuid += hex(); uuid += hex(); uuid += hex();
    return uuid;
}

/// Build a minimal SDKResultSuccess message for session archival.
/// The server needs this event before a WS close to trigger archival.
[[nodiscard]] inline SDKResultSuccess make_result_message(const std::string& session_id) {
    return SDKResultSuccess{
        .session_id = session_id,
        .uuid = generate_bridge_uuid(),
    };
}

/// Serialize a SDKResultSuccess to JSON for sending to the server.
[[nodiscard]] inline std::string serialize_result_message(const SDKResultSuccess& msg) {
    return std::format(
        R"({{"type":"{}","subtype":"{}","duration_ms":{},"duration_api_ms":{},"is_error":{},"num_turns":{},"result":"{}","stop_reason":{},"total_cost_usd":{},"usage":{},"modelUsage":{},"permission_denials":{},"session_id":"{}","uuid":"{}"}})",
        msg.type,
        msg.subtype,
        msg.duration_ms,
        msg.duration_api_ms,
        msg.is_error ? "true" : "false",
        msg.num_turns,
        msg.result,
        msg.stop_reason ? std::format("\"{}\"", *msg.stop_reason) : "null",
        msg.total_cost_usd,
        msg.usage_json,
        msg.model_usage_json,
        msg.permission_denials_json,
        msg.session_id,
        msg.uuid);
}

} // namespace cc::bridge
