// Compact Types Module
module;
#include <optional>
#include <string>
#include <vector>

export module cc.services.compact.types;

import cc.utils.error;

export namespace cc::services::compact {

using cc::utils::Result;

// Message types for compact service
enum class MessageType {
    User,
    Assistant,
    System,
    CompactBoundary,
    // Add more types
};

struct Message {
    MessageType type;
    std::string content;
    std::string id;
    // Add more fields
};

// Compact result
struct CompactResult {
    std::vector<Message> original_messages;
    std::vector<Message> compacted_messages;
    int token_saved = 0;
    bool success = false;
    std::optional<std::string> error;
};

// Compact config
struct CompactConfig {
    int max_output_tokens = 4096;
    bool enable_micro_compact = true;
    bool enable_post_compact_cleanup = true;
    // Add more config fields
};

} // namespace cc::services::compact
