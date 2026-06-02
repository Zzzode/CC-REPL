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

// Snip 操作类型
enum class SnipAction {
    RemoveMessage,    // 删除特定消息
    TrimFrom,        // 从某点开始裁剪
    Compact,         // 压缩/合并连续消息
    Recalculate,     // 重新计算 token 使用量
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

// Snip 错误类型
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

// 消息角色
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

// 对话消息
struct Message {
    std::string id;
    MessageRole role;
    std::string content;
    size_t token_count{0};         // 该消息占用的 token 数
    bool is_protected{false};      // 系统消息不可删除
    std::chrono::system_clock::time_point timestamp;
};

// Snip 请求
struct SnipRequest {
    SnipAction action;
    std::optional<std::string> message_id;       // 特定消息 ID
    std::optional<size_t> from_index;            // 裁剪起始索引
    std::optional<size_t> to_index;              // 裁剪结束索引
    std::optional<size_t> keep_last_n;           // 保留最后 N 条消息
};

// Snip 结果
struct SnipResult {
    size_t messages_removed{0};
    size_t tokens_freed{0};
    size_t remaining_messages{0};
    size_t remaining_tokens{0};
};

// 对话历史管理器
class ConversationHistory {
public:
    // 添加消息
    void push(Message msg) {
        total_tokens_ += msg.token_count;
        messages_.push_back(std::move(msg));
    }

    // 获取消息数量
    [[nodiscard]] size_t size() const { return messages_.size(); }
    [[nodiscard]] bool empty() const { return messages_.empty(); }
    [[nodiscard]] size_t total_tokens() const { return total_tokens_; }

    // 按 ID 查找消息索引
    auto find_index(std::string_view id) const -> std::optional<size_t> {
        for (size_t i = 0; i < messages_.size(); ++i) {
            if (messages_[i].id == id) return i;
        }
        return std::nullopt;
    }

    // 删除指定索引的消息
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

    // 从指定索引开始裁剪到末尾 (保留 [0, index) )
    auto trim_from(size_t index) -> std::expected<SnipResult, SnipError> {
        if (index >= messages_.size()) {
            return std::unexpected(SnipError::IndexOutOfRange);
        }

        SnipResult result;
        // 跳过受保护的消息
        for (size_t i = index; i < messages_.size(); ++i) {
            if (!messages_[i].is_protected) {
                result.tokens_freed += messages_[i].token_count;
                result.messages_removed++;
            }
        }

        // 执行裁剪 (保留受保护消息)
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

    // 重新计算 token 使用量
    auto recalculate() -> size_t {
        total_tokens_ = 0;
        for (const auto& msg : messages_) {
            total_tokens_ += msg.token_count;
        }
        return total_tokens_;
    }

    // 获取消息的只读访问
    [[nodiscard]] auto messages() const -> const std::vector<Message>& {
        return messages_;
    }

private:
    std::vector<Message> messages_;
    size_t total_tokens_{0};
};

// SnipTool - 历史/上下文裁剪工具
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
                // 压缩：合并相邻同角色消息 (简化实现)
                auto before_tokens = history_.total_tokens();
                // 实际实现需要对消息内容做摘要压缩
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
