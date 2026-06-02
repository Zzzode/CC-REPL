module;
#include <span>
#include <string>
#include <vector>
export module cc.services.compact.grouping;

export namespace cc::services::compact {

// Minimal message shape used across compact service grouping.
struct Message {
    std::string role;
    std::string content;
    int token_count{0};
};

// A group of messages that can be compacted together
struct MessageGroup {
    std::vector<int> message_indices;
    std::string summary_key;
    int token_count{0};
};

// Group messages by role/pattern for compact summarization
auto group_messages_for_compact(std::span<Message> messages) -> std::vector<MessageGroup> {
    std::vector<MessageGroup> groups;
    if (messages.empty()) return groups;

    MessageGroup current_group;
    std::string current_role = messages[0].role;

    for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
        // Start a new group when role changes or token count is large
        if (messages[i].role != current_role || current_group.token_count > 4000) {
            if (!current_group.message_indices.empty()) {
                current_group.summary_key = current_role + "_group";
                groups.push_back(std::move(current_group));
                current_group = MessageGroup{};
            }
            current_role = messages[i].role;
        }
        current_group.message_indices.push_back(i);
        current_group.token_count += messages[i].token_count;
    }

    // Push final group
    if (!current_group.message_indices.empty()) {
        current_group.summary_key = current_role + "_group";
        groups.push_back(std::move(current_group));
    }

    return groups;
}

// Estimate token savings from compacting a group
auto estimate_group_savings(const MessageGroup& group) -> int {
    // Estimate ~70% reduction for compacted groups
    return static_cast<int>(group.token_count * 0.7);
}

} // namespace cc::services::compact
