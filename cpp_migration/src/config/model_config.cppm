module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.config.model_config;


export namespace cc::core {


enum class ModelProvider { anthropic, bedrock, vertex, openai_compat, foundry };


struct ModelCapabilities {
    size_t context_window{200'000};
    size_t max_output_tokens{16'384};
    bool supports_vision{true};
    bool supports_tools{true};
    bool supports_extended_thinking{false};
    bool supports_computer_use{false};
    bool supports_streaming{true};
    bool supports_caching{true};
};


struct ModelCost {
    double input_per_mtok{3.0};
    double output_per_mtok{15.0};
    double cache_read_per_mtok{0.3};
    double cache_write_per_mtok{3.75};
};


struct ModelEndpoint {
    ModelProvider provider{ModelProvider::anthropic};
    std::string base_url{"https://api.anthropic.com"};
    std::string api_key_env{"ANTHROPIC_API_KEY"};
    std::string region;
    std::string model_id;
};


struct ModelAlias {
    std::string alias;
    std::string resolved_model_id;
};


class ModelRegistry {
    std::string default_model_{"claude-sonnet-4-20250514"};
    std::string current_model_;
    std::vector<ModelAlias> aliases_;
    std::unordered_map<std::string, ModelEndpoint> endpoints_;
    std::unordered_map<std::string, ModelCapabilities> capabilities_;
    std::unordered_map<std::string, ModelCost> costs_;
    std::unordered_map<std::string, std::vector<std::string>> fallback_chains_;

public:
    ModelRegistry() { init_defaults(); }


    [[nodiscard]] auto resolve_alias(std::string_view alias_or_id) const -> std::string {
        for (const auto& a : aliases_) {
            if (a.alias == alias_or_id) return a.resolved_model_id;
        }
        return std::string(alias_or_id);
    }


    [[nodiscard]] auto get_endpoint(std::string_view model_id) const -> std::optional<ModelEndpoint> {
        auto resolved = resolve_alias(model_id);
        if (auto it = endpoints_.find(resolved); it != endpoints_.end())
            return it->second;

        ModelEndpoint endpoint;
        endpoint.provider = ModelProvider::anthropic;
        endpoint.model_id = std::move(resolved);
        return endpoint;
    }


    [[nodiscard]] auto get_capabilities(std::string_view model_id) const -> ModelCapabilities {
        auto resolved = resolve_alias(model_id);
        if (auto it = capabilities_.find(resolved); it != capabilities_.end())
            return it->second;
        return ModelCapabilities{};
    }


    [[nodiscard]] auto get_cost(std::string_view model_id) const -> ModelCost {
        auto resolved = resolve_alias(model_id);
        if (auto it = costs_.find(resolved); it != costs_.end())
            return it->second;
        return ModelCost{};
    }


    [[nodiscard]] auto get_default_model() const -> std::string_view { return default_model_; }
    [[nodiscard]] auto get_current_model() const -> std::string_view {
        return current_model_.empty() ? default_model_ : current_model_;
    }
    void set_model(std::string model_id) { current_model_ = std::move(model_id); }


    [[nodiscard]] auto list_available_models() const -> std::vector<std::string> {
        std::vector<std::string> models;
        models.reserve(endpoints_.size());
        for (const auto& [id, _] : endpoints_) models.push_back(id);
        std::sort(models.begin(), models.end());
        return models;
    }


    void add_alias(std::string alias, std::string model_id) {
        aliases_.push_back({.alias = std::move(alias), .resolved_model_id = std::move(model_id)});
    }


    [[nodiscard]] auto get_provider(std::string_view model_id) const -> ModelProvider {
        if (auto ep = get_endpoint(model_id)) return ep->provider;
        return ModelProvider::anthropic;
    }


    [[nodiscard]] auto get_fallback_chain(std::string_view model_id) const -> std::vector<std::string> {
        auto resolved = resolve_alias(model_id);
        if (auto it = fallback_chains_.find(resolved); it != fallback_chains_.end())
            return it->second;
        return {};
    }

private:
    void init_defaults() {

        aliases_ = {
            {"sonnet", "claude-sonnet-4-20250514"},
            {"opus", "claude-opus-4-20250514"},
            {"haiku", "claude-3-5-haiku-20241022"},
            {"sonnet-3.5", "claude-3-5-sonnet-20241022"},
        };


        capabilities_["claude-sonnet-4-20250514"] = {
            .context_window = 200'000, .max_output_tokens = 16'384,
            .supports_vision = true, .supports_tools = true,
            .supports_extended_thinking = true, .supports_computer_use = true
        };
        costs_["claude-sonnet-4-20250514"] = {
            .input_per_mtok = 3.0, .output_per_mtok = 15.0,
            .cache_read_per_mtok = 0.3, .cache_write_per_mtok = 3.75
        };

        // Claude 4 Opus
        capabilities_["claude-opus-4-20250514"] = {
            .context_window = 200'000, .max_output_tokens = 32'768,
            .supports_vision = true, .supports_tools = true,
            .supports_extended_thinking = true, .supports_computer_use = true
        };
        costs_["claude-opus-4-20250514"] = {
            .input_per_mtok = 15.0, .output_per_mtok = 75.0,
            .cache_read_per_mtok = 1.5, .cache_write_per_mtok = 18.75
        };

        // Claude 3.5 Haiku
        capabilities_["claude-3-5-haiku-20241022"] = {
            .context_window = 200'000, .max_output_tokens = 8'192,
            .supports_vision = true, .supports_tools = true,
            .supports_extended_thinking = false
        };
        costs_["claude-3-5-haiku-20241022"] = {
            .input_per_mtok = 1.0, .output_per_mtok = 5.0,
            .cache_read_per_mtok = 0.1, .cache_write_per_mtok = 1.25
        };

        // Fallback chains
        fallback_chains_["claude-sonnet-4-20250514"] = {"claude-3-5-sonnet-20241022"};
        fallback_chains_["claude-opus-4-20250514"] = {"claude-sonnet-4-20250514"};
    }
};

} // namespace cc::core
