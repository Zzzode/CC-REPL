// SendMessageTool - Inter-agent messaging for coordinated multi-agent workflows
module;
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.send_message;


export namespace cc::tools {

// Error types for send message operations
enum class SendMessageError {
    TargetEmpty,
    MessageEmpty,
    TargetNotFound,
    QueueFull,
    DeliveryFailed,
    Timeout,
    InvalidFormat,
    SelfMessage,
};

constexpr auto format_error(SendMessageError err) -> std::string_view {
    switch (err) {
        case SendMessageError::TargetEmpty:    return "Target agent ID is empty";
        case SendMessageError::MessageEmpty:   return "Message content is empty";
        case SendMessageError::TargetNotFound: return "Target agent not found";
        case SendMessageError::QueueFull:      return "Message queue is full";
        case SendMessageError::DeliveryFailed: return "Message delivery failed";
        case SendMessageError::Timeout:        return "Message delivery timed out";
        case SendMessageError::InvalidFormat:  return "Invalid message format";
        case SendMessageError::SelfMessage:    return "Cannot send message to self";
        default:                               return "Unknown send message error";
    }
}

// Message priority levels
enum class MessagePriority {
    Low,
    Normal,
    High,
    Urgent,
};

constexpr auto message_priority_name(MessagePriority p) -> std::string_view {
    switch (p) {
        case MessagePriority::Low:    return "low";
        case MessagePriority::Normal: return "normal";
        case MessagePriority::High:   return "high";
        case MessagePriority::Urgent: return "urgent";
        default:                      return "unknown";
    }
}

// Delivery status for sent messages
enum class DeliveryStatus {
    Queued,
    Delivered,
    Read,
    Failed,
};

constexpr auto delivery_status_name(DeliveryStatus s) -> std::string_view {
    switch (s) {
        case DeliveryStatus::Queued:    return "queued";
        case DeliveryStatus::Delivered: return "delivered";
        case DeliveryStatus::Read:      return "read";
        case DeliveryStatus::Failed:    return "failed";
        default:                        return "unknown";
    }
}

// Message structure for inter-agent communication
struct AgentMessage {
    std::string id;
    std::string from_agent;
    std::string to_agent;
    std::string content;
    MessagePriority priority{MessagePriority::Normal};
    DeliveryStatus status{DeliveryStatus::Queued};
    std::chrono::system_clock::time_point sent_at;
    std::optional<std::chrono::system_clock::time_point> delivered_at;
    std::optional<std::string> reply_to;  // ID of message being replied to
    std::unordered_map<std::string, std::string> metadata;
};

// Delivery confirmation
struct DeliveryConfirmation {
    std::string message_id;
    DeliveryStatus status;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> error_detail;
};

// Message queue for an agent
class MessageQueue {
public:
    static constexpr size_t kMaxQueueSize = 100;

    auto enqueue(AgentMessage msg) -> std::expected<void, SendMessageError> {
        if (messages_.size() >= kMaxQueueSize) {
            return std::unexpected(SendMessageError::QueueFull);
        }
        messages_.push_back(std::move(msg));
        return {};
    }

    auto dequeue() -> std::optional<AgentMessage> {
        if (messages_.empty()) return std::nullopt;
        auto msg = std::move(messages_.front());
        messages_.pop_front();
        return msg;
    }

    auto peek() const -> const AgentMessage* {
        if (messages_.empty()) return nullptr;
        return &messages_.front();
    }

    [[nodiscard]] size_t size() const { return messages_.size(); }
    [[nodiscard]] bool empty() const { return messages_.empty(); }

    // Get all pending messages (non-destructive)
    [[nodiscard]] auto pending() const -> std::vector<const AgentMessage*> {
        std::vector<const AgentMessage*> result;
        for (const auto& msg : messages_) {
            result.push_back(&msg);
        }
        return result;
    }

private:
    std::deque<AgentMessage> messages_;
};

// Message router: manages queues for all known agents
class MessageRouter {
public:
    static MessageRouter& instance() {
        static MessageRouter router;
        return router;
    }

    // Register an agent to receive messages
    void register_agent(const std::string& agent_id) {
        if (!queues_.contains(agent_id)) {
            queues_[agent_id] = std::make_unique<MessageQueue>();
        }
    }

    // Unregister an agent
    void unregister_agent(const std::string& agent_id) {
        queues_.erase(agent_id);
    }

    // Route a message to target agent's queue
    auto route(AgentMessage msg) -> std::expected<DeliveryConfirmation, SendMessageError> {
        auto it = queues_.find(msg.to_agent);
        if (it == queues_.end()) {
            return std::unexpected(SendMessageError::TargetNotFound);
        }

        auto msg_id = msg.id;
        auto enqueue_result = it->second->enqueue(std::move(msg));
        if (!enqueue_result) return std::unexpected(enqueue_result.error());

        return DeliveryConfirmation{
            .message_id = msg_id,
            .status = DeliveryStatus::Delivered,
            .timestamp = std::chrono::system_clock::now(),
        };
    }

    // Get queue for an agent
    auto get_queue(const std::string& agent_id) -> MessageQueue* {
        auto it = queues_.find(agent_id);
        if (it == queues_.end()) return nullptr;
        return it->second.get();
    }

    [[nodiscard]] bool agent_exists(const std::string& agent_id) const {
        return queues_.contains(agent_id);
    }

private:
    MessageRouter() = default;
    std::unordered_map<std::string, std::unique_ptr<MessageQueue>> queues_;
};

// SendMessageTool - sends messages between agents
class SendMessageTool {
public:
    static constexpr std::string_view name = "send_message";
    static constexpr std::string_view description = "Send a message to another agent for coordination";
    static constexpr size_t kMaxMessageSize = 100 * 1024;  // 100KB

    explicit SendMessageTool(std::string self_agent_id = "main")
        : self_id_(std::move(self_agent_id))
    {
        // Ensure self is registered
        MessageRouter::instance().register_agent(self_id_);
    }

    // Validate message before sending
    auto validate(const std::string& target, const std::string& content) const
        -> std::expected<void, SendMessageError>
    {
        if (target.empty()) return std::unexpected(SendMessageError::TargetEmpty);
        if (content.empty()) return std::unexpected(SendMessageError::MessageEmpty);
        if (target == self_id_) return std::unexpected(SendMessageError::SelfMessage);
        if (content.size() > kMaxMessageSize) return std::unexpected(SendMessageError::InvalidFormat);
        return {};
    }

    // Send a message to a target agent
    auto execute(std::string target, std::string content,
                 MessagePriority priority = MessagePriority::Normal,
                 std::optional<std::string> reply_to = std::nullopt)
        -> std::expected<DeliveryConfirmation, SendMessageError>
    {
        if (auto valid = validate(target, content); !valid) {
            return std::unexpected(valid.error());
        }

        AgentMessage msg{
            .id = generate_message_id(),
            .from_agent = self_id_,
            .to_agent = target,
            .content = std::move(content),
            .priority = priority,
            .status = DeliveryStatus::Queued,
            .sent_at = std::chrono::system_clock::now(),
            .reply_to = std::move(reply_to),
        };

        return MessageRouter::instance().route(std::move(msg));
    }

    // Check for incoming messages
    auto receive() -> std::optional<AgentMessage> {
        auto* queue = MessageRouter::instance().get_queue(self_id_);
        if (!queue) return std::nullopt;
        return queue->dequeue();
    }

    // Peek at pending messages without consuming them
    [[nodiscard]] auto pending_count() const -> size_t {
        auto* queue = MessageRouter::instance().get_queue(self_id_);
        if (!queue) return 0;
        return queue->size();
    }

    // Generate JSON schema for LLM tool invocation
    auto schema() const -> std::string {
        return std::format(R"({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "target_agent": {{ "type": "string", "description": "ID of the target agent" }},
      "content": {{ "type": "string", "description": "Message content to send" }},
      "priority": {{ "type": "string", "enum": ["low", "normal", "high", "urgent"] }},
      "reply_to": {{ "type": "string", "description": "Message ID being replied to" }}
    }},
    "required": ["target_agent", "content"]
  }}
}})", name, description);
    }

private:
    std::string self_id_;
    size_t next_msg_id_{0};

    auto generate_message_id() -> std::string {
        return std::format("msg_{}_{}", self_id_, next_msg_id_++);
    }
};

} // namespace cc::tools
