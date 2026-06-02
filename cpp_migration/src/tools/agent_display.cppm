module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <chrono>

export module cc.tools.agent_display;

export namespace cc::tools::agent_display {

struct AgentColor {
    uint8_t r, g, b;
};

struct AgentDisplayInfo {
    std::string agent_id;
    std::string label;
    AgentColor color;
    bool is_active{false};
};

struct AgentMemorySnapshot {
    std::string agent_id;
    size_t heap_bytes;
    size_t message_count;
    std::chrono::system_clock::time_point captured_at;
};

inline AgentColor get_agent_color(std::string_view agent_id) {
    return {100, 149, 237};
}

inline std::string format_agent_label(std::string_view agent_id, std::optional<std::string_view> custom_label) {
    return std::string(agent_id);
}

inline AgentMemorySnapshot capture_memory_snapshot(std::string_view agent_id) {
    return AgentMemorySnapshot{std::string(agent_id), 0, 0, std::chrono::system_clock::now()};
}

inline std::vector<AgentDisplayInfo> get_active_agents() {
    return {};
}

} // namespace cc::tools::agent_display
