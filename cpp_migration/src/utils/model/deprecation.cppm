module;
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.model.deprecation;

export namespace cc::utils {

namespace detail {
    struct DeprecationEntry {
        std::string message;
        std::string replacement;
        std::chrono::system_clock::time_point deprecation_date;
    };

    inline std::chrono::system_clock::time_point make_date(int year, int month, int day) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        auto t = std::mktime(&tm);
        return std::chrono::system_clock::from_time_t(t);
    }

    inline const std::map<std::string, DeprecationEntry>& deprecated_models() {
        static const std::map<std::string, DeprecationEntry> m = {
            {"claude-3-opus-20240229", {
                "Claude 3 Opus is deprecated. Please use claude-opus-4-20250514.",
                "claude-opus-4-20250514",
                make_date(2025, 6, 1)
            }},
            {"claude-3-sonnet-20240229", {
                "Claude 3 Sonnet is deprecated. Please use claude-sonnet-4-20250514.",
                "claude-sonnet-4-20250514",
                make_date(2025, 3, 1)
            }},
            {"claude-3-haiku-20240307", {
                "Claude 3 Haiku is deprecated. Please use claude-haiku-4-20250514.",
                "claude-haiku-4-20250514",
                make_date(2025, 6, 1)
            }},
            {"claude-3-5-sonnet-20241022", {
                "Claude 3.5 Sonnet is deprecated. Please use claude-sonnet-4-20250514.",
                "claude-sonnet-4-20250514",
                make_date(2025, 8, 1)
            }},
        };
        return m;
    }
} // namespace detail

bool is_deprecated(std::string_view model_id) {
    return detail::deprecated_models().contains(std::string(model_id));
}

std::optional<std::string> get_deprecation_message(std::string_view model_id) {
    auto& m = detail::deprecated_models();
    auto it = m.find(std::string(model_id));
    if (it != m.end()) {
        return it->second.message;
    }
    return std::nullopt;
}

std::optional<std::string> get_replacement_model(std::string_view deprecated_id) {
    auto& m = detail::deprecated_models();
    auto it = m.find(std::string(deprecated_id));
    if (it != m.end()) {
        return it->second.replacement;
    }
    return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> get_deprecation_date(std::string_view model_id) {
    auto& m = detail::deprecated_models();
    auto it = m.find(std::string(model_id));
    if (it != m.end()) {
        return it->second.deprecation_date;
    }
    return std::nullopt;
}

} // namespace cc::utils
