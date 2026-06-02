module;

#include <cctype>
#include <map>
#include <string>
#include <string_view>

export module cc.utils.timeouts;

export namespace cc::utils::timeouts {

constexpr int default_timeout_ms = 120000;
constexpr int max_timeout_ms = 600000;

using EnvLike = std::map<std::string, std::string>;

namespace detail {
    [[nodiscard]] inline int parse_int_prefix(std::string_view value) noexcept {
        std::size_t i = 0;
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        bool negative = false;
        if (i < value.size() && (value[i] == '+' || value[i] == '-')) {
            negative = value[i] == '-';
            ++i;
        }
        if (i >= value.size() || !std::isdigit(static_cast<unsigned char>(value[i]))) return 0;
        long long out = 0;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            out = out * 10 + (value[i] - '0');
            if (out > 2147483647LL) return negative ? -2147483647 : 2147483647;
            ++i;
        }
        return static_cast<int>(negative ? -out : out);
    }

    [[nodiscard]] inline std::string get(const EnvLike& env, std::string_view key) {
        auto it = env.find(std::string(key));
        return it == env.end() ? std::string{} : it->second;
    }
} // namespace detail

[[nodiscard]] inline int get_default_bash_timeout_ms(const EnvLike& env) {
    const auto env_value = detail::get(env, "BASH_DEFAULT_TIMEOUT_MS");
    if (!env_value.empty()) {
        const int parsed = detail::parse_int_prefix(env_value);
        if (parsed > 0) return parsed;
    }
    return default_timeout_ms;
}

[[nodiscard]] inline int get_max_bash_timeout_ms(const EnvLike& env) {
    const auto env_value = detail::get(env, "BASH_MAX_TIMEOUT_MS");
    if (!env_value.empty()) {
        const int parsed = detail::parse_int_prefix(env_value);
        if (parsed > 0) return parsed > get_default_bash_timeout_ms(env) ? parsed : get_default_bash_timeout_ms(env);
    }
    const int def = get_default_bash_timeout_ms(env);
    return max_timeout_ms > def ? max_timeout_ms : def;
}

} // namespace cc::utils::timeouts
