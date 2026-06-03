module;
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.hooks.notifs.deprecation_warning;

export namespace cc::hooks::notifs {

inline bool is_deprecation_dismissed(std::string_view model);

namespace detail {
    // Models whose deprecation warning has been dismissed by the user.
    inline std::unordered_set<std::string>& dismissed_models() {
        static std::unordered_set<std::string> s;
        return s;
    }
} // namespace detail

// Check whether the current model set contains deprecated model names.
inline std::vector<std::string> check_deprecation_warnings() {
    // Known deprecated models that should trigger migration warnings
    static const std::vector<std::string> deprecated_models = {
        "fennec-latest",
        "fennec-fast-latest",
        "claude-opus-4-0",
        "claude-sonnet-4-5-20250929",
    };
    std::vector<std::string> warnings;
    for (const auto& model : deprecated_models) {
        if (!detail::dismissed_models().contains(model)) {
            warnings.push_back(model);
        }
    }
    return warnings;
}

// Show a model deprecation notification with a replacement hint.
inline void show_deprecation_notification(std::string_view model, std::string_view replacement) {
    if (is_deprecation_dismissed(model)) return;
    std::fprintf(stderr, "[WARNING] Model '%.*s' is deprecated. Please migrate to '%.*s'.\n",
                 static_cast<int>(model.size()), model.data(),
                 static_cast<int>(replacement.size()), replacement.data());
}

// Return whether a model deprecation warning has been dismissed.
inline bool is_deprecation_dismissed(std::string_view model) {
    return detail::dismissed_models().contains(std::string(model));
}

} // namespace cc::hooks::notifs
