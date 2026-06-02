// C++23 Agent Model Utilities Module
// Provides agent model selection and configuration utilities
module;

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.agent_model;

import cc.utils.model_aliases;
import cc.utils.string;

export namespace cc::utils::agent_model {

using cc::utils::ToLower;
using cc::utils::ToUpper;
using cc::utils::Contains;
using cc::utils::resolve_alias;

// Agent model options
constexpr std::array<std::string_view, 5> AGENT_MODEL_OPTIONS = {
    "sonnet",
    "opus",
    "haiku",
    "best",
    "inherit"
};

// Get default subagent model
[[nodiscard]] inline auto get_default_subagent_model() -> std::string {
    return "inherit";
}

// Check if alias matches parent model tier
[[nodiscard]] inline auto alias_matches_parent_tier(std::string_view alias, std::string_view parent_model) -> bool {
    auto canonical = resolve_alias(parent_model);
    auto alias_lower = ToLower(alias);

    if (alias_lower == "opus" && Contains(canonical, "opus")) {
        return true;
    }
    if (alias_lower == "sonnet" && Contains(canonical, "sonnet")) {
        return true;
    }
    if (alias_lower == "haiku" && Contains(canonical, "haiku")) {
        return true;
    }
    return false;
}

// Agent model option descriptor
struct AgentModelOption {
    std::string_view value;
    std::string_view label;
    std::string_view description;
};

[[nodiscard]] inline auto get_agent_model_options() -> std::vector<AgentModelOption> {
    return {
        {"sonnet", "Sonnet", "Balanced performance - best for most agents"},
        {"opus", "Opus", "Most capable for complex reasoning tasks"},
        {"haiku", "Haiku", "Fast and efficient for simple tasks"},
        {"inherit", "Inherit from parent", "Use the same model as the main conversation"}
    };
}

// Get display name for agent model
[[nodiscard]] inline auto get_agent_model_display(std::optional<std::string_view> model) -> std::string {
    if (!model || *model == "inherit") {
        return "Inherit from parent";
    }
    return ToUpper(std::string(*model).substr(0, 1)) + std::string(*model).substr(1);
}

// Configuration for resolving effective agent model
struct GetAgentModelOptions {
    std::optional<std::string_view> agent_model;
    std::string_view parent_model;
    std::optional<std::string_view> tool_specified_model;
    std::optional<std::string_view> permission_mode;
};

[[nodiscard]] inline auto get_agent_model(const GetAgentModelOptions& options) -> std::string {
    if (const char* env = std::getenv("CLAUDE_CODE_SUBAGENT_MODEL")) {
        return std::string(env);
    }

    if (options.tool_specified_model) {
        if (alias_matches_parent_tier(*options.tool_specified_model, options.parent_model)) {
            return std::string(options.parent_model);
        }
        return std::string(resolve_alias(*options.tool_specified_model));
    }

    auto agent_model = options.agent_model.value_or(get_default_subagent_model());

    if (agent_model == "inherit") {
        return std::string(options.parent_model);
    }

    if (alias_matches_parent_tier(agent_model, options.parent_model)) {
        return std::string(options.parent_model);
    }

    return std::string(resolve_alias(agent_model));
}

} // namespace cc::utils::agent_model
