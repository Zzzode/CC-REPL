module;
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <future>
#include <optional>
#include <sstream>
#include <span>
#include <chrono>
#include <system_error>
#include <utility>

export module cc.tools.spawn_multi_agent;

import cc.tools.agent;
import cc.tools.agent_types;
import cc.tools.tool;

export namespace cc::tools {

namespace detail {
[[nodiscard]] inline std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

[[nodiscard]] inline std::string agent_type_to_subagent_type(
    AgentType type,
    std::string_view fallback_name = {}
) {
    switch (type) {
        case AgentType::Explore: return "Explore";
        case AgentType::Plan: return "Plan";
        case AgentType::Verify: return "verification";
        case AgentType::GeneralPurpose: return "general-purpose";
        case AgentType::Custom:
            return fallback_name.empty() ? std::string{"general-purpose"} : std::string{fallback_name};
    }
    return "general-purpose";
}

[[nodiscard]] inline std::string agent_prompt(
    const MultiAgentAgentConfig& agent_config,
    const std::optional<std::string>& coordinator_prompt
) {
    if (coordinator_prompt && !coordinator_prompt->empty()) return *coordinator_prompt;
    return "Run the assigned multi-agent task as '" + agent_config.name + "'.";
}

[[nodiscard]] inline std::string tool_result_text(const cc::core::ToolResult& result) {
    std::string text;
    for (const auto& block : result.content) {
        if (!text.empty()) text += "\n";
        text += block.text;
    }
    return text;
}

[[nodiscard]] inline auto failed_agent_result(std::string message) -> MultiAgentResult {
    const auto tokens = static_cast<int>((message.size() + 3) / 4);
    return MultiAgentResult{.output = std::move(message), .turns_used = 0, .tokens_used = tokens, .completed = false};
}

[[nodiscard]] inline auto execute_agent_tool_background(
    const MultiAgentAgentConfig& agent_config,
    std::string prompt,
    std::string working_dir
) -> MultiAgentResult {
    cc::tools::agent::AgentConfig runtime_config{};
    runtime_config.max_turns = agent_config.max_turns.value_or(runtime_config.max_turns);
    if (!agent_config.model.empty()) runtime_config.default_model = agent_config.model;
    runtime_config.allowed_tools = agent_config.allowed_tools;
    const auto subagent_type = agent_type_to_subagent_type(agent_config.type, agent_config.name);
    if (!runtime_config.allowed_tools.empty()) {
        runtime_config.allowed_tools.push_back("Agent(" + subagent_type + ")");
    }

    cc::tools::agent::AgentTool tool(runtime_config);
    std::ostringstream input;
    input << R"({"prompt":")" << json_escape(prompt)
        << R"(","subagent_type":")" << json_escape(subagent_type)
        << R"(","run_in_background":true)";
    if (!agent_config.name.empty()) {
        input << R"(,"name":")" << json_escape(agent_config.name) << '"';
    }
    if (!agent_config.model.empty()) {
        input << R"(,"model":")" << json_escape(agent_config.model) << '"';
    }
    if (!working_dir.empty()) {
        input << R"(,"cwd":")" << json_escape(working_dir) << '"';
    }
    input << '}';

    auto result = tool.execute(cc::core::ToolInput::from_json(input.str()));
    if (!result) {
        auto error = result.error().format();
        return failed_agent_result(std::move(error));
    }
    auto output = tool_result_text(*result);
    const int tokens = static_cast<int>((output.size() + 3) / 4);
    return MultiAgentResult{
        .output = std::move(output),
        .turns_used = 0,
        .tokens_used = tokens,
        .completed = !result->is_error,
    };
}

[[nodiscard]] inline std::string resolve_working_dir(std::string configured) {
    if (!configured.empty()) return configured;
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) return cwd.string();
    return ".";
}

[[nodiscard]] inline std::string teammate_prompt(
    const MultiAgentAgentConfig& agent_config,
    const std::optional<std::string>& coordinator_prompt
) {
    if (coordinator_prompt && !coordinator_prompt->empty()) return *coordinator_prompt;
    return "You have been spawned as teammate '" + agent_config.name + "'.";
}

[[nodiscard]] inline auto spawn_teammate_agent(
    const MultiAgentAgentConfig& agent_config,
    std::string_view team_name,
    std::string prompt,
    std::string working_dir,
    bool prefer_in_process
) -> MultiAgentResult {
    if (agent_config.name.empty()) {
        return failed_agent_result("Agent name is required for teammate spawn.");
    }
    if (team_name.empty()) {
        return failed_agent_result("Team name is required for teammate spawn.");
    }

    cc::tools::agent::AgentConfig runtime_config{};
    runtime_config.max_turns = agent_config.max_turns.value_or(runtime_config.max_turns);
    if (!agent_config.model.empty()) runtime_config.default_model = agent_config.model;
    runtime_config.allowed_tools = agent_config.allowed_tools;
    runtime_config.prefer_in_process_teammate = prefer_in_process;
    const auto subagent_type = agent_type_to_subagent_type(agent_config.type, agent_config.name);
    if (!runtime_config.allowed_tools.empty()) {
        runtime_config.allowed_tools.push_back("Agent(" + subagent_type + ")");
    }

    cc::tools::agent::AgentTool tool(runtime_config);
    std::ostringstream input;
    input << R"({"prompt":")" << json_escape(prompt)
        << R"(","subagent_type":")" << json_escape(subagent_type)
        << R"(","run_in_background":true)"
        << R"(,"name":")" << json_escape(agent_config.name)
        << R"(","team_name":")" << json_escape(team_name) << '"';
    if (!agent_config.model.empty()) {
        input << R"(,"model":")" << json_escape(agent_config.model) << '"';
    }
    if (agent_config.type == AgentType::Plan) {
        input << R"(,"mode":"plan")";
    }
    auto cwd = resolve_working_dir(std::move(working_dir));
    if (!cwd.empty()) {
        input << R"(,"cwd":")" << json_escape(cwd) << '"';
    }
    input << '}';

    auto result = tool.execute(cc::core::ToolInput::from_json(input.str()));
    if (!result) {
        auto error = result.error().format();
        return failed_agent_result(std::move(error));
    }
    auto output = tool_result_text(*result);
    const auto tokens = static_cast<int>((output.size() + 3) / 4);
    return MultiAgentResult{.output = std::move(output), .turns_used = 0, .tokens_used = tokens, .completed = !result->is_error};
}
}


struct MultiAgentConfig {
    std::vector<MultiAgentAgentConfig> agents;
    bool parallel = true;
    std::optional<std::string> coordinator_prompt;
    std::optional<std::string> team_name;
    std::string working_dir;
    bool prefer_in_process = false;
};


inline auto spawn_agents(const MultiAgentConfig& config) -> std::vector<std::future<MultiAgentResult>> {
    std::vector<std::future<MultiAgentResult>> futures;
    futures.reserve(config.agents.size());

    for (const auto& agent_config : config.agents) {
        if (config.team_name && !config.team_name->empty()) {
            auto prompt = detail::teammate_prompt(agent_config, config.coordinator_prompt);
            if (config.parallel) {
                futures.push_back(std::async(std::launch::async,
                    [agent_config,
                     team_name = *config.team_name,
                     prompt = std::move(prompt),
                     working_dir = config.working_dir,
                     prefer_in_process = config.prefer_in_process]() mutable -> MultiAgentResult {
                        return detail::spawn_teammate_agent(
                            agent_config,
                            team_name,
                            std::move(prompt),
                            std::move(working_dir),
                            prefer_in_process
                        );
                    }
                ));
            } else {
                futures.push_back(std::async(std::launch::deferred,
                    [agent_config,
                     team_name = *config.team_name,
                     prompt = std::move(prompt),
                     working_dir = config.working_dir,
                     prefer_in_process = config.prefer_in_process]() mutable -> MultiAgentResult {
                        return detail::spawn_teammate_agent(
                            agent_config,
                            team_name,
                            std::move(prompt),
                            std::move(working_dir),
                            prefer_in_process
                        );
                    }
                ));
            }
            continue;
        }

        if (config.parallel) {

            futures.push_back(std::async(std::launch::async,
                [agent_config, prompt = detail::agent_prompt(agent_config, config.coordinator_prompt), working_dir = config.working_dir]() mutable -> MultiAgentResult {
                    return detail::execute_agent_tool_background(agent_config, std::move(prompt), std::move(working_dir));
                }
            ));
        } else {

            futures.push_back(std::async(std::launch::deferred,
                [agent_config, prompt = detail::agent_prompt(agent_config, config.coordinator_prompt), working_dir = config.working_dir]() mutable -> MultiAgentResult {
                    return detail::execute_agent_tool_background(agent_config, std::move(prompt), std::move(working_dir));
                }
            ));
        }
    }

    return futures;
}


inline auto wait_all(std::span<std::future<MultiAgentResult>> futures) -> std::vector<MultiAgentResult> {
    std::vector<MultiAgentResult> results;
    results.reserve(futures.size());

    for (auto& fut : futures) {
        results.push_back(fut.get());
    }

    return results;
}


inline auto merge_agent_results(std::span<const MultiAgentResult> results) -> std::string {
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
