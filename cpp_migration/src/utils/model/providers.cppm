module;
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

export module cc.utils.model.providers;

export namespace cc::utils {

enum class Provider {
    Anthropic,
    Bedrock,
    Vertex,
    Custom
};

Provider detect_provider(std::string_view model_id) {
    // Bedrock models use anthropic. prefix
    if (model_id.starts_with("anthropic.")) return Provider::Bedrock;
    // Vertex models may have publishers/ prefix
    if (model_id.find("publishers/") != std::string_view::npos) return Provider::Vertex;
    // Check environment for custom provider
    const char* custom = std::getenv("CLAUDE_CODE_PROVIDER");
    if (custom) {
        std::string_view cv(custom);
        if (cv == "bedrock") return Provider::Bedrock;
        if (cv == "vertex") return Provider::Vertex;
        if (cv == "custom") return Provider::Custom;
    }
    // Default to Anthropic for claude-* models
    return Provider::Anthropic;
}

std::string get_api_base_url(Provider provider) {
    switch (provider) {
        case Provider::Anthropic:
            return "https://api.anthropic.com";
        case Provider::Bedrock: {
            const char* region = std::getenv("AWS_REGION");
            std::string r = region ? region : "us-east-1";
            return "https://bedrock-runtime." + r + ".amazonaws.com";
        }
        case Provider::Vertex: {
            const char* project = std::getenv("GOOGLE_CLOUD_PROJECT");
            const char* region = std::getenv("GOOGLE_CLOUD_REGION");
            std::string p = project ? project : "my-project";
            std::string reg = region ? region : "us-east5";
            return "https://" + reg + "-aiplatform.googleapis.com/v1/projects/" + p;
        }
        case Provider::Custom: {
            const char* base = std::getenv("ANTHROPIC_BASE_URL");
            return base ? std::string(base) : "http://localhost:8080";
        }
    }
    return "https://api.anthropic.com";
}

std::pair<std::string, std::string> get_auth_header(Provider provider) {
    switch (provider) {
        case Provider::Anthropic: {
            const char* key = std::getenv("ANTHROPIC_API_KEY");
            return {"x-api-key", key ? std::string(key) : ""};
        }
        case Provider::Bedrock:
            // Bedrock uses AWS SigV4, not a simple header
            return {"Authorization", ""};
        case Provider::Vertex:
            // Vertex uses OAuth2 bearer token
            return {"Authorization", "Bearer "};
        case Provider::Custom: {
            const char* key = std::getenv("ANTHROPIC_API_KEY");
            return {"x-api-key", key ? std::string(key) : ""};
        }
    }
    return {"", ""};
}

bool is_provider_configured(Provider provider) {
    switch (provider) {
        case Provider::Anthropic:
            return std::getenv("ANTHROPIC_API_KEY") != nullptr;
        case Provider::Bedrock:
            return std::getenv("AWS_REGION") != nullptr ||
                   std::getenv("AWS_PROFILE") != nullptr;
        case Provider::Vertex:
            return std::getenv("GOOGLE_CLOUD_PROJECT") != nullptr;
        case Provider::Custom:
            return std::getenv("ANTHROPIC_BASE_URL") != nullptr;
    }
    return false;
}

} // namespace cc::utils
