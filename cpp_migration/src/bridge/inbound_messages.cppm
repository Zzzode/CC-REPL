module;
#include <string>
#include <map>
#include <optional>
#include <chrono>
#include <queue>
#include <mutex>

export module cc.bridge.inbound_messages;

export namespace cc::bridge {

// An inbound message received via the bridge
struct InboundMessage {
    std::string type;
    std::string content;
    std::map<std::string, std::string> metadata;
    std::chrono::system_clock::time_point received;
};

// Thread-safe queue for inbound messages
class InboundMessageQueue {
public:
    InboundMessageQueue() = default;

    // Push a new message to the back of the queue
    void push(InboundMessage msg) {
        std::lock_guard lock(mutex_);
        queue_.push(std::move(msg));
    }

    // Pop the front message from the queue (returns nullopt if empty)
    std::optional<InboundMessage> pop() {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }

        auto msg = std::move(queue_.front());
        queue_.pop();
        return msg;
    }

    // Peek at the front message without removing it
    std::optional<InboundMessage> peek() const {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        return queue_.front();
    }

    // Get the current number of messages in the queue
    size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    // Clear all messages from the queue
    void clear() {
        std::lock_guard lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }

private:
    std::queue<InboundMessage> queue_;
    mutable std::mutex mutex_;
};

} // namespace cc::bridge
