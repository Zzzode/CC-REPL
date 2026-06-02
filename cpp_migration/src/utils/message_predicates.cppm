module;

#include <string>

export module cc.utils.message_predicates;

export namespace cc::utils::message_predicates {

struct MessageLike {
    std::string type;
    bool is_meta = false;
    bool has_tool_use_result = false;
};

[[nodiscard]] inline bool is_human_turn(const MessageLike& message) noexcept {
    return message.type == "user" && !message.is_meta && !message.has_tool_use_result;
}

} // namespace cc::utils::message_predicates
