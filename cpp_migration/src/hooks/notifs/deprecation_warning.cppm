module;
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

export module cc.hooks.notifs.deprecation_warning;

export namespace cc::hooks::notifs {

namespace detail {
    // 已被用户关闭的废弃警告集合
    inline std::unordered_set<std::string>& dismissed_models() {
        static std::unordered_set<std::string> s;
        return s;
    }
} // namespace detail

// 检查当前是否存在废弃警告（如已废弃的模型名）
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

// 展示模型废弃通知，提示用户迁移到替代模型
inline void show_deprecation_notification(std::string_view model, std::string_view replacement) {
    if (is_deprecation_dismissed(model)) return;
    std::fprintf(stderr, "[WARNING] Model '%.*s' is deprecated. Please migrate to '%.*s'.\n",
                 static_cast<int>(model.size()), model.data(),
                 static_cast<int>(replacement.size()), replacement.data());
}

// 判断某个模型的废弃通知是否已被用户关闭
inline bool is_deprecation_dismissed(std::string_view model) {
    return detail::dismissed_models().contains(std::string(model));
}

} // namespace cc::hooks::notifs
