module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <cstdint>

export module cc.utils.ansi_rendering;

export namespace cc::utils::ansi_rendering {

struct RenderOptions {
    uint32_t width{80};
    uint32_t height{24};
    std::string font_family{"monospace"};
    uint32_t font_size{14};
};

inline std::expected<std::vector<uint8_t>, std::string> ansi_to_png(
    std::string_view ansi_text, const RenderOptions& opts = {}) {
    return std::vector<uint8_t>{};
}

inline std::expected<std::string, std::string> ansi_to_svg(
    std::string_view ansi_text, const RenderOptions& opts = {}) {
    return "";
}

inline std::expected<std::string, std::string> to_asciicast(
    std::string_view session_recording) {
    return "";
}

inline std::string strip_ansi_codes(std::string_view text) {
    return std::string(text);
}

} // namespace cc::utils::ansi_rendering
