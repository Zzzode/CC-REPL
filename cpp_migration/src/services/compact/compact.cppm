// Compact Service Module
module;
#include <expected>
#include <algorithm>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.compact.compact;

import cc.utils.error;
import cc.services.compact.types;

export namespace cc::services::compact {

using cc::utils::Result;
using cc::services::compact::Message;
using cc::services::compact::CompactResult;
using cc::services::compact::CompactConfig;

// Compact service
class CompactService {
public:
    explicit CompactService(CompactConfig config);
    
    // Perform full compaction on a list of messages
    Result<CompactResult> compact(std::vector<Message> messages);
    
    // Perform micro compaction (lightweight compaction)
    Result<std::vector<Message>> micro_compact(std::vector<Message> messages);
    
    // Post-compact cleanup
    Result<std::vector<Message>> post_compact_cleanup(std::vector<Message> messages);
    
    // Get the messages after the last compact boundary
    static std::vector<Message> get_messages_after_compact_boundary(const std::vector<Message>& messages);
    
private:
    CompactConfig config_;
    static int estimate_tokens(const Message& message);
    static int estimate_tokens(const std::vector<Message>& messages);
    static std::string message_type_name(MessageType type);
    static Message build_summary_message(const std::vector<Message>& messages, int original_tokens);
};

// Constructor
CompactService::CompactService(CompactConfig config)
    : config_(std::move(config)) {}

// Perform full compaction
Result<CompactResult> CompactService::compact(std::vector<Message> messages) {
    CompactResult result;
    result.original_messages = messages;
    
    try {
        const auto original_tokens = estimate_tokens(result.original_messages);
        std::vector<Message> working_messages = std::move(messages);
        
        // Micro compact first if enabled
        if (config_.enable_micro_compact) {
            auto micro_result = micro_compact(working_messages);
            if (!micro_result) {
                return std::unexpected(micro_result.error());
            }
            working_messages = std::move(*micro_result);
        }
        
        const auto target_tokens = config_.max_output_tokens > 0 ? config_.max_output_tokens : 4096;
        if (estimate_tokens(working_messages) > target_tokens && working_messages.size() > 3) {
            std::vector<Message> preserved;
            std::vector<Message> summarized;

            for (const auto& message : working_messages) {
                if (message.type == MessageType::System) {
                    preserved.push_back(message);
                } else {
                    summarized.push_back(message);
                }
            }

            const auto keep_tail = std::min<std::size_t>(summarized.size(), 6);
            std::vector<Message> next_messages;
            next_messages.reserve(preserved.size() + keep_tail + 1);
            next_messages.insert(next_messages.end(), preserved.begin(), preserved.end());

            const auto summary_count = summarized.size() > keep_tail ? summarized.size() - keep_tail : 0;
            if (summary_count > 0) {
                std::vector<Message> summary_source(
                    summarized.begin(),
                    summarized.begin() + static_cast<std::ptrdiff_t>(summary_count));
                next_messages.push_back(build_summary_message(summary_source, estimate_tokens(summary_source)));
            }
            next_messages.insert(
                next_messages.end(),
                summarized.end() - static_cast<std::ptrdiff_t>(keep_tail),
                summarized.end());
            working_messages = std::move(next_messages);
        }
        
        // Post-compact cleanup if enabled
        if (config_.enable_post_compact_cleanup) {
            auto cleanup_result = post_compact_cleanup(working_messages);
            if (!cleanup_result) {
                return std::unexpected(cleanup_result.error());
            }
            working_messages = std::move(*cleanup_result);
        }
        
        result.compacted_messages = std::move(working_messages);
        result.success = true;
        result.token_saved = std::max(0, original_tokens - estimate_tokens(result.compacted_messages));
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }
    
    return result;
}

// Micro compact
Result<std::vector<Message>> CompactService::micro_compact(std::vector<Message> messages) {
    std::vector<Message> compacted;
    compacted.reserve(messages.size());

    for (auto& message : messages) {
        if (message.content.empty() && message.type != MessageType::CompactBoundary) {
            continue;
        }
        if (!compacted.empty() &&
            compacted.back().type == message.type &&
            message.type != MessageType::CompactBoundary &&
            compacted.back().content.size() + message.content.size() < 16 * 1024) {
            compacted.back().content += "\n\n";
            compacted.back().content += message.content;
            if (!message.id.empty()) {
                compacted.back().id += compacted.back().id.empty() ? message.id : "," + message.id;
            }
            continue;
        }
        compacted.push_back(std::move(message));
    }

    return compacted;
}

// Post-compact cleanup
Result<std::vector<Message>> CompactService::post_compact_cleanup(std::vector<Message> messages) {
    std::vector<Message> cleaned;
    cleaned.reserve(messages.size());
    for (auto& message : messages) {
        if (message.type != MessageType::CompactBoundary && message.content.empty()) {
            continue;
        }
        if (message.type == MessageType::CompactBoundary &&
            !cleaned.empty() &&
            cleaned.back().type == MessageType::CompactBoundary) {
            cleaned.back().content += "\n\n";
            cleaned.back().content += message.content;
            continue;
        }
        cleaned.push_back(std::move(message));
    }
    return cleaned;
}

// Get messages after compact boundary
std::vector<Message> CompactService::get_messages_after_compact_boundary(const std::vector<Message>& messages) {
    std::vector<Message> result;
    bool found_boundary = false;
    
    for (const auto& msg : messages) {
        if (msg.type == MessageType::CompactBoundary) {
            found_boundary = true;
            result.clear();
        } else if (found_boundary) {
            result.push_back(msg);
        }
    }
    
    return result;
}

int CompactService::estimate_tokens(const Message& message) {
    return std::max(1, static_cast<int>(message.content.size() / 4));
}

int CompactService::estimate_tokens(const std::vector<Message>& messages) {
    int total = 0;
    for (const auto& message : messages) {
        total += estimate_tokens(message);
    }
    return total;
}

std::string CompactService::message_type_name(MessageType type) {
    switch (type) {
        case MessageType::User: return "user";
        case MessageType::Assistant: return "assistant";
        case MessageType::System: return "system";
        case MessageType::CompactBoundary: return "compact_boundary";
    }
    return "unknown";
}

Message CompactService::build_summary_message(const std::vector<Message>& messages, int original_tokens) {
    std::string content = std::format(
        "Compacted conversation segment ({} messages, approximately {} tokens before compaction).\n",
        messages.size(),
        original_tokens);
    for (const auto& message : messages) {
        auto excerpt = message.content.substr(0, std::min<std::size_t>(message.content.size(), 320));
        if (message.content.size() > excerpt.size()) {
            excerpt += "...";
        }
        content += std::format(
            "- [{}{}] {}\n",
            message_type_name(message.type),
            message.id.empty() ? "" : ":" + message.id,
            excerpt);
    }
    return Message{
        .type = MessageType::CompactBoundary,
        .content = std::move(content),
        .id = "compact-summary",
    };
}

} // namespace cc::services::compact
