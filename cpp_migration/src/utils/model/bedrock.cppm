module;
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.model.bedrock;

export namespace cc::utils {

struct BedrockConfig {
    std::string region;
    std::string profile;
    std::optional<std::string> endpoint;
};

std::optional<BedrockConfig> get_bedrock_config() {
    const char* region = std::getenv("AWS_REGION");
    const char* profile = std::getenv("AWS_PROFILE");

    if (!region && !profile) {
        return std::nullopt;
    }

    BedrockConfig cfg;
    cfg.region = region ? region : "us-east-1";
    cfg.profile = profile ? profile : "default";

    const char* endpoint = std::getenv("BEDROCK_ENDPOINT");
    if (endpoint) {
        cfg.endpoint = std::string(endpoint);
    }

    return cfg;
}

std::string get_bedrock_model_id(std::string_view model) {
    // Bedrock uses ARN-style identifiers
    if (model == "claude-sonnet-4-20250514") {
        return "anthropic.claude-sonnet-4-20250514-v1:0";
    }
    if (model == "claude-opus-4-20250514") {
        return "anthropic.claude-opus-4-20250514-v1:0";
    }
    if (model == "claude-haiku-4-20250514") {
        return "anthropic.claude-haiku-4-20250514-v1:0";
    }
    // Pass through unknown model ids
    return std::string(model);
}

bool is_bedrock_enabled() {
    const char* enabled = std::getenv("CLAUDE_CODE_USE_BEDROCK");
    return enabled && (std::string_view(enabled) == "1" || std::string_view(enabled) == "true");
}

} // namespace cc::utils
