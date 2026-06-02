module;

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

export module cc.utils.agent_swarms_enabled;

export namespace cc::utils::agent_swarms_enabled {

struct AgentSwarmsGateInput {
    std::string user_type;
    bool env_opt_in = false;
    bool flag_set = false;
    bool growthbook_enabled = true;
};

[[nodiscard]] inline bool is_env_truthy(std::string_view value) {
    std::string normalized(value);
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    normalized.erase(normalized.begin(), std::find_if(normalized.begin(), normalized.end(), not_space));
    normalized.erase(std::find_if(normalized.rbegin(), normalized.rend(), not_space).base(), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

[[nodiscard]] inline bool is_agent_swarms_enabled(const AgentSwarmsGateInput& input) noexcept {
    if (input.user_type == "ant") return true;
    if (!input.env_opt_in && !input.flag_set) return false;
    if (!input.growthbook_enabled) return false;
    return true;
}

} // namespace cc::utils::agent_swarms_enabled
