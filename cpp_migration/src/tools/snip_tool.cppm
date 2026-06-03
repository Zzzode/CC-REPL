// SnipTool - History/context snipping for conversation management
module;
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.snip;


export namespace cc::tools {


enum class SnipAction {
    RemoveMessage,
    TrimFrom,
    Compact,
    Recalculate,
};

constexpr auto snip_action_name(SnipAction a) -> std::string_view {
    switch (a) {
        case SnipAction::RemoveMessage: return "remove";
        case SnipAction::TrimFrom:     return "trim";
        case SnipAction::Compact:      return "compact";
        case SnipAction::Recalculate:  return "recalculate";
        default:                       return "unknown";
    }
}


enum class SnipError {
    MessageNotFound,
    IndexOutOfRange,
    InvalidRange,
    EmptyHistory,
    ProtectedMessage,
    RecalculationFailed,
};

constexpr auto format_error(SnipError err) -> std::string_view {
    switch (err) {
        case SnipError::MessageNotFound:      return "Message not found in history";
        case SnipError::IndexOutOfRange:      return "Message index out of range";
        case SnipError::InvalidRange:         return "Invalid range specification";
        case SnipError::EmptyHistory:         return "Conversation history is empty";
        case SnipError::ProtectedMessage:     return "Cannot remove system/protected messages";
        case SnipError::RecalculationFailed:  return "Token recalculation failed";
        default:                              return "Unknown snip error";
    }
}


enum class MessageRole {
    System,
    User,
    Assistant,
    Tool,
};

constexpr auto role_name(MessageRole r) -> std::string_view {
    switch (r) {
        case MessageRole::System:    return "system";
        case MessageRole::User:      return "user";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool:      return "tool";
        default:                     return "unknown";
    }
}


struct Message {
    std::string id;
    MessageRole role;
    std::string content;
    size_t token_count{0};
    bool is_protected{false};
    std::chrono::system_clock::time_point timestamp;
};


struct SnipRequest {
    SnipAction action;
    std::optional<std::string> message_id;
    std::optional<size_t> from_index;
    std::optional<size_t> to_index;
    std::optional<size_t> keep_last_n;
};


struct SnipResult {
    size_t messages_removed{0};
    size_t tokens_freed{0};
    size_t remaining_messages{0};
    size_t remaining_tokens{0};
};


class ConversationHistory {
public:

    void push(Message msg) {
        total_tokens_ += msg.token_count;
        messages_.push_back(std::move(msg));
    }


    [[nodiscard]] size_t size() const { return messages_.size(); }
    [[nodiscard]] bool empty() const { return messages_.empty(); }
    [[nodiscard]] size_t total_tokens() const { return total_tokens_; }


    auto find_index(std::string_view id) const -> std::optional<size_t> {
        for (size_t i = 0; i < messages_.size(); ++i) {
            if (messages_[i].id == id) return i;
        }
        return std::nullopt;
    }


    auto remove_at(size_t index) -> std::expected<size_t, SnipError> {
        if (index >= messages_.size()) {
            return std::unexpected(SnipError::IndexOutOfRange);
        }
        if (messages_[index].is_protected) {
            return std::unexpected(SnipError::ProtectedMessage);
        }
        size_t tokens = messages_[index].token_count;
        total_tokens_ -= tokens;
        messages_.erase(messages_.begin() + static_cast<ptrdiff_t>(index));
        return tokens;
    }


    auto trim_from(size_t index) -> std::expected<SnipResult, SnipError> {
        if (index >= messages_.size()) {
            return std::unexpected(SnipError::IndexOutOfRange);
        }

        SnipResult result;

        for (size_t i = index; i < messages_.size(); ++i) {
            if (!messages_[i].is_protected) {
                result.tokens_freed += messages_[i].token_count;
                result.messages_removed++;
            }
        }


        std::vector<Message> kept;
        kept.reserve(index);
        for (size_t i = 0; i < messages_.size(); ++i) {
            if (i < index || messages_[i].is_protected) {
                kept.push_back(std::move(messages_[i]));
            }
        }
        messages_ = std::move(kept);
        total_tokens_ -= result.tokens_freed;

        result.remaining_messages = messages_.size();
        result.remaining_tokens = total_tokens_;
        return result;
    }


    auto recalculate() -> size_t {
        total_tokens_ = 0;
        for (const auto& msg : messages_) {
            total_tokens_ += msg.token_count;
        }
        return total_tokens_;
    }


    [[nodiscard]] auto messages() const -> const std::vector<Message>& {
        return messages_;
    }

private:
    std::vector<Message> messages_;
    size_t total_tokens_{0};
};


class SnipTool {
public:
    static constexpr std::string_view name = "snip";
    static constexpr std::string_view description = "Remove or trim messages from conversation history to manage context";

    explicit SnipTool(ConversationHistory& history) : history_(history) {}

    auto validate(const SnipRequest& request) const -> std::expected<void, SnipError> {
        if (history_.empty()) {
            return std::unexpected(SnipError::EmptyHistory);
        }
        if (request.action == SnipAction::RemoveMessage && !request.message_id) {
            return std::unexpected(SnipError::MessageNotFound);
        }
        if (request.action == SnipAction::TrimFrom && !request.from_index) {
            return std::unexpected(SnipError::InvalidRange);
        }
        return {};
    }

    auto execute(SnipRequest request) -> std::expected<SnipResult, SnipError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        switch (request.action) {
            case SnipAction::RemoveMessage: {
                auto index = history_.find_index(*request.message_id);
                if (!index) return std::unexpected(SnipError::MessageNotFound);

                auto tokens = history_.remove_at(*index);
                if (!tokens) return std::unexpected(tokens.error());

                return SnipResult{
                    .messages_removed = 1,
                    .tokens_freed = *tokens,
                    .remaining_messages = history_.size(),
                    .remaining_tokens = history_.total_tokens(),
                };
            }
            case SnipAction::TrimFrom: {
                return history_.trim_from(*request.from_index);
            }
            case SnipAction::Compact: {

                auto before_tokens = history_.total_tokens();

                return SnipResult{
                    .remaining_messages = history_.size(),
                    .remaining_tokens = history_.total_tokens(),
                };
            }
            case SnipAction::Recalculate: {
                auto new_total = history_.recalculate();
                return SnipResult{
                    .remaining_messages = history_.size(),
                    .remaining_tokens = new_total,
                };
            }
            default:
                return std::unexpected(SnipError::InvalidRange);
        }
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "action": {{ "type": "string", "enum": ["remove", "trim", "compact", "recalculate"], "description": "Snip action to perform" }},
      "message_id": {{ "type": "string", "description": "ID of the message to remove" }},
      "from_index": {{ "type": "integer", "description": "Index to trim from (removes this and all after)" }},
      "keep_last_n": {{ "type": "integer", "description": "Keep only the last N messages" }}
    }},
    "required": ["action"]
  }}
}})json", name, description);
    }

private:
    ConversationHistory& history_;
};

} // namespace cc::tools
