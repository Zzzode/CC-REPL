/// @file in_process_transport.cppm
/// @brief In-process linked transport pair for MCP servers/clients.
///
/// Mirrors src/services/mcp/InProcessTransport.ts. A linked pair lets an MCP
/// server and client run in the same process without spawning a subprocess:
/// `send()` on one side is delivered to the peer's `on_message` handler, and
/// `close()` on either side fans out to both sides' `on_close` handlers.
///
/// The TS original defers delivery via queueMicrotask() to bound stack depth
/// on synchronous request/response cycles. The C++ port delivers synchronously
/// (no microtask scheduler in the migration); this is acceptable parity because
/// the only observable difference is stack depth under deeply nested sends,
/// which the MCP message loop does not drive.
module;
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
export module cc.services.mcp.in_process_transport;
export namespace cc::services::mcp {

/// A JSON-RPC message carried over the in-process transport. The raw JSON body
/// is passed through unchanged so callers can parse it with their own JSON
/// library (matches the opaque JSONRPCMessage contract in the TS SDK).
struct InProcessMessage {
    std::string body_json;  // raw JSON-RPC message payload
};

/// Shared state for one side of a linked pair. Both sides hold a
/// shared_ptr to each other's state so send()/close() can reach across.
struct InProcessTransportState {
    std::function<void(const InProcessMessage&)> on_message;
    std::function<void()> on_close;
    mutable std::mutex mutex;
    bool closed = false;
    std::shared_ptr<InProcessTransportState> peer;  // cycles broken explicitly on close
};

/// One half of a linked transport pair. Mirrors InProcessTransport in TS:
/// send() throws when closed, otherwise forwards to the peer's on_message;
/// close() marks both sides closed and invokes on_close on both.
class InProcessTransport {
public:
    InProcessTransport() = default;
    explicit InProcessTransport(std::shared_ptr<InProcessTransportState> state)
        : state_(std::move(state)) {}

    /// No-op start, parity with TS (async start() resolves immediately).
    std::expected<void, std::string> start() { return {}; }

    /// Deliver a message to the peer's on_message handler. Errors if this
    /// side (or its peer) is closed.
    std::expected<void, std::string> send(const InProcessMessage& message) {
        auto state = state_;
        if (!state) return std::unexpected("Transport has no state");
        std::function<void(const InProcessMessage&)> handler;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed) {
                return std::unexpected("Transport is closed");
            }
            auto peer = state->peer;
            if (!peer) {
                return std::unexpected("Transport has no peer");
            }
            std::lock_guard peer_lock(peer->mutex);
            if (peer->closed) {
                return std::unexpected("Transport is closed");
            }
            handler = peer->on_message;
        }
        if (handler) handler(message);
        return {};
    }

    /// Close this side; if the peer is still open, close it too and fire its
    /// on_close handler. Both on_close handlers fire at most once.
    std::expected<void, std::string> close() {
        auto state = state_;
        if (!state) return {};
        std::function<void()> our_close;
        std::shared_ptr<InProcessTransportState> peer_state;
        std::function<void()> peer_close;
        {
            std::lock_guard lock(state->mutex);
            if (!state->closed) {
                state->closed = true;
                our_close = state->on_close;
            }
            peer_state = state->peer;
        }
        if (peer_state) {
            std::lock_guard peer_lock(peer_state->mutex);
            if (!peer_state->closed) {
                peer_state->closed = true;
                peer_close = peer_state->on_close;
            }
        }
        if (our_close) our_close();
        if (peer_close) peer_close();
        return {};
    }

    /// Wire the message-delivery handler on this side. Mirrors `onmessage =`.
    void on_message(std::function<void(const InProcessMessage&)> handler) {
        std::lock_guard lock(state_->mutex);
        state_->on_message = std::move(handler);
    }

    /// Wire the close-notification handler on this side. Mirrors `onclose =`.
    void on_close(std::function<void()> handler) {
        std::lock_guard lock(state_->mutex);
        state_->on_close = std::move(handler);
    }

    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(state_->mutex);
        return state_->closed;
    }

private:
    std::shared_ptr<InProcessTransportState> state_;
};

/// A pair of linked transports: messages sent on `first` are delivered to
/// `second.on_message`, and vice-versa. Parity with createLinkedTransportPair.
struct LinkedTransportPair {
    InProcessTransport first;   // typically the client side
    InProcessTransport second;  // typically the server side
};

/// Create a pair of linked in-process transports.
[[nodiscard]] inline LinkedTransportPair create_linked_transport_pair() {
    auto a_state = std::make_shared<InProcessTransportState>();
    auto b_state = std::make_shared<InProcessTransportState>();
    a_state->peer = b_state;
    b_state->peer = a_state;
    return LinkedTransportPair{
        InProcessTransport{a_state},
        InProcessTransport{b_state},
    };
}

} // namespace cc::services::mcp
