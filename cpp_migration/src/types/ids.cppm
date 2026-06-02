/// @file ids.cppm
/// @brief ID type definitions and generators.
/// Migrated from src/types/ids.ts
module;

#include <string>
#include <random>
#include <chrono>

export module cc.types.ids;

export namespace cc::types {

/// Generate a random alphanumeric ID of given length
[[nodiscard]] inline std::string generate_id(std::size_t length = 12, std::string_view prefix = "") {
    static constexpr char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);
    
    std::string id(prefix);
    id.reserve(prefix.size() + length);
    for (std::size_t i = 0; i < length; ++i) {
        id += chars[dist(rng)];
    }
    return id;
}

/// Generate a message ID
[[nodiscard]] inline std::string generate_message_id() {
    return generate_id(24, "msg_");
}

/// Generate a tool use ID
[[nodiscard]] inline std::string generate_tool_use_id() {
    return generate_id(24, "toolu_");
}

/// Generate a session ID
[[nodiscard]] inline std::string generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "session_" + std::to_string(epoch) + "_" + generate_id(8);
}

/// Generate an agent ID
[[nodiscard]] inline std::string generate_agent_id() {
    return generate_id(8, "a");
}

} // namespace cc::types
