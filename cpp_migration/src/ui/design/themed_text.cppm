module;
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdint>

export module cc.ui.design.themed_text;

export namespace cc::ui::design {

// Text decoration styles using ANSI escape codes
enum class TextStyle { Bold, Dim, Italic, Underline, Strikethrough };

// Apply a text style using ANSI escape sequences
inline auto style_text(std::string_view text, TextStyle style) -> std::string {
    std::string prefix;
    std::string suffix = "\033[0m";

    switch (style) {
        case TextStyle::Bold:          prefix = "\033[1m"; break;
        case TextStyle::Dim:           prefix = "\033[2m"; break;
        case TextStyle::Italic:        prefix = "\033[3m"; break;
        case TextStyle::Underline:     prefix = "\033[4m"; break;
        case TextStyle::Strikethrough: prefix = "\033[9m"; break;
    }

    return prefix + std::string(text) + suffix;
}

// Apply a named or hex color to text
inline auto color_text(std::string_view text, std::string_view color) -> std::string {
    std::ostringstream out;

    // Try to parse hex color (#RRGGBB or #RGB)
    if (!color.empty() && color[0] == '#') {
        uint8_t r = 0, g = 0, b = 0;
        if (color.size() == 7) {
            auto hex_byte = [](char hi, char lo) -> uint8_t {
                auto nibble = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                    return 0;
                };
                return (nibble(hi) << 4) | nibble(lo);
            };
            r = hex_byte(color[1], color[2]);
            g = hex_byte(color[3], color[4]);
            b = hex_byte(color[5], color[6]);
        } else if (color.size() == 4) {
            auto nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            r = nibble(color[1]) * 17;
            g = nibble(color[2]) * 17;
            b = nibble(color[3]) * 17;
        }
        out << "\033[38;2;" << (int)r << ";" << (int)g << ";" << (int)b << "m"
            << text << "\033[0m";
        return out.str();
    }

    // Named colors mapped to ANSI codes
    int code = 37; // default white
    if (color == "red")     code = 31;
    else if (color == "green")   code = 32;
    else if (color == "yellow")  code = 33;
    else if (color == "blue")    code = 34;
    else if (color == "magenta") code = 35;
    else if (color == "cyan")    code = 36;
    else if (color == "white")   code = 37;
    else if (color == "black")   code = 30;

    out << "\033[" << code << "m" << text << "\033[0m";
    return out.str();
}

// Render text with a color gradient (linear interpolation between two hex colors)
inline auto gradient_text(std::string_view text,
                          std::string_view start_color,
                          std::string_view end_color) -> std::string {
    if (text.empty()) return "";

    // Parse hex colors
    auto parse_hex = [](std::string_view hex) -> std::tuple<uint8_t, uint8_t, uint8_t> {
        uint8_t r = 0, g = 0, b = 0;
        if (hex.size() >= 7 && hex[0] == '#') {
            auto nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            r = (nibble(hex[1]) << 4) | nibble(hex[2]);
            g = (nibble(hex[3]) << 4) | nibble(hex[4]);
            b = (nibble(hex[5]) << 4) | nibble(hex[6]);
        }
        return {r, g, b};
    };

    auto [sr, sg, sb] = parse_hex(start_color);
    auto [er, eg, eb] = parse_hex(end_color);

    std::ostringstream out;
    int len = static_cast<int>(text.size());

    for (int i = 0; i < len; ++i) {
        float t = (len > 1) ? static_cast<float>(i) / (len - 1) : 0.0f;
        uint8_t r = static_cast<uint8_t>(sr + t * (er - sr));
        uint8_t g = static_cast<uint8_t>(sg + t * (eg - sg));
        uint8_t b = static_cast<uint8_t>(sb + t * (eb - sb));
        out << "\033[38;2;" << (int)r << ";" << (int)g << ";" << (int)b << "m"
            << text[i];
    }
    out << "\033[0m";
    return out.str();
}

// Truncate styled text to max_width visible characters (strips ANSI for measurement)
inline auto truncate_styled(std::string_view styled_text, int max_width) -> std::string {
    if (max_width <= 0) return "";

    std::string result;
    int visible_count = 0;
    bool in_escape = false;

    for (size_t i = 0; i < styled_text.size(); ++i) {
        char c = styled_text[i];

        if (c == '\033') {
            in_escape = true;
            result += c;
            continue;
        }
        if (in_escape) {
            result += c;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                in_escape = false;
            }
            continue;
        }

        if (visible_count >= max_width) {
            // Append ellipsis and reset
            if (max_width >= 3) {
                // Remove last 3 visible chars for ellipsis
                result += "…\033[0m";
            }
            break;
        }
        result += c;
        ++visible_count;
    }

    return result;
}

} // namespace cc::ui::design
