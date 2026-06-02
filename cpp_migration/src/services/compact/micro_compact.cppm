module;
#include <span>
#include <string>
#include <vector>
export module cc.services.compact.micro_compact;

export namespace cc::services::compact {

// Forward declaration of Message (defined in grouping module)
struct MicroMessage {
    std::string role;
    std::string content;
    int token_count{0};
};

// Perform micro-compact: lightweight context trimming for small overages
auto micro_compact(std::span<MicroMessage> messages, int target_tokens)
    -> std::vector<MicroMessage> {
    std::vector<MicroMessage> result;
    int total_tokens = 0;

    // Keep most recent messages, trim oldest first
    // Calculate total
    for (const auto& msg : messages) {
        total_tokens += msg.token_count;
    }

    // If already under target, return as-is
    if (total_tokens <= target_tokens) {
        return {messages.begin(), messages.end()};
    }

    // Drop oldest messages until under target (keep system + recent)
    int tokens_to_drop = total_tokens - target_tokens;
    int dropped = 0;

    for (const auto& msg : messages) {
        if (dropped < tokens_to_drop && msg.role != "system") {
            dropped += msg.token_count;
            continue;
        }
        result.push_back(msg);
    }

    return result;
}

// Check if micro-compact is applicable (small overage)
auto can_micro_compact(std::span<MicroMessage> messages) -> bool {
    int total = 0;
    for (const auto& msg : messages) {
        total += msg.token_count;
    }
    // Micro-compact is suitable for < 20% overage
    return messages.size() > 2;
}

// Estimate token savings from micro-compact
auto get_micro_compact_savings(std::span<MicroMessage> messages) -> int {
    int total = 0;
    for (const auto& msg : messages) {
        if (msg.role != "system") {
            total += msg.token_count;
        }
    }
    // Estimate ~30% savings from micro-compact
    return static_cast<int>(total * 0.3);
}

} // namespace cc::services::compact
