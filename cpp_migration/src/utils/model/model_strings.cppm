module;
#include <string>
#include <string_view>

export module cc.utils.model.model_strings;

export namespace cc::utils {

// Return human-friendly short name (e.g., "Sonnet 4")
std::string get_friendly_name(std::string_view model_id) {
    if (model_id.find("sonnet-4") != std::string_view::npos) return "Sonnet 4";
    if (model_id.find("opus-4") != std::string_view::npos) return "Opus 4";
    if (model_id.find("haiku-4") != std::string_view::npos) return "Haiku 4";
    if (model_id.find("sonnet") != std::string_view::npos) return "Sonnet";
    if (model_id.find("opus") != std::string_view::npos) return "Opus";
    if (model_id.find("haiku") != std::string_view::npos) return "Haiku";
    return std::string(model_id);
}

// Return the provider name based on model ID prefix
std::string get_provider_name(std::string_view model_id) {
    if (model_id.starts_with("claude-")) return "Anthropic";
    if (model_id.starts_with("anthropic.")) return "Bedrock";
    if (model_id.find("vertex") != std::string_view::npos) return "Vertex AI";
    return "Unknown";
}

// Format a colored badge for terminal UI display
std::string format_model_badge(std::string_view model_id) {
    std::string name = get_friendly_name(model_id);

    // ANSI escape: bold + color based on model family
    std::string color;
    if (model_id.find("opus") != std::string_view::npos) {
        color = "\033[1;35m"; // Bold magenta for Opus
    } else if (model_id.find("sonnet") != std::string_view::npos) {
        color = "\033[1;34m"; // Bold blue for Sonnet
    } else if (model_id.find("haiku") != std::string_view::npos) {
        color = "\033[1;32m"; // Bold green for Haiku
    } else {
        color = "\033[1;37m"; // Bold white for unknown
    }

    return color + "[" + name + "]" + "\033[0m";
}

} // namespace cc::utils
