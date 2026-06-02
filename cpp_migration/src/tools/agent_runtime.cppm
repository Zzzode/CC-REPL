module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>

export module cc.tools.agent_runtime;

export namespace cc::tools::agent_runtime {

struct AgentRuntimeConfig {
    std::string agent_id;
    std::string working_dir;
    std::vector<std::string> capabilities;
    std::optional<std::string> parent_agent_id;
    bool allow_fork{true};
};

struct AgentExecutionResult {
    std::string agent_id;
    int exit_code;
    std::string output;
    std::optional<std::string> error;
};

enum class AgentLifecycle {
    Starting,
    Running,
    Suspended,
    Completed,
    Failed
};

inline std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config) {
    return AgentExecutionResult{config.agent_id, 0, "", std::nullopt};
}

inline std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config) {
    return std::string(config.agent_id);
}

inline std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id) {
    return AgentExecutionResult{std::string(agent_id), 0, "", std::nullopt};
}

inline std::expected<std::vector<std::string>, std::string> load_agents_from_dir(std::string_view dir_path) {
    return std::vector<std::string>{};
}

inline AgentLifecycle get_agent_lifecycle(std::string_view agent_id) {
    return AgentLifecycle::Completed;
}

} // namespace cc::tools::agent_runtime
