module;
#include <string>
#include <string_view>

export module cc.utils.model.agent;

export namespace cc::utils {

// Default agent model identifier
inline constexpr std::string_view kDefaultAgentModel = "claude-sonnet-4-20250514";

// Mapping of task types to preferred models
std::string get_agent_model() {
    return std::string(kDefaultAgentModel);
}

std::string get_agent_model_for_task(std::string_view task_type) {
    // Route complex tasks to more capable models
    if (task_type == "coding" || task_type == "analysis" || task_type == "architecture") {
        return "claude-sonnet-4-20250514";
    }
    if (task_type == "simple" || task_type == "classification" || task_type == "extraction") {
        return "claude-haiku-4-20250514";
    }
    if (task_type == "creative" || task_type == "research" || task_type == "complex_reasoning") {
        return "claude-opus-4-20250514";
    }
    // Default to sonnet for unrecognized task types
    return std::string(kDefaultAgentModel);
}

bool is_agent_model(std::string_view model_id) {
    // Agent models are the ones used for sub-agent spawning
    return model_id == "claude-sonnet-4-20250514" ||
           model_id == "claude-haiku-4-20250514" ||
           model_id == "claude-opus-4-20250514";
}

} // namespace cc::utils
