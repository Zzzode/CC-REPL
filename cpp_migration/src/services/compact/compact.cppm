// Compact Service Module
module;
#include <expected>
#include <stdexcept>
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
};

// Constructor
CompactService::CompactService(CompactConfig config)
    : config_(std::move(config)) {}

// Perform full compaction
Result<CompactResult> CompactService::compact(std::vector<Message> messages) {
    CompactResult result;
    result.original_messages = messages;
    
    try {
        // Pre-compact hooks
        // In real implementation, run pre-compact hooks
        
        std::vector<Message> working_messages = std::move(messages);
        
        // Micro compact first if enabled
        if (config_.enable_micro_compact) {
            auto micro_result = micro_compact(working_messages);
            if (!micro_result) {
                return std::unexpected(micro_result.error());
            }
            working_messages = std::move(*micro_result);
        }
        
        // Main compact logic
        // In real implementation, call the API to compact messages
        
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
        // In real implementation, calculate actual tokens saved
        
        // Post-compact hooks
        // In real implementation, run post-compact hooks
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }
    
    return result;
}

// Micro compact
Result<std::vector<Message>> CompactService::micro_compact(std::vector<Message> messages) {
    // In real implementation, perform lightweight compaction
    // - Remove redundant messages
    // - Merge consecutive messages from the same user
    // - Etc.
    
    return messages;
}

// Post-compact cleanup
Result<std::vector<Message>> CompactService::post_compact_cleanup(std::vector<Message> messages) {
    // In real implementation, perform cleanup after compaction
    // - Validate the compacted messages
    // - Ensure proper message boundaries
    // - Etc.
    
    return messages;
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

} // namespace cc::services::compact
