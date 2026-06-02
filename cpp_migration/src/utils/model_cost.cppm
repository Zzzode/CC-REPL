module;

#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <string>

export module cc.utils.model_cost;

export namespace cc::utils::model_cost {

struct ModelCosts {
    double input_tokens = 0;
    double output_tokens = 0;
    double prompt_cache_write_tokens = 0;
    double prompt_cache_read_tokens = 0;
    double web_search_requests = 0;
};

struct Usage {
    double input_tokens = 0;
    double output_tokens = 0;
    double cache_read_input_tokens = 0;
    double cache_creation_input_tokens = 0;
    double web_search_requests = 0;
    std::optional<std::string> speed;
};

inline constexpr ModelCosts COST_TIER_3_15{3, 15, 3.75, 0.3, 0.01};
inline constexpr ModelCosts COST_TIER_15_75{15, 75, 18.75, 1.5, 0.01};
inline constexpr ModelCosts COST_TIER_5_25{5, 25, 6.25, 0.5, 0.01};
inline constexpr ModelCosts COST_TIER_30_150{30, 150, 37.5, 3, 0.01};
inline constexpr ModelCosts COST_HAIKU_35{0.8, 4, 1, 0.08, 0.01};
inline constexpr ModelCosts COST_HAIKU_45{1, 5, 1.25, 0.1, 0.01};

[[nodiscard]] inline std::string canonical_model_name(std::string model) {
    if (model.find("haiku-3-5") != std::string::npos || model.find("3-5-haiku") != std::string::npos) return "claude-haiku-3-5";
    if (model.find("haiku-4-5") != std::string::npos || model.find("4-5-haiku") != std::string::npos) return "claude-haiku-4-5";
    if (model.find("sonnet-3-5") != std::string::npos || model.find("3-5-sonnet") != std::string::npos) return "claude-sonnet-3-5";
    if (model.find("sonnet-3-7") != std::string::npos || model.find("3-7-sonnet") != std::string::npos) return "claude-sonnet-3-7";
    if (model.find("sonnet-4-6") != std::string::npos || model.find("4-6-sonnet") != std::string::npos) return "claude-sonnet-4-6";
    if (model.find("sonnet-4-5") != std::string::npos || model.find("4-5-sonnet") != std::string::npos) return "claude-sonnet-4-5";
    if (model.find("sonnet-4") != std::string::npos || model.find("4-sonnet") != std::string::npos) return "claude-sonnet-4";
    if (model.find("opus-4-6") != std::string::npos || model.find("4-6-opus") != std::string::npos) return "claude-opus-4-6";
    if (model.find("opus-4-5") != std::string::npos || model.find("4-5-opus") != std::string::npos) return "claude-opus-4-5";
    if (model.find("opus-4-1") != std::string::npos || model.find("4-1-opus") != std::string::npos) return "claude-opus-4-1";
    if (model.find("opus-4") != std::string::npos || model.find("4-opus") != std::string::npos) return "claude-opus-4";
    return model;
}

[[nodiscard]] inline const std::map<std::string, ModelCosts>& model_costs() {
    static const std::map<std::string, ModelCosts> costs = {
        {"claude-haiku-3-5", COST_HAIKU_35},
        {"claude-haiku-4-5", COST_HAIKU_45},
        {"claude-sonnet-3-5", COST_TIER_3_15},
        {"claude-sonnet-3-7", COST_TIER_3_15},
        {"claude-sonnet-4", COST_TIER_3_15},
        {"claude-sonnet-4-5", COST_TIER_3_15},
        {"claude-sonnet-4-6", COST_TIER_3_15},
        {"claude-opus-4", COST_TIER_15_75},
        {"claude-opus-4-1", COST_TIER_15_75},
        {"claude-opus-4-5", COST_TIER_5_25},
        {"claude-opus-4-6", COST_TIER_5_25},
    };
    return costs;
}

[[nodiscard]] inline ModelCosts get_opus_46_cost_tier(bool fast_mode, bool global_fast_mode_enabled = true) {
    return global_fast_mode_enabled && fast_mode ? COST_TIER_30_150 : COST_TIER_5_25;
}

[[nodiscard]] inline ModelCosts get_model_costs(
    std::string model,
    const Usage& usage = {},
    bool global_fast_mode_enabled = true,
    std::string default_main_loop_model = "claude-sonnet-4-5"
) {
    const auto short_name = canonical_model_name(model);
    if (short_name == "claude-opus-4-6") return get_opus_46_cost_tier(usage.speed == "fast", global_fast_mode_enabled);
    const auto& costs = model_costs();
    auto it = costs.find(short_name);
    if (it != costs.end()) return it->second;
    auto fallback = costs.find(canonical_model_name(std::move(default_main_loop_model)));
    if (fallback != costs.end()) return fallback->second;
    return COST_TIER_5_25;
}

[[nodiscard]] inline double tokens_to_usd_cost(const ModelCosts& costs, const Usage& usage) {
    return (usage.input_tokens / 1'000'000.0) * costs.input_tokens +
           (usage.output_tokens / 1'000'000.0) * costs.output_tokens +
           (usage.cache_read_input_tokens / 1'000'000.0) * costs.prompt_cache_read_tokens +
           (usage.cache_creation_input_tokens / 1'000'000.0) * costs.prompt_cache_write_tokens +
           usage.web_search_requests * costs.web_search_requests;
}

[[nodiscard]] inline double calculate_usd_cost(
    std::string model,
    const Usage& usage,
    bool global_fast_mode_enabled = true,
    std::string default_main_loop_model = "claude-sonnet-4-5"
) {
    return tokens_to_usd_cost(get_model_costs(std::move(model), usage, global_fast_mode_enabled, std::move(default_main_loop_model)), usage);
}

[[nodiscard]] inline double calculate_cost_from_tokens(
    std::string model,
    double input_tokens,
    double output_tokens,
    double cache_read_input_tokens,
    double cache_creation_input_tokens,
    std::string default_main_loop_model = "claude-sonnet-4-5"
) {
    return calculate_usd_cost(std::move(model), Usage{
        .input_tokens = input_tokens,
        .output_tokens = output_tokens,
        .cache_read_input_tokens = cache_read_input_tokens,
        .cache_creation_input_tokens = cache_creation_input_tokens,
        .web_search_requests = 0,
        .speed = std::nullopt,
    }, true, std::move(default_main_loop_model));
}

[[nodiscard]] inline std::string format_price(double price) {
    if (std::floor(price) == price) return "$" + std::to_string(static_cast<long long>(price));
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << '$' << price;
    return out.str();
}

[[nodiscard]] inline std::string format_model_pricing(const ModelCosts& costs) {
    return format_price(costs.input_tokens) + "/" + format_price(costs.output_tokens) + " per Mtok";
}

[[nodiscard]] inline std::optional<std::string> get_model_pricing_string(std::string model) {
    const auto short_name = canonical_model_name(std::move(model));
    const auto& costs = model_costs();
    auto it = costs.find(short_name);
    if (it == costs.end()) return std::nullopt;
    return format_model_pricing(it->second);
}

} // namespace cc::utils::model_cost
