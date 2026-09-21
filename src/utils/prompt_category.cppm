module;
#include <optional>
#include <set>
#include <string>
#include <string_view>

export module cc.utils.prompt_category;

export namespace cc::utils::prompt_category {

inline constexpr std::string_view default_output_style_name = "default";

[[nodiscard]] inline std::string get_query_source_for_agent(
    std::optional<std::string_view> agent_type,
    bool is_built_in_agent
) {
    if (is_built_in_agent) {
        if (agent_type.has_value() && !agent_type->empty()) {
            return "agent:builtin:" + std::string(*agent_type);
        }
        return "agent:default";
    }
    return "agent:custom";
}

[[nodiscard]] inline std::string get_query_source_for_agent(
    std::string_view agent_type,
    bool is_built_in_agent
) {
    return get_query_source_for_agent(std::optional<std::string_view>{agent_type}, is_built_in_agent);
}

[[nodiscard]] inline std::string get_query_source_for_agent(
    const char* agent_type,
    bool is_built_in_agent
) {
    return get_query_source_for_agent(std::string_view(agent_type == nullptr ? "" : agent_type), is_built_in_agent);
}

[[nodiscard]] inline std::string get_query_source_for_repl(
    std::string_view output_style,
    const std::set<std::string>& built_in_output_styles,
    std::string_view default_style = default_output_style_name
) {
    if (output_style == default_style) {
        return "repl_main_thread";
    }
    if (built_in_output_styles.contains(std::string(output_style))) {
        return "repl_main_thread:outputStyle:" + std::string(output_style);
    }
    return "repl_main_thread:outputStyle:custom";
}

} // namespace cc::utils::prompt_category
