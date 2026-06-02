module;
#include <cstddef>
#include <string>
#include <string_view>

export module cc.utils.model.model_capabilities;

export namespace cc::utils {

struct Capabilities {
    bool vision;
    bool thinking;
    bool tool_use;
    bool streaming;
    bool computer_use;
    std::size_t context_window;
    std::size_t max_output;
};

Capabilities get_capabilities(std::string_view model_id) {
    Capabilities caps{};

    // All Claude 4 models support these baseline features
    caps.vision = true;
    caps.thinking = true;
    caps.tool_use = true;
    caps.streaming = true;
    caps.computer_use = true;
    caps.context_window = 200000;

    if (model_id.find("opus") != std::string_view::npos) {
        caps.max_output = 32768;
    } else if (model_id.find("haiku") != std::string_view::npos) {
        caps.max_output = 8192;
    } else {
        // Sonnet and default
        caps.max_output = 16384;
    }

    return caps;
}

bool supports_feature(std::string_view model_id, std::string_view feature) {
    auto caps = get_capabilities(model_id);

    if (feature == "vision") return caps.vision;
    if (feature == "thinking") return caps.thinking;
    if (feature == "tool_use") return caps.tool_use;
    if (feature == "streaming") return caps.streaming;
    if (feature == "computer_use") return caps.computer_use;

    // Unknown feature
    return false;
}

} // namespace cc::utils
