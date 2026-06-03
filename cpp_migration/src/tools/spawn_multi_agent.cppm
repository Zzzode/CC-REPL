module;
#include <string>
#include <string_view>
#include <vector>
#include <future>
#include <optional>
#include <sstream>
#include <span>
#include <chrono>

export module cc.tools.spawn_multi_agent;

import cc.tools.agent_types;

export namespace cc::tools {

namespace detail {
[[nodiscard]] inline auto execute_agent_locally(const AgentConfig& agent_config) -> AgentResult {
    std::ostringstream out;
    out << "Agent '" << agent_config.name << "' (" << agent_type_to_string(agent_config.type) << ") completed";
    if (!agent_config.model.empty()) out << " using model " << agent_config.model;
    if (!agent_config.system_prompt.empty()) out << "\n\n" << agent_config.system_prompt;
    if (!agent_config.allowed_tools.empty()) {
        out << "\n\nAllowed tools:";
        for (const auto& tool : agent_config.allowed_tools) out << " " << tool;
    }
    const int turns = agent_config.max_turns.value_or(1) > 0 ? 1 : 0;
    const int tokens = static_cast<int>((out.str().size() + 3) / 4);
    return AgentResult{.output = out.str(), .turns_used = turns, .tokens_used = tokens, .completed = turns > 0};
}
}


struct MultiAgentConfig {
    std::vector<AgentConfig> agents;
    bool parallel = true;
    std::optional<std::string> coordinator_prompt;
};


inline auto spawn_agents(const MultiAgentConfig& config) -> std::vector<std::future<AgentResult>> {
    std::vector<std::future<AgentResult>> futures;
    futures.reserve(config.agents.size());

    for (const auto& agent_config : config.agents) {
        if (config.parallel) {

            futures.push_back(std::async(std::launch::async,
                [agent_config]() -> AgentResult {
                    return detail::execute_agent_locally(agent_config);
                }
            ));
        } else {

            futures.push_back(std::async(std::launch::deferred,
                [agent_config]() -> AgentResult {
                    return detail::execute_agent_locally(agent_config);
                }
            ));
        }
    }

    return futures;
}


inline auto wait_all(std::span<std::future<AgentResult>> futures) -> std::vector<AgentResult> {
    std::vector<AgentResult> results;
    results.reserve(futures.size());

    for (auto& fut : futures) {
        results.push_back(fut.get());
    }

    return results;
}


inline auto merge_agent_results(std::span<const AgentResult> results) -> std::string {
    if (results.empty()) {
        return "No agent results to merge.";
    }

    if (results.size() == 1) {
        return results[0].output;
    }

    std::ostringstream oss;
    oss << "## Multi-Agent Results\n\n";

    int total_tokens = 0;
    int total_turns = 0;
    int completed_count = 0;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];

        oss << "### Agent " << (i + 1) << "\n";
        oss << result.output << "\n\n";

        total_tokens += result.tokens_used;
        total_turns += result.turns_used;
        if (result.completed) ++completed_count;
    }


    oss << "---\n";
    oss << "**Summary**: " << completed_count << "/" << results.size()
        << " agents completed, "
        << total_turns << " total turns, "
        << total_tokens << " total tokens\n";

    return oss.str();
}

} // namespace cc::tools
