module;
#include <map>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.privacy_level;

export namespace cc::utils::privacy {

enum class PrivacyLevel {
    Default,
    NoTelemetry,
    EssentialTraffic,
};

using EnvLike = std::map<std::string, std::string>;

namespace detail {

[[nodiscard]] inline bool has_truthy_js_env_value(const EnvLike& env, std::string_view key) {
    const auto it = env.find(std::string(key));
    return it != env.end() && !it->second.empty();
}

} // namespace detail

[[nodiscard]] inline PrivacyLevel get_privacy_level(const EnvLike& env) {
    if (detail::has_truthy_js_env_value(env, "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC")) {
        return PrivacyLevel::EssentialTraffic;
    }
    if (detail::has_truthy_js_env_value(env, "DISABLE_TELEMETRY")) {
        return PrivacyLevel::NoTelemetry;
    }
    return PrivacyLevel::Default;
}

[[nodiscard]] inline bool is_essential_traffic_only(const EnvLike& env) {
    return get_privacy_level(env) == PrivacyLevel::EssentialTraffic;
}

[[nodiscard]] inline bool is_telemetry_disabled(const EnvLike& env) {
    return get_privacy_level(env) != PrivacyLevel::Default;
}

[[nodiscard]] inline std::optional<std::string> get_essential_traffic_only_reason(const EnvLike& env) {
    if (detail::has_truthy_js_env_value(env, "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC")) {
        return std::string("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC");
    }
    return std::nullopt;
}

} // namespace cc::utils::privacy
