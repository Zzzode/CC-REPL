// Model Definitions - Claude model registry, configs, cost calculation
module;
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.services.api.models;


export namespace cc::services::api {

// API provider backends
enum class Provider {
    Anthropic,
    Bedrock,
    Vertex,
};

// Convert provider to display string
constexpr std::string_view provider_name(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "anthropic";
        case Provider::Bedrock: return "bedrock";
        case Provider::Vertex: return "vertex";
    }
    return "unknown";
}

// Feature capabilities for a model
struct ModelCapabilities {
    bool supports_thinking = false;
    bool supports_tool_use = true;
    bool supports_vision = true;
    bool supports_pdf = false;
    bool supports_extended_thinking = false;
    bool supports_citations = false;
    bool supports_computer_use = false;
    bool supports_batch = true;
};

// Pricing per million tokens (in USD cents)
struct ModelPricing {
    double input_per_mtok = 0.0;      // Cost per 1M input tokens
    double output_per_mtok = 0.0;     // Cost per 1M output tokens
    double cache_write_per_mtok = 0.0; // Cost for cache creation
    double cache_read_per_mtok = 0.0;  // Cost for cache hits
};

// Complete model configuration
struct ModelConfig {
    std::string id;                    // Model identifier string
    std::string display_name;          // Human-readable name
    int context_window = 200000;       // Max input context size
    int max_output_tokens = 4096;      // Default max output
    int absolute_max_output = 128000;  // Hard limit on output
    ModelCapabilities capabilities;
    ModelPricing pricing;
    Provider default_provider = Provider::Anthropic;
    std::string api_model_id;          // Override for provider-specific ID
};

// Known model identifiers
enum class ModelId {
    ClaudeSonnet4_20250514,
    ClaudeOpus4_20250514,
    ClaudeSonnet3_5_20241022,
    ClaudeHaiku3_5_20241022,
    ClaudeSonnet3_5_20240620,
};

// Model registry - all supported models with configurations
class ModelRegistry {
public:
    ModelRegistry() {
        register_models();
    }

    // Resolve a model string to its configuration
    [[nodiscard]] std::expected<ModelConfig, std::string> resolve(std::string_view model_str) const {
        // Direct match
        auto it = models_.find(std::string(model_str));
        if (it != models_.end()) return it->second;

        // Try alias lookup
        auto alias_it = aliases_.find(std::string(model_str));
        if (alias_it != aliases_.end()) {
            auto model_it = models_.find(alias_it->second);
            if (model_it != models_.end()) return model_it->second;
        }

        // Partial match - find models containing the search string
        for (const auto& [id, config] : models_) {
            if (id.contains(model_str)) return config;
        }

        return std::unexpected(std::format("Unknown model: {}", model_str));
    }

    // Get all registered models
    [[nodiscard]] std::vector<ModelConfig> all_models() const {
        std::vector<ModelConfig> result;
        result.reserve(models_.size());
        for (const auto& [_, config] : models_) {
            result.push_back(config);
        }
        return result;
    }

    // Get models filtered by capability
    [[nodiscard]] std::vector<ModelConfig> models_with_thinking() const {
        return filter_models([](const ModelConfig& m) { return m.capabilities.supports_thinking; });
    }

    [[nodiscard]] std::vector<ModelConfig> models_with_vision() const {
        return filter_models([](const ModelConfig& m) { return m.capabilities.supports_vision; });
    }

    // Get the default model for general use
    [[nodiscard]] const ModelConfig& default_model() const {
        return models_.at("claude-sonnet-4-20250514");
    }

private:
    void register_models() {
        // Claude Sonnet 4 (latest)
        register_model(ModelConfig{
            .id = "claude-sonnet-4-20250514",
            .display_name = "Claude Sonnet 4",
            .context_window = 200000,
            .max_output_tokens = 16384,
            .absolute_max_output = 128000,
            .capabilities = {
                .supports_thinking = true,
                .supports_tool_use = true,
                .supports_vision = true,
                .supports_pdf = true,
                .supports_extended_thinking = true,
                .supports_citations = true,
                .supports_computer_use = true,
                .supports_batch = true,
            },
            .pricing = {
                .input_per_mtok = 300,
                .output_per_mtok = 1500,
                .cache_write_per_mtok = 375,
                .cache_read_per_mtok = 30,
            },
            .api_model_id = {},
        });
        aliases_["sonnet"] = "claude-sonnet-4-20250514";
        aliases_["claude-sonnet"] = "claude-sonnet-4-20250514";

        // Claude Opus 4
        register_model(ModelConfig{
            .id = "claude-opus-4-20250514",
            .display_name = "Claude Opus 4",
            .context_window = 200000,
            .max_output_tokens = 16384,
            .absolute_max_output = 128000,
            .capabilities = {
                .supports_thinking = true,
                .supports_tool_use = true,
                .supports_vision = true,
                .supports_pdf = true,
                .supports_extended_thinking = true,
                .supports_citations = true,
                .supports_computer_use = true,
                .supports_batch = true,
            },
            .pricing = {
                .input_per_mtok = 1500,
                .output_per_mtok = 7500,
                .cache_write_per_mtok = 1875,
                .cache_read_per_mtok = 150,
            },
            .api_model_id = {},
        });
        aliases_["opus"] = "claude-opus-4-20250514";
        aliases_["claude-opus"] = "claude-opus-4-20250514";

        // Claude 3.5 Sonnet (October 2024)
        register_model(ModelConfig{
            .id = "claude-3-5-sonnet-20241022",
            .display_name = "Claude 3.5 Sonnet (Oct 2024)",
            .context_window = 200000,
            .max_output_tokens = 8192,
            .absolute_max_output = 8192,
            .capabilities = {
                .supports_thinking = false,
                .supports_tool_use = true,
                .supports_vision = true,
                .supports_pdf = true,
                .supports_extended_thinking = false,
                .supports_citations = false,
                .supports_computer_use = true,
                .supports_batch = true,
            },
            .pricing = {
                .input_per_mtok = 300,
                .output_per_mtok = 1500,
                .cache_write_per_mtok = 375,
                .cache_read_per_mtok = 30,
            },
            .api_model_id = {},
        });
        aliases_["sonnet-3.5"] = "claude-3-5-sonnet-20241022";

        // Claude 3.5 Haiku
        register_model(ModelConfig{
            .id = "claude-3-5-haiku-20241022",
            .display_name = "Claude 3.5 Haiku",
            .context_window = 200000,
            .max_output_tokens = 8192,
            .absolute_max_output = 8192,
            .capabilities = {
                .supports_thinking = false,
                .supports_tool_use = true,
                .supports_vision = false,
                .supports_pdf = false,
                .supports_extended_thinking = false,
                .supports_citations = false,
                .supports_computer_use = false,
                .supports_batch = true,
            },
            .pricing = {
                .input_per_mtok = 100,
                .output_per_mtok = 500,
                .cache_write_per_mtok = 125,
                .cache_read_per_mtok = 10,
            },
            .api_model_id = {},
        });
        aliases_["haiku"] = "claude-3-5-haiku-20241022";

        // Claude 3.5 Sonnet (June 2024)
        register_model(ModelConfig{
            .id = "claude-3-5-sonnet-20240620",
            .display_name = "Claude 3.5 Sonnet (Jun 2024)",
            .context_window = 200000,
            .max_output_tokens = 4096,
            .absolute_max_output = 4096,
            .capabilities = {
                .supports_thinking = false,
                .supports_tool_use = true,
                .supports_vision = true,
                .supports_pdf = false,
                .supports_extended_thinking = false,
                .supports_citations = false,
                .supports_computer_use = false,
                .supports_batch = true,
            },
            .pricing = {
                .input_per_mtok = 300,
                .output_per_mtok = 1500,
                .cache_write_per_mtok = 375,
                .cache_read_per_mtok = 30,
            },
            .api_model_id = {},
        });
    }

    void register_model(ModelConfig config) {
        auto id = config.id;
        models_.emplace(std::move(id), std::move(config));
    }

    // Filter models by predicate
    template<typename Pred>
    [[nodiscard]] std::vector<ModelConfig> filter_models(Pred&& pred) const {
        std::vector<ModelConfig> result;
        for (const auto& [_, config] : models_) {
            if (pred(config)) result.push_back(config);
        }
        return result;
    }

    std::unordered_map<std::string, ModelConfig> models_;
    std::unordered_map<std::string, std::string> aliases_;
};

// Calculate cost for a given token usage
struct CostCalculator {
    // Calculate total cost in USD for token usage on a specific model
    [[nodiscard]] static double calculate(
        const ModelPricing& pricing,
        int input_tokens,
        int output_tokens,
        int cache_write_tokens = 0,
        int cache_read_tokens = 0) {
        double cost = 0.0;
        cost += (static_cast<double>(input_tokens) / 1'000'000.0) * pricing.input_per_mtok;
        cost += (static_cast<double>(output_tokens) / 1'000'000.0) * pricing.output_per_mtok;
        cost += (static_cast<double>(cache_write_tokens) / 1'000'000.0) * pricing.cache_write_per_mtok;
        cost += (static_cast<double>(cache_read_tokens) / 1'000'000.0) * pricing.cache_read_per_mtok;
        return cost / 100.0; // Convert from cents to dollars
    }

    // Format cost as human-readable string
    [[nodiscard]] static std::string format_cost(double cost_usd) {
        if (cost_usd < 0.01) {
            return std::format("${:.4f}", cost_usd);
        }
        return std::format("${:.2f}", cost_usd);
    }
};

// Provider-specific base URL resolution
[[nodiscard]] inline std::string resolve_base_url(Provider provider, const std::string& region = "") {
    switch (provider) {
        case Provider::Anthropic:
            return "https://api.anthropic.com";
        case Provider::Bedrock:
            return std::format("https://bedrock-runtime.{}.amazonaws.com", 
                region.empty() ? "us-east-1" : region);
        case Provider::Vertex:
            return std::format("https://{}-aiplatform.googleapis.com",
                region.empty() ? "us-east5" : region);
    }
    return "https://api.anthropic.com";
}

// Get provider-specific model ID mapping
[[nodiscard]] inline std::string provider_model_id(
    const std::string& model_id, Provider provider) {
    if (provider == Provider::Anthropic) return model_id;
    if (provider == Provider::Bedrock) {
        return std::format("anthropic.{}", model_id);
    }
    if (provider == Provider::Vertex) {
        return model_id; // Vertex uses same model IDs
    }
    return model_id;
}

} // namespace cc::services::api
