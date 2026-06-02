module;
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.model;

export namespace cc::utils {

namespace detail {
    // Module-level state for current model
    inline std::string& current_model_ref() {
        static std::string model = "claude-sonnet-4-20250514";
        return model;
    }

    inline const std::vector<std::string>& known_models() {
        static const std::vector<std::string> models = {
            "claude-sonnet-4-20250514",
            "claude-opus-4-20250514",
            "claude-haiku-4-20250514",
        };
        return models;
    }
} // namespace detail

std::string get_current_model() {
    return detail::current_model_ref();
}

bool set_current_model(std::string_view model_id) {
    // Only allow setting to known models
    for (auto& m : detail::known_models()) {
        if (m == model_id) {
            detail::current_model_ref() = std::string(model_id);
            return true;
        }
    }
    return false;
}

bool validate_model_id(std::string_view model_id) {
    // Valid model IDs follow pattern: claude-{family}-{version}-{date}
    if (model_id.empty()) return false;
    if (!model_id.starts_with("claude-")) return false;

    for (auto& m : detail::known_models()) {
        if (m == model_id) return true;
    }
    return false;
}

std::string get_model_display_name(std::string_view model_id) {
    if (model_id.find("sonnet") != std::string_view::npos) return "Claude Sonnet 4";
    if (model_id.find("opus") != std::string_view::npos) return "Claude Opus 4";
    if (model_id.find("haiku") != std::string_view::npos) return "Claude Haiku 4";
    return std::string(model_id);
}

} // namespace cc::utils
