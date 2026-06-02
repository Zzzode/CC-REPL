module;
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.model.configs;

export namespace cc::utils {

struct ModelConfig {
    std::string model_id;
    float temperature;
    int max_tokens;
    std::optional<float> top_p;
    bool stream;
};

ModelConfig get_default_config(std::string_view model_id) {
    ModelConfig cfg;
    cfg.model_id = std::string(model_id);
    cfg.stream = true;

    if (model_id.find("opus") != std::string_view::npos) {
        cfg.temperature = 1.0f;
        cfg.max_tokens = 32768;
    } else if (model_id.find("haiku") != std::string_view::npos) {
        cfg.temperature = 1.0f;
        cfg.max_tokens = 8192;
    } else {
        // Default (sonnet and others)
        cfg.temperature = 1.0f;
        cfg.max_tokens = 16384;
    }

    cfg.top_p = std::nullopt;
    return cfg;
}

ModelConfig merge_config(ModelConfig base, ModelConfig override_) {
    ModelConfig result = base;

    if (!override_.model_id.empty()) {
        result.model_id = override_.model_id;
    }
    // Temperature of 0 is valid, so always override if model_id was set
    if (!override_.model_id.empty() || override_.temperature != 0.0f) {
        result.temperature = override_.temperature;
    }
    if (override_.max_tokens > 0) {
        result.max_tokens = override_.max_tokens;
    }
    if (override_.top_p.has_value()) {
        result.top_p = override_.top_p;
    }
    // stream is always taken from override
    result.stream = override_.stream;

    return result;
}

} // namespace cc::utils
