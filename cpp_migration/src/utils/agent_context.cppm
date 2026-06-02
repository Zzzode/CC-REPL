module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.agent_context;

export namespace cc::utils::agent_context {

struct AgentContextData {
    std::string agent_id;
    std::string working_dir;
    std::vector<std::string> loaded_files;
    std::optional<std::string> current_task;
};

struct StandaloneAgentConfig {
    std::string name;
    std::string model;
    std::vector<std::string> tools;
    bool headless{true};
};

inline std::expected<AgentContextData, std::string> get_agent_context([[maybe_unused]] std::string_view agent_id) {
    return AgentContextData{"", "", {}, std::nullopt};
}

inline std::expected<std::string, std::string> launch_standalone_agent([[maybe_unused]] const StandaloneAgentConfig& config) {
    return "";
}

inline std::expected<void, std::string> update_agent_context([[maybe_unused]] std::string_view agent_id, [[maybe_unused]] const AgentContextData& ctx) {
    return {};
}

inline std::vector<std::string> get_in_process_teammates() {
    return {};
}

inline std::expected<void, std::string> register_teammate_helper([[maybe_unused]] std::string_view agent_id) {
    return {};
}

} // namespace cc::utils::agent_context
