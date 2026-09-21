module;
#include <map>
#include <string>
#include <string_view>

export module cc.utils.script_tool_enabled;

import cc.utils.env_utils;

export namespace cc::utils::script_tool {

using EnvLike = std::map<std::string, std::string>;

namespace detail {

[[nodiscard]] inline std::string_view get_env_value(const EnvLike& env, std::string_view key) {
    const auto it = env.find(std::string(key));
    return it == env.end() ? std::string_view{} : std::string_view(it->second);
}

} // namespace detail

[[nodiscard]] inline bool is_script_tool_enabled(const EnvLike& env) {
    return cc::utils::is_env_truthy(detail::get_env_value(env, "ENABLE_SCRIPT_TOOL"));
}

[[nodiscard]] inline bool is_bash_tool_disabled(const EnvLike& env) {
    return cc::utils::is_env_truthy(detail::get_env_value(env, "DISABLE_BASH_TOOL")) || is_script_tool_enabled(env);
}

} // namespace cc::utils::script_tool
