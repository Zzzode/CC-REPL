module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

export module cc.utils.model_aliases;


export namespace cc::utils {


enum class ModelFamily { claude3, claude3_5, claude4 };


enum class ModelProvider { anthropic, bedrock, vertex, openai_compat };


struct ModelInfo {
    std::string_view id;
    ModelFamily family;
    std::string_view display_name;
    size_t context_window;
    size_t max_output;
    ModelProvider provider_hint{ModelProvider::anthropic};
    bool supports_vision{true};
};


inline constexpr std::array kAliases = {
    std::pair<std::string_view, std::string_view>{"sonnet", "claude-sonnet-4-20250514"},
    std::pair<std::string_view, std::string_view>{"opus", "claude-opus-4-20250514"},
    std::pair<std::string_view, std::string_view>{"haiku", "claude-3-5-haiku-20241022"},
    std::pair<std::string_view, std::string_view>{"sonnet-3.5", "claude-3-5-sonnet-20241022"},
    std::pair<std::string_view, std::string_view>{"sonnet-4", "claude-sonnet-4-20250514"},
    std::pair<std::string_view, std::string_view>{"opus-4", "claude-opus-4-20250514"},
    std::pair<std::string_view, std::string_view>{"fast", "claude-3-5-haiku-20241022"},
};


inline const std::array<ModelInfo, 5> kModels = {{
    {"claude-sonnet-4-20250514", ModelFamily::claude4, "Claude Sonnet 4", 200'000, 16'384},
    {"claude-opus-4-20250514", ModelFamily::claude4, "Claude Opus 4", 200'000, 32'768},
    {"claude-3-5-sonnet-20241022", ModelFamily::claude3_5, "Claude 3.5 Sonnet", 200'000, 8'192},
    {"claude-3-5-haiku-20241022", ModelFamily::claude3_5, "Claude 3.5 Haiku", 200'000, 8'192},
    {"claude-3-opus-20240229", ModelFamily::claude3, "Claude 3 Opus", 200'000, 4'096},
}};


[[nodiscard]] inline auto resolve_alias(std::string_view alias_or_id) -> std::string_view {
    for (const auto& [alias, model_id] : kAliases) {
        if (alias == alias_or_id) return model_id;
    }
    return alias_or_id;
}


[[nodiscard]] inline auto get_model_info(std::string_view model_id) -> std::optional<ModelInfo> {
    auto resolved = resolve_alias(model_id);
    for (const auto& info : kModels) {
        if (info.id == resolved) return info;
    }
    return std::nullopt;
}


[[nodiscard]] inline auto list_aliases() -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {kAliases.begin(), kAliases.end()};
}


[[nodiscard]] inline auto detect_provider(std::string_view model_id) -> ModelProvider {
    if (model_id.find("bedrock") != std::string_view::npos) return ModelProvider::bedrock;
    if (model_id.find("vertex") != std::string_view::npos) return ModelProvider::vertex;
    if (model_id.starts_with("claude")) return ModelProvider::anthropic;
    return ModelProvider::openai_compat;
}


[[nodiscard]] inline auto get_context_window(std::string_view model_id) -> size_t {
    if (auto info = get_model_info(model_id)) return info->context_window;
    return 100'000;
}


[[nodiscard]] inline auto is_vision_capable(std::string_view model_id) -> bool {
    if (auto info = get_model_info(model_id)) return info->supports_vision;
    return false;
}

} // namespace cc::utils
