// cc.hooks.mailbox_bridge — migrated from useMailboxBridge.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <chrono>
#include <mutex>
#include <deque>
#include <functional>
#include <atomic>
#include <cstdint>

export module cc.hooks.mailbox_bridge;

export namespace cc::hooks::mailbox_bridge {

struct MailboxMessage {
    std::string id;
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::chrono::system_clock::time_point sent_at;
    bool delivered{false};
};

struct MailboxState {
    std::vector<MailboxMessage> inbox;
    std::vector<MailboxMessage> outbox;
    bool connected;
    std::string agent_id;
    std::chrono::system_clock::time_point connected_since;
};

using MessageCallback = std::function<void(const MailboxMessage&)>;
using MessageSender = std::function<std::expected<void, std::string>(const MailboxMessage&)>;

namespace detail {

struct MailboxInternalState {
    std::mutex mutex;
    std::deque<MailboxMessage> inbox;
    std::deque<MailboxMessage> outbox;
    std::vector<MessageCallback> on_receive_callbacks;
    MessageSender sender;
    std::string agent_id;
    std::atomic<bool> connected{false};
    std::chrono::system_clock::time_point connected_since;
    std::uint64_t next_msg_id{1};
    static constexpr int MAX_MESSAGES = 200;
};

inline auto get_state() -> MailboxInternalState& {
    static MailboxInternalState state;
    return state;
}

inline auto generate_message_id() -> std::string {
    auto& state = get_state();
    auto id = state.next_msg_id++;
    return "msg-" + std::to_string(id);
}

} // namespace detail

/// Configure the transport used for outbound mailbox messages.
inline void set_message_sender(MessageSender sender) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.sender = std::move(sender);
}

/// Connect to the mailbox system with the given agent identity.
inline std::expected<void, std::string> connect_mailbox(std::string_view agent_id) {
    if (agent_id.empty()) {
        return std::unexpected(std::string{"agent_id must not be empty"});
    }

    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!state.sender) {
        return std::unexpected(std::string{"Mailbox message sender is not configured"});
    }

    if (state.connected.load()) {
        if (state.agent_id == agent_id) {
            return {}; // Already connected as this agent
        }
        return std::unexpected(std::string{"Already connected as agent: " + state.agent_id});
    }

    state.agent_id = std::string{agent_id};
    state.connected.store(true);
    state.connected_since = std::chrono::system_clock::now();
    return {};
}

/// Disconnect from the mailbox system.
inline void disconnect_mailbox() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.connected.store(false);
    state.agent_id.clear();
}

/// Send a message through the mailbox.
inline std::expected<std::string, std::string> send_message(MailboxMessage msg) {
    auto& state = detail::get_state();
    MessageSender sender;

    {
        std::lock_guard lock(state.mutex);

        if (!state.connected.load()) {
            return std::unexpected(std::string{"Mailbox not connected"});
        }

        if (msg.to.empty()) {
            return std::unexpected(std::string{"Message recipient (to) must not be empty"});
        }

        msg.id = detail::generate_message_id();
        msg.from = state.agent_id;
        msg.sent_at = std::chrono::system_clock::now();
        msg.delivered = false;

        if (!state.sender) {
            return std::unexpected(std::string{"Mailbox message sender is not configured"});
        }
        sender = state.sender;

        // Enforce outbox size limit
        while (static_cast<int>(state.outbox.size()) >= detail::MailboxInternalState::MAX_MESSAGES) {
            state.outbox.pop_front();
        }

        state.outbox.push_back(msg);
    }

    const auto id = msg.id;
    auto delivered = sender(msg);
    if (!delivered) return std::unexpected(delivered.error());

    {
        std::lock_guard lock(state.mutex);
        if (!state.outbox.empty() && state.outbox.back().id == id) {
            state.outbox.back().delivered = true;
        }
    }
    return id;
}

/// Receive pending messages (called by the transport layer).
inline void deliver_message(MailboxMessage msg) {
    auto& state = detail::get_state();
    std::vector<MessageCallback> callbacks;

    {
        std::lock_guard lock(state.mutex);

        if (!state.connected.load()) return;

        // Only accept messages addressed to us (or broadcast)
        if (!msg.to.empty() && msg.to != state.agent_id && msg.to != "*") {
            return;
        }

        msg.delivered = true;

        // Enforce inbox size limit
        while (static_cast<int>(state.inbox.size()) >= detail::MailboxInternalState::MAX_MESSAGES) {
            state.inbox.pop_front();
        }

        state.inbox.push_back(msg);
        callbacks = state.on_receive_callbacks;
    }

    for (const auto& cb : callbacks) {
        cb(msg);
    }
}

/// Get all messages currently in the inbox.
inline std::vector<MailboxMessage> get_inbox() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return {state.inbox.begin(), state.inbox.end()};
}

/// Get recent messages from outbox.
inline std::vector<MailboxMessage> get_outbox() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return {state.outbox.begin(), state.outbox.end()};
}

/// Get the current mailbox state snapshot.
inline MailboxState get_mailbox_state() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return MailboxState{
        .inbox = {state.inbox.begin(), state.inbox.end()},
        .outbox = {state.outbox.begin(), state.outbox.end()},
        .connected = state.connected.load(),
        .agent_id = state.agent_id,
        .connected_since = state.connected_since
    };
}

/// Register a callback for incoming messages.
inline void on_message_received(MessageCallback callback) {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.on_receive_callbacks.push_back(std::move(callback));
}

/// Check if connected.
inline bool is_connected() {
    return detail::get_state().connected.load();
}

/// Clear the inbox.
inline void clear_inbox() {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.inbox.clear();
}

} // namespace cc::hooks::mailbox_bridge
