module;
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.model_support_overrides;

export namespace cc::utils {

// Forward declaration - ModelConfig is defined in configs.cppm
// For this module, we use a simplified local definition
struct ModelConfig {
    std::string model_id;
    float temperature;
    int max_tokens;
    std::optional<float> top_p;
    bool stream;
};

struct ModelOverride {
    std::string model_id;
    std::optional<std::size_t> max_tokens;
    std::optional<bool> disable_thinking;
};

std::vector<ModelOverride> get_overrides() {
    // Overrides can be loaded from config; these are hard-coded defaults
    return {
        // Example: limit haiku output for cost control in certain deployments
        ModelOverride{"claude-haiku-4-20250514", 4096, std::nullopt},
    };
}

ModelConfig apply_overrides(ModelConfig config, std::string_view model_id) {
    for (auto& override_ : get_overrides()) {
        if (override_.model_id == model_id) {
            if (override_.max_tokens.has_value()) {
                config.max_tokens = static_cast<int>(*override_.max_tokens);
            }
            if (override_.disable_thinking.has_value() && *override_.disable_thinking) {
                // When thinking is disabled, we don't modify config struct directly
                // but this signals the caller to skip thinking blocks
            }
            break;
        }
    }
    return config;
}

} // namespace cc::utils
