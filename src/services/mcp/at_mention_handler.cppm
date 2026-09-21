// cc.services.mcp.at_mention_handler — inbound "at_mentioned" notification
// dispatch. Faithful counterpart of TS useIdeAtMentioned.ts, which registers
// a notification handler on the IDE MCP client. In C++ the single inbound
// dispatch path lives in McpConnectionManager::handle_server_notification
// (services/mcp/connection_manager.cppm); when it sees method == "at_mentioned"
// it calls dispatch_at_mention() here, which forwards to whichever UI
// responder has been registered. The responder is set by AppAdapter
// (ui/app.cppm) and stages an "@<relpath>#L<a>-<b>" token into
// ReplScreenState::pending_at_mention_inserts.
//
// Line numbers arrive from the IDE 0-based (per TS useIdeAtMentioned.ts which
// adds +1). We normalise to 1-based inside dispatch_at_mention so the
// responder always sees 1-based values, matching TS onAtMentioned output.
module;
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.mcp.at_mention_handler;

import cc.utils.json;

export namespace cc::services::mcp {

// Parsed at_mentioned payload (line numbers are 1-based after normalisation,
// matching TS IDEAtMentioned.lineStart/lineEnd).
struct AtMentionNotification {
    std::string server_name;
    std::string file_path;
    std::optional<int> line_start;
    std::optional<int> line_end;
};

// Responder invoked on the MCP receive thread for each at_mentioned
// notification. Implementations must be thread-safe w.r.t. UI state; the
// canonical implementation (AppAdapter) only stages into a mutex-guarded
// vector and triggers a re-render.
using AtMentionResponder = std::function<void(const AtMentionNotification&)>;

namespace detail {
inline std::mutex responder_mutex;
inline AtMentionResponder responder;

// Extract a string field from a JSON object string via the json utils.
// Returns std::nullopt if the field is absent or not a string.
inline auto get_string_field(const std::string& params_json,
                             std::string_view key) -> std::optional<std::string> {
    if (params_json.empty()) return std::nullopt;
    auto parsed = cc::utils::json::parse(params_json);
    if (!parsed) return std::nullopt;
    auto root = parsed->root();
    if (!root.is_obj()) return std::nullopt;
    auto node = root.get(std::string(key));
    if (!node.is_str()) return std::nullopt;
    return std::string(node.as_str());
}

// Extract an integer field; std::nullopt if absent / non-number.
inline auto get_int_field(const std::string& params_json,
                          std::string_view key) -> std::optional<int> {
    if (params_json.empty()) return std::nullopt;
    auto parsed = cc::utils::json::parse(params_json);
    if (!parsed) return std::nullopt;
    auto root = parsed->root();
    if (!root.is_obj()) return std::nullopt;
    auto node = root.get(std::string(key));
    if (node.is_int()) return static_cast<int>(node.as_int());
    return std::nullopt;
}
} // namespace detail

// Register the UI-side responder. Pass nullptr to clear.
auto set_at_mention_responder(AtMentionResponder responder) -> void {
    std::lock_guard lock(detail::responder_mutex);
    detail::responder = std::move(responder);
}

// Called by McpConnectionManager::handle_server_notification when method ==
// "at_mentioned". server_name is the MCP server that sent the notification
// (the IDE client on TS); params_json is the raw JSON-RPC params object.
auto dispatch_at_mention(const std::string& server_name,
                         const std::optional<std::string>& params_json) -> void {
    AtMentionResponder active;
    {
        std::lock_guard lock(detail::responder_mutex);
        active = detail::responder;
    }
    if (!active) return;

    AtMentionNotification n;
    n.server_name = server_name;
    const std::string params = params_json.value_or(std::string{});

    if (auto fp = detail::get_string_field(params, "filePath")) {
        n.file_path = std::move(*fp);
    } else {
        // Without a filePath the at-mention is malformed; drop it (TS parity:
        // the schema is zod-validated and would reject this).
        return;
    }

    if (auto ls = detail::get_int_field(params, "lineStart")) {
        // IDE sends 0-based; normalise to 1-based (TS adds +1).
        n.line_start = *ls + 1;
    }
    if (auto le = detail::get_int_field(params, "lineEnd")) {
        n.line_end = *le + 1;
    }

    try {
        active(n);
    } catch (...) {
        // Swallow responder errors so a buggy UI hook cannot take down the
        // MCP receive thread. TS wraps the handler in try/catch + logError.
    }
}

} // namespace cc::services::mcp
