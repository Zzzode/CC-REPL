module;
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.env_validation;

export namespace cc::utils {

struct EnvIssue {
    std::string key;
    std::string message;
    enum class Severity { Warning, Error } severity;
};

// Forward declarations
std::vector<EnvIssue> check_required_env_vars();
std::vector<EnvIssue> check_conflicting_env_vars();

// Validate the overall environment for CC-REPL
std::vector<EnvIssue> validate_environment() {
    std::vector<EnvIssue> issues;

    // Combine required and conflicting checks
    auto required = check_required_env_vars();
    auto conflicts = check_conflicting_env_vars();

    issues.insert(issues.end(), required.begin(), required.end());
    issues.insert(issues.end(), conflicts.begin(), conflicts.end());

    return issues;
}

// Check that required environment variables are set
std::vector<EnvIssue> check_required_env_vars() {
    std::vector<EnvIssue> issues;

    // API key is required unless using bedrock/vertex
    const char* api_key = std::getenv("ANTHROPIC_API_KEY");
    const char* use_bedrock = std::getenv("CLAUDE_CODE_USE_BEDROCK");
    const char* use_vertex = std::getenv("CLAUDE_CODE_USE_VERTEX");

    if (!api_key && !use_bedrock && !use_vertex) {
        issues.push_back(EnvIssue{
            "ANTHROPIC_API_KEY",
            "No API key found. Set ANTHROPIC_API_KEY or configure a cloud provider.",
            EnvIssue::Severity::Error
        });
    }

    // Warn if both bedrock and vertex are set
    if (use_bedrock && use_vertex) {
        issues.push_back(EnvIssue{
            "CLAUDE_CODE_USE_BEDROCK",
            "Both Bedrock and Vertex are enabled. Only one provider should be configured.",
            EnvIssue::Severity::Error
        });
    }

    // Check bedrock dependencies
    if (use_bedrock && std::string_view(use_bedrock) == "1") {
        if (!std::getenv("AWS_REGION") && !std::getenv("AWS_DEFAULT_REGION")) {
            issues.push_back(EnvIssue{
                "AWS_REGION",
                "Bedrock is enabled but AWS_REGION is not set.",
                EnvIssue::Severity::Warning
            });
        }
    }

    return issues;
}

// Check for conflicting environment variables
std::vector<EnvIssue> check_conflicting_env_vars() {
    std::vector<EnvIssue> issues;

    // Check for conflicting model settings
    const char* model_env = std::getenv("CLAUDE_MODEL");
    const char* anthropic_model = std::getenv("ANTHROPIC_MODEL");

    if (model_env && anthropic_model && std::string_view(model_env) != std::string_view(anthropic_model)) {
        issues.push_back(EnvIssue{
            "CLAUDE_MODEL",
            "CLAUDE_MODEL and ANTHROPIC_MODEL are both set with different values. CLAUDE_MODEL takes precedence.",
            EnvIssue::Severity::Warning
        });
    }

    // Check for deprecated env vars
    if (std::getenv("CLAUDE_API_KEY")) {
        issues.push_back(EnvIssue{
            "CLAUDE_API_KEY",
            "CLAUDE_API_KEY is deprecated. Use ANTHROPIC_API_KEY instead.",
            EnvIssue::Severity::Warning
        });
    }

    return issues;
}

} // namespace cc::utils
