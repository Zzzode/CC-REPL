/// @file remote_permission_bridge.cppm
/// @brief Remote permission bridging for synthetic assistant messages.
/// Creates synthetic AssistantMessage objects for remote permission requests
/// so that the REPL's permission UI can handle them identically to local
/// tool permissions. The bridge tracks pending requests, routes local UI
/// decisions back to the remote side, and supports cancellation.

module;

#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <format>
#include <atomic>
#include <random>
#include <cstddef>

export module cc.remote.remote_permission_bridge;

import cc.bridge.messages;

export namespace cc::remote {

// ============================================================
// Remote permission request / response types
// ============================================================

/// A permission request arriving from a remote CCR container.
/// Contains everything the local REPL needs to display a permission prompt.
struct RemotePermissionRequest {
    std::string request_id;         // Unique identifier for this request
    std::string tool_use_id;        // ID of the tool_use content block
    std::string tool_name;          // e.g. "bash", "file_write"
    std::string input_json;         // Serialized tool input (JSON)
    std::string permission_type;    // Category: "bash", "file_write", "mcp", etc.
};

/// The local user's decision for a remote permission request.
struct RemotePermissionResponse {
    std::string request_id;                      // Which request this answers
    std::string behavior;                        // "allow" or "deny"
    std::optional<std::string> updated_input_json;  // Modified input if user edited
};

// ============================================================
// Pending request entry (internal tracking)
// ============================================================

namespace detail {

/// Tracked state for a single pending remote permission request.
struct PendingEntry {
    RemotePermissionRequest request;
    std::chrono::steady_clock::time_point submitted_at;
};

/// Generate a UUID v4-like string for synthetic message identifiers.
inline auto generate_uuid() -> std::string {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;

    auto hi = dist(rng);
    auto lo = dist(rng);

    // Set version (4) and variant bits per RFC 4122
    hi = (hi & 0xFFFF'FFFF'FFFF'0FFFULL) | 0x0000'0000'0000'4000ULL;
    lo = (lo & 0x3FFF'FFFF'FFFF'FFFFULL) | 0x8000'0000'0000'0000ULL;

    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint16_t>(hi >> 16),
        static_cast<uint16_t>(hi),
        static_cast<uint16_t>(lo >> 48),
        static_cast<uint64_t>(lo & 0xFFFF'FFFF'FFFFULL));
}

/// Build a JSON tool_use content block string for embedding in a synthetic message.
inline auto build_tool_use_block(const RemotePermissionRequest& req) -> std::string {
    return std::format(
        R"({{"type":"tool_use","id":"{}","name":"{}","input":{}}})",
        req.tool_use_id, req.tool_name,
        req.input_json.empty() ? "{}" : req.input_json);
}

/// Build the inner message JSON (role, content, model, usage).
inline auto build_inner_message_json(
    const std::string& message_id,
    const std::string& tool_use_block) -> std::string {
    return std::format(
        R"({{"id":"{}","type":"message","role":"assistant","content":[{}],)"
        R"("model":"","stop_reason":null,"stop_sequence":null,)"
        R"("container":null,"context_management":null,)"
        R"("usage":{{"input_tokens":0,"output_tokens":0,)"
        R"("cache_creation_input_tokens":0,"cache_read_input_tokens":0}}}})",
        message_id, tool_use_block);
}

/// Build the outer SDKMessage JSON with the is_remote_permission flag.
inline auto build_synthetic_message_json(
    const std::string& uuid,
    const std::string& inner_message) -> std::string {
    return std::format(
        R"({{"type":"assistant","uuid":"{}","message":{},"is_remote_permission":true}})",
        uuid, inner_message);
}

} // namespace detail

// ============================================================
// RemotePermissionBridge
// ============================================================

/// Callback type for sending a permission response back to the remote side.
using PermissionResponseCallback = std::function<void(const RemotePermissionResponse&)>;

/// Callback type for notifying when all pending requests have been cancelled.
using CancelCallback = std::function<void()>;

/// Bridges remote permission requests into the local REPL permission UI.
///
/// Remote tool executions that require user approval arrive here as
/// RemotePermissionRequest objects. The bridge wraps each one in a
/// synthetic SDKMessage that mimics an assistant message with a tool_use
/// block. The local REPL processes this identically to a local permission
/// prompt. When the user responds, the bridge forwards the decision back
/// to the remote container via the response callback.
class RemotePermissionBridge {
public:
    /// Construct a bridge with the given response callback.
    /// @param on_respond  Called when the local UI resolves a permission request.
    /// @param on_cancel   Called when cancel_all() is invoked (optional).
    explicit RemotePermissionBridge(
        PermissionResponseCallback on_respond,
        CancelCallback on_cancel = nullptr)
        : respond_callback_(std::move(on_respond))
        , cancel_callback_(std::move(on_cancel))
    {}

    /// Non-copyable.
    RemotePermissionBridge(const RemotePermissionBridge&) = delete;
    RemotePermissionBridge& operator=(const RemotePermissionBridge&) = delete;

    /// Create a synthetic SDKMessage from a remote permission request.
    ///
    /// The returned message has:
    ///   - type = "assistant"
    ///   - uuid = randomly generated
    ///   - message.id = "remote-<request_id>"
    ///   - message.content = [tool_use block with the tool name and input]
    ///   - is_remote_permission = true
    ///
    /// The request is tracked internally as pending until the local UI
    /// resolves it via handle_permission_result() or cancel_all().
    auto create_synthetic_assistant_message(const RemotePermissionRequest& request)
        -> cc::bridge::SDKMessage
    {
        auto uuid = detail::generate_uuid();
        auto message_id = std::format("remote-{}", request.request_id);

        // Build the synthetic message content
        auto tool_use_block = detail::build_tool_use_block(request);
        auto inner_message = detail::build_inner_message_json(message_id, tool_use_block);

        // Construct the SDKMessage struct
        cc::bridge::SDKMessage msg;
        msg.type = "assistant";
        msg.uuid = uuid;
        msg.message.content = std::vector<cc::bridge::ContentBlock>{};

        // Store the full JSON for the synthetic content as a string variant
        // so downstream consumers can parse it.
        msg.message.content = inner_message;

        // Track as pending
        {
            std::lock_guard lock(mutex_);
            pending_.emplace(request.request_id, detail::PendingEntry{
                .request = request,
                .submitted_at = std::chrono::steady_clock::now(),
            });
        }

        return msg;
    }

    /// Process a permission decision from the local UI.
    ///
    /// Looks up the pending request by @p request_id. If found, the
    /// response is forwarded to the remote side via the response callback
    /// and the entry is removed from the pending map.
    ///
    /// If the user edited the tool input, @p response.updated_input_json
    /// will contain the modified JSON; otherwise it is nullopt.
    void handle_permission_result(
        const std::string& request_id,
        const RemotePermissionResponse& response)
    {
        std::lock_guard lock(mutex_);

        auto it = pending_.find(request_id);
        if (it == pending_.end()) {
            // Unknown request — ignore silently (may have been cancelled).
            return;
        }

        // Remove from pending before invoking callback (prevents re-entrancy issues).
        pending_.erase(it);

        // Forward to remote via callback.
        if (respond_callback_) {
            respond_callback_(response);
        }
    }

    /// Return the number of currently pending permission requests.
    [[nodiscard]] auto pending_count() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

    /// Check whether a specific request is still pending.
    [[nodiscard]] bool has_pending(const std::string& request_id) const {
        std::lock_guard lock(mutex_);
        return pending_.find(request_id) != pending_.end();
    }

    /// Cancel all pending permission requests.
    ///
    /// Each pending request is removed and a denial response is sent back
    /// to the remote side for each one. The cancel callback (if set) is
    /// invoked once after all denials have been dispatched.
    void cancel_all() {
        std::unordered_map<std::string, detail::PendingEntry> to_cancel;

        {
            std::lock_guard lock(mutex_);
            to_cancel = std::move(pending_);
            pending_.clear();
        }

        // Send denial for each cancelled request.
        for (const auto& [id, entry] : to_cancel) {
            if (respond_callback_) {
                RemotePermissionResponse denial;
                denial.request_id = id;
                denial.behavior = "deny";
                respond_callback_(denial);
            }
        }

        // Notify via cancel callback.
        if (cancel_callback_) {
            cancel_callback_();
        }
    }

    /// Get the age of a pending request (returns nullopt if not found).
    [[nodiscard]] auto pending_age(const std::string& request_id) const
        -> std::optional<std::chrono::steady_clock::duration>
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return std::nullopt;
        return std::chrono::steady_clock::now() - it->second.submitted_at;
    }

private:
    /// Callback invoked when the local UI resolves a permission request.
    PermissionResponseCallback respond_callback_;

    /// Callback invoked when cancel_all() fires.
    CancelCallback cancel_callback_;

    /// Map of request_id -> pending entry.
    std::unordered_map<std::string, detail::PendingEntry> pending_;

    /// Mutex protecting pending_ and all reads.
    mutable std::mutex mutex_;
};

} // namespace cc::remote
