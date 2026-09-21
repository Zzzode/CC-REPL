module;
#include <string>
#include <string_view>

export module cc.utils.display_tags;

export namespace cc::utils {

// Format a colored tag (e.g., "[ERROR]" in red)
std::string format_tag(std::string_view text, std::string_view color) {
    std::string result;
    result += color;
    result += "[";
    result += text;
    result += "]";
    result += "\033[0m";
    return result;
}

// Format a key-value badge (e.g., "model: sonnet-4")
std::string format_badge(std::string_view label, std::string_view value) {
    std::string result;
    result += "\033[2m"; // Dim for label
    result += label;
    result += ":\033[0m ";
    result += "\033[1m"; // Bold for value
    result += value;
    result += "\033[0m";
    return result;
}

// Format a pill-shaped tag with rounded appearance
std::string format_pill(std::string_view text) {
    std::string result;
    result += "\033[48;5;237m\033[38;5;252m"; // Gray background, light text
    result += " ";
    result += text;
    result += " ";
    result += "\033[0m";
    return result;
}

} // namespace cc::utils
