/// @file mailbox.cppm
/// @brief Inter-agent message passing module for the Claude Code CLI engine.
/// Implements typed mailboxes, message routing, priority-based delivery,
/// and both non-blocking and coroutine-awaitable receive operations.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <coroutine>
#include <atomic>
#include <functional>

export module cc.context.mailbox;

import cc.types.types;
import cc.coordinator.swarm;

export namespace cc::core {

// ============================================================
// Message envelope and delivery types
// ============================================================

/// Priority levels for message delivery ordering
enum class MessagePriority : std::uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3,
};

/// Delivery status tracking for sent messages
enum class DeliveryStatus : std::uint8_t {
    Queued,     // Message is in the recipient's mailbox
    Delivered,  // Message was consumed by recipient
    Failed,     // Delivery failed (mailbox full, recipient gone)
    Expired,    // Message TTL elapsed before delivery
};

/// Convert DeliveryStatus to display string
[[nodiscard]] constexpr std::string_view delivery_status_to_string(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::Queued:    return "queued";
        case DeliveryStatus::Delivered: return "delivered";
        case DeliveryStatus::Failed:    return "failed";
        case DeliveryStatus::Expired:   return "expired";
    }
    return "unknown";
}

/// Strong ID for messages
struct MessageEnvelopeIdTag {};
using EnvelopeId = StrongId<MessageEnvelopeIdTag>;

/// Envelope wrapping a message with routing metadata
template <typename T>
struct MessageEnvelope {
    EnvelopeId id;                                     // Unique envelope identifier
    WorkerId from;                                     // Sender agent ID
    WorkerId to;                                       // Recipient agent ID
    std::chrono::system_clock::time_point timestamp;   // When the message was sent
    T payload;                                         // Actual message content
    MessagePriority priority = MessagePriority::Normal;
    DeliveryStatus status = DeliveryStatus::Queued;
    std::optional<std::chrono::milliseconds> ttl;      // Time-to-live (nullopt = forever)

    /// Check if this message has expired based on current time
    [[nodiscard]] bool is_expired() const {
        if (!ttl) return false;
        auto elapsed = std::chrono::system_clock::now() - timestamp;
        return elapsed > *ttl;
    }

    /// Compare by priority for ordering (higher priority first)
    [[nodiscard]] bool operator<(const MessageEnvelope& other) const noexcept {
        return static_cast<int>(priority) < static_cast<int>(other.priority);
    }
};

// ============================================================
// Mailbox<T> - per-agent message queue
// ============================================================

/// Awaiter for coroutine-based blocking receive
template <typename T>
struct MailboxAwaiter {
    std::deque<MessageEnvelope<T>>& queue;
    std::mutex& mutex;
    std::condition_variable& cv;
    MessageEnvelope<T> result;

    bool await_ready() {
        std::lock_guard lock(mutex);
        return !queue.empty();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        // Resume when a message is available on the condition variable.
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return !queue.empty(); });
        handle.resume();
    }

    MessageEnvelope<T> await_resume() {
        std::lock_guard lock(mutex);
        auto msg = std::move(queue.front());
        queue.pop_front();
        msg.status = DeliveryStatus::Delivered;
        return msg;
    }
};

/// Thread-safe mailbox for receiving messages of type T.
/// Supports priority ordering, non-blocking peek, and coroutine await.
template <typename T>
class Mailbox {
public:
    explicit Mailbox(std::size_t max_capacity = 1024)
        : max_capacity_(max_capacity) {}

    // Non-copyable, movable
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    Mailbox(Mailbox&&) noexcept = default;
    Mailbox& operator=(Mailbox&&) noexcept = default;

    /// Enqueue a message into this mailbox
    [[nodiscard]] Result<DeliveryStatus> send(MessageEnvelope<T> envelope) {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= max_capacity_) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Mailbox capacity exceeded"));
        }
        // Remove expired messages opportunistically
        purge_expired_locked();
        envelope.status = DeliveryStatus::Queued;
        queue_.push_back(std::move(envelope));
        // Re-sort by priority (stable sort preserves FIFO within same priority)
        std::ranges::stable_sort(queue_,
            [](const auto& a, const auto& b) {
                return static_cast<int>(a.priority) > static_cast<int>(b.priority);
            });
        cv_.notify_one();
        return DeliveryStatus::Queued;
    }

    /// Try to receive a message without blocking. Returns nullopt if empty.
    [[nodiscard]] std::optional<MessageEnvelope<T>> receive() {
        std::lock_guard lock(mutex_);
        purge_expired_locked();
        if (queue_.empty()) return std::nullopt;
        auto msg = std::move(queue_.front());
        queue_.pop_front();
        msg.status = DeliveryStatus::Delivered;
        return msg;
    }

    /// Blocking receive using coroutine suspension.
    /// Returns an awaiter for co_await usage.
    [[nodiscard]] MailboxAwaiter<T> receive_blocking() {
        return MailboxAwaiter<T>{queue_, mutex_, cv_, {}};
    }

    /// Peek at the front message without consuming it
    [[nodiscard]] std::optional<MessageEnvelope<T>> peek() const {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        // Return a copy without removing
        return queue_.front();
    }

    /// Check if there are pending messages
    [[nodiscard]] bool has_messages() const {
        std::lock_guard lock(mutex_);
        return !queue_.empty();
    }

    /// Get the number of pending messages
    [[nodiscard]] std::size_t message_count() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    /// Clear all messages from the mailbox
    void clear() {
        std::lock_guard lock(mutex_);
        queue_.clear();
    }

    /// Get the maximum capacity
    [[nodiscard]] std::size_t capacity() const noexcept { return max_capacity_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<MessageEnvelope<T>> queue_;
    std::size_t max_capacity_;

    /// Remove expired messages (must be called with lock held)
    void purge_expired_locked() {
        std::erase_if(queue_, [](const MessageEnvelope<T>& msg) {
            return msg.is_expired();
        });
    }
};

// ============================================================
// MailboxRouter - routes messages between agents
// ============================================================

/// Delivery receipt returned after routing a message
struct DeliveryReceipt {
    EnvelopeId envelope_id;
    DeliveryStatus status;
    std::chrono::system_clock::time_point routed_at;
};

/// Routes messages between agent mailboxes, managing registration and delivery.
/// Uses string-typed messages for routing flexibility; agents interpret payloads.
class MailboxRouter {
public:
    /// Register a new mailbox for an agent
    VoidResult register_mailbox(const WorkerId& agent_id) {
        std::lock_guard lock(mutex_);
        if (mailboxes_.contains(agent_id.value)) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("Mailbox already registered for agent '{}'", agent_id.value)));
        }
        mailboxes_.emplace(agent_id.value, std::make_unique<Mailbox<std::string>>());
        return {};
    }

    /// Unregister a mailbox (typically when an agent terminates)
    VoidResult unregister_mailbox(const WorkerId& agent_id) {
        std::lock_guard lock(mutex_);
        if (!mailboxes_.contains(agent_id.value)) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Mailbox not found"));
        }
        mailboxes_.erase(agent_id.value);
        return {};
    }

    /// Route a message from one agent to another
    [[nodiscard]] Result<DeliveryReceipt> route(const WorkerId& from,
                                                 const WorkerId& to,
                                                 std::string message,
                                                 MessagePriority priority = MessagePriority::Normal) {
        std::lock_guard lock(mutex_);
        auto it = mailboxes_.find(to.value);
        if (it == mailboxes_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("No mailbox for recipient '{}'", to.value)));
        }

        auto envelope_id = generate_envelope_id();
        MessageEnvelope<std::string> envelope{
            .id = envelope_id,
            .from = from,
            .to = to,
            .timestamp = std::chrono::system_clock::now(),
            .payload = std::move(message),
            .priority = priority,
            .status = DeliveryStatus::Queued,
            .ttl = std::nullopt,
        };

        auto result = it->second->send(std::move(envelope));
        if (!result) return std::unexpected(result.error());

        return DeliveryReceipt{
            .envelope_id = envelope_id,
            .status = *result,
            .routed_at = std::chrono::system_clock::now(),
        };
    }

    /// Broadcast a message from one agent to all other registered agents
    [[nodiscard]] Result<std::vector<DeliveryReceipt>> broadcast(
            const WorkerId& from,
            const std::string& message,
            MessagePriority priority = MessagePriority::Normal) {
        std::vector<DeliveryReceipt> receipts;
        // Collect recipients (everyone except sender)
        std::vector<std::string> recipients;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, _] : mailboxes_) {
                if (id != from.value) {
                    recipients.push_back(id);
                }
            }
        }

        for (const auto& recipient_id : recipients) {
            // Unlock and re-route individually to avoid holding lock too long
            auto result = route(from, WorkerId{recipient_id}, message, priority);
            if (result) {
                receipts.push_back(std::move(*result));
            }
        }
        return receipts;
    }

    /// Get a reference to an agent's mailbox for direct operations
    [[nodiscard]] Result<Mailbox<std::string>*> get_mailbox(const WorkerId& agent_id) {
        std::lock_guard lock(mutex_);
        auto it = mailboxes_.find(agent_id.value);
        if (it == mailboxes_.end()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                std::format("No mailbox for agent '{}'", agent_id.value)));
        }
        return it->second.get();
    }

    /// Check if an agent has a registered mailbox
    [[nodiscard]] bool has_mailbox(const WorkerId& agent_id) const {
        std::lock_guard lock(mutex_);
        return mailboxes_.contains(agent_id.value);
    }

    /// Get count of registered mailboxes
    [[nodiscard]] std::size_t mailbox_count() const {
        std::lock_guard lock(mutex_);
        return mailboxes_.size();
    }

    /// Get IDs of all registered agents
    [[nodiscard]] std::vector<WorkerId> registered_agents() const {
        std::lock_guard lock(mutex_);
        std::vector<WorkerId> agents;
        agents.reserve(mailboxes_.size());
        for (const auto& [id, _] : mailboxes_) {
            agents.push_back(WorkerId{id});
        }
        return agents;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Mailbox<std::string>>> mailboxes_;
    std::atomic<std::uint64_t> next_envelope_id_{1};

    /// Generate a unique envelope ID
    [[nodiscard]] EnvelopeId generate_envelope_id() {
        auto id = next_envelope_id_.fetch_add(1, std::memory_order_relaxed);
        return EnvelopeId{std::format("env-{}", id)};
    }
};

} // namespace cc::core
