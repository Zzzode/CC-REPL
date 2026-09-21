module;
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.model_options;

export namespace cc::utils {

struct ModelOptions {
    std::optional<float> temperature;
    std::optional<int> max_tokens;
    std::optional<float> top_p;
    std::optional<std::vector<std::string>> stop_sequences;
};

ModelOptions parse_model_options(const std::map<std::string, std::string>& opts) {
    ModelOptions result;

    if (auto it = opts.find("temperature"); it != opts.end()) {
        result.temperature = std::stof(it->second);
    }
    if (auto it = opts.find("max_tokens"); it != opts.end()) {
        result.max_tokens = std::stoi(it->second);
    }
    if (auto it = opts.find("top_p"); it != opts.end()) {
        result.top_p = std::stof(it->second);
    }
    if (auto it = opts.find("stop"); it != opts.end()) {
        // Parse comma-separated stop sequences
        std::vector<std::string> seqs;
        std::string_view sv = it->second;
        std::size_t pos = 0;
        while (pos < sv.size()) {
            auto comma = sv.find(',', pos);
            if (comma == std::string_view::npos) {
                seqs.emplace_back(sv.substr(pos));
                break;
            }
            seqs.emplace_back(sv.substr(pos, comma - pos));
            pos = comma + 1;
        }
        result.stop_sequences = std::move(seqs);
    }

    return result;
}

std::vector<std::string> validate_options(const ModelOptions& opts, std::string_view model_id) {
    std::vector<std::string> errors;

    if (opts.temperature.has_value()) {
        float t = *opts.temperature;
        if (t < 0.0f || t > 2.0f) {
            errors.push_back("temperature must be between 0.0 and 2.0");
        }
    }

    if (opts.max_tokens.has_value()) {
        int max = *opts.max_tokens;
        if (max <= 0) {
            errors.push_back("max_tokens must be positive");
        }
        // Check against model limits
        int model_max = 16384;
        if (model_id.find("opus") != std::string_view::npos) model_max = 32768;
        if (model_id.find("haiku") != std::string_view::npos) model_max = 8192;
        if (max > model_max) {
            errors.push_back("max_tokens exceeds model limit of " + std::to_string(model_max));
        }
    }

    if (opts.top_p.has_value()) {
        float p = *opts.top_p;
        if (p < 0.0f || p > 1.0f) {
            errors.push_back("top_p must be between 0.0 and 1.0");
        }
    }

    return errors;
}

} // namespace cc::utils
