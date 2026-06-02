/// @file ink_components.cppm
/// @brief Ink-style UI components as FTXUI-compatible abstractions.
/// Migrates ink/components/ (18 TS files: Box, Text, Button, ScrollBox, Link,
/// Spacer, etc.)
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <variant>
#include <cstdint>

export module cc.ui.ink_components;

export namespace cc::ui::ink_components {

// ============================================================
// Component Props Structures
// ============================================================

/// Properties for a Box component
struct BoxProps {
    std::optional<int> width;
    std::optional<int> height;
    int padding_x{0};
    int padding_y{0};
    int margin_x{0};
    int margin_y{0};
    bool border{false};
    std::string border_style{"single"};
};

/// Properties for a Text component
struct TextProps {
    bool bold{false};
    bool italic{false};
    bool underline{false};
    bool dim{false};
    bool strikethrough{false};
    std::optional<std::string> color;
    std::optional<std::string> bg_color;
};

/// Properties for a Button component
struct ButtonProps {
    std::string label;
    std::function<void()> on_press;
    bool focused{false};
    bool disabled{false};
};

/// Properties for a ScrollBox component
struct ScrollBoxProps {
    int visible_height{10};
    int scroll_offset{0};
    int total_lines{0};
    bool show_scrollbar{true};
};

/// Properties for a Link component
struct LinkProps {
    std::string text;
    std::string url;
    bool underline{true};
};

/// Properties for a Spacer component
struct SpacerProps {
    int size{1};
    bool horizontal{false};
};

// ============================================================
// Rendering Functions
// ============================================================

/// Render a box with optional border and padding around content
inline std::string render_box(std::string_view content, BoxProps props = {}) {
    std::string result;
    // Apply vertical margin
    for (int i = 0; i < props.margin_y; ++i) result += '\n';
    // Apply horizontal margin + optional border
    std::string h_margin(static_cast<std::size_t>(props.margin_x), ' ');
    std::string h_padding(static_cast<std::size_t>(props.padding_x), ' ');

    if (props.border) {
        result += h_margin + "+" + std::string(content.size() + props.padding_x * 2, '-') + "+\n";
    }
    // Vertical padding
    for (int i = 0; i < props.padding_y; ++i) {
        result += h_margin + (props.border ? "|" : "") + h_padding +
                  std::string(content.size(), ' ') + h_padding +
                  (props.border ? "|" : "") + '\n';
    }
    // Content line
    result += h_margin + (props.border ? "|" : "") + h_padding +
              std::string(content) + h_padding + (props.border ? "|" : "") + '\n';
    // Vertical padding (bottom)
    for (int i = 0; i < props.padding_y; ++i) {
        result += h_margin + (props.border ? "|" : "") + h_padding +
                  std::string(content.size(), ' ') + h_padding +
                  (props.border ? "|" : "") + '\n';
    }
    if (props.border) {
        result += h_margin + "+" + std::string(content.size() + props.padding_x * 2, '-') + "+\n";
    }
    // Bottom margin
    for (int i = 0; i < props.margin_y; ++i) result += '\n';
    return result;
}

/// Render styled text with ANSI escape sequences
inline std::string render_text(std::string_view text, TextProps props = {}) {
    std::string result;
    // Apply ANSI styles
    if (props.bold) result += "\033[1m";
    if (props.dim) result += "\033[2m";
    if (props.italic) result += "\033[3m";
    if (props.underline) result += "\033[4m";
    if (props.strikethrough) result += "\033[9m";
    result += text;
    // Reset if any style was applied
    if (props.bold || props.dim || props.italic || props.underline || props.strikethrough) {
        result += "\033[0m";
    }
    return result;
}

/// Render a button element
inline std::string render_button(ButtonProps props) {
    std::string result;
    if (props.disabled) {
        result = "[ " + props.label + " ] (disabled)";
    } else if (props.focused) {
        result = "\033[7m[ " + props.label + " ]\033[0m";
    } else {
        result = "[ " + props.label + " ]";
    }
    return result;
}

/// Render a scrollable box of lines
inline std::string render_scroll_box(std::vector<std::string> lines, ScrollBoxProps props = {}) {
    std::string result;
    int end = std::min(props.scroll_offset + props.visible_height,
                       static_cast<int>(lines.size()));
    for (int i = props.scroll_offset; i < end; ++i) {
        result += lines[static_cast<std::size_t>(i)] + '\n';
    }
    if (props.show_scrollbar && static_cast<int>(lines.size()) > props.visible_height) {
        result += "[scrollbar " + std::to_string(props.scroll_offset) + "/" +
                  std::to_string(lines.size()) + "]\n";
    }
    return result;
}

/// Render a hyperlink (OSC 8 terminal hyperlink)
inline std::string render_link(LinkProps props) {
    std::string text_content = props.text;
    if (props.underline) {
        text_content = "\033[4m" + text_content + "\033[0m";
    }
    // OSC 8 hyperlink sequence
    return "\033]8;;" + props.url + "\033\\" + text_content + "\033]8;;\033\\";
}

/// Render a spacer (vertical or horizontal whitespace)
inline std::string render_spacer(SpacerProps props = {}) {
    if (props.horizontal) {
        return std::string(static_cast<std::size_t>(props.size), ' ');
    }
    return std::string(static_cast<std::size_t>(props.size), '\n');
}

/// Render one or more newlines
inline std::string render_newline(int count = 1) {
    return std::string(static_cast<std::size_t>(count), '\n');
}

/// Pass through raw ANSI content unchanged
inline std::string render_raw_ansi(std::string_view ansi_content) {
    return std::string(ansi_content);
}

/// Calculate scroll position returning {start, end} indices
inline std::pair<int, int> calculate_scroll_position(
    int total, int visible, int current) {
    if (total <= visible) return {0, total};
    int half = visible / 2;
    int start = current - half;
    if (start < 0) start = 0;
    int end = start + visible;
    if (end > total) {
        end = total;
        start = total - visible;
    }
    return {start, end};
}

} // namespace cc::ui::ink_components
