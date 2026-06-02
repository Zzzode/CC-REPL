module;
#include <string>
#include <optional>
#include <sstream>
#include <vector>

export module cc.ui.design.themed_box;

export namespace cc::ui::design {

// Available box border styles
enum class BoxStyle { Single, Double, Rounded, Heavy, None };

// Configuration for box rendering
struct BoxConfig {
    BoxStyle style = BoxStyle::Rounded;
    std::optional<std::string> title;
    int width = 0;   // 0 = auto-fit to content
    int height = 0;  // 0 = auto-fit to content
    bool fill = false;
};

namespace detail {

// Border character sets for each style
struct BorderChars {
    const char* tl; // top-left
    const char* tr; // top-right
    const char* bl; // bottom-left
    const char* br; // bottom-right
    const char* h;  // horizontal
    const char* v;  // vertical
};

inline auto get_border_chars(BoxStyle style) -> BorderChars {
    switch (style) {
        case BoxStyle::Single:
            return {"┌", "┐", "└", "┘", "─", "│"};
        case BoxStyle::Double:
            return {"╔", "╗", "╚", "╝", "═", "║"};
        case BoxStyle::Rounded:
            return {"╭", "╮", "╰", "╯", "─", "│"};
        case BoxStyle::Heavy:
            return {"┏", "┓", "┗", "┛", "━", "┃"};
        case BoxStyle::None:
            return {" ", " ", " ", " ", " ", " "};
    }
    return {"╭", "╮", "╰", "╯", "─", "│"}; // default Rounded
}

// Split content into lines
inline auto split_lines(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty()) lines.emplace_back("");
    return lines;
}

} // namespace detail

// Render content inside a box with configurable border style
inline auto render_box(std::string_view content, BoxConfig config) -> std::string {
    auto chars = detail::get_border_chars(config.style);
    auto lines = detail::split_lines(content);

    // Determine box width
    int inner_width = config.width > 2 ? config.width - 2 : 0;
    if (inner_width == 0) {
        for (const auto& line : lines) {
            inner_width = std::max(inner_width, static_cast<int>(line.size()));
        }
        inner_width += 2; // padding
    }

    // Determine box height (number of content lines)
    int inner_height = config.height > 2 ? config.height - 2 : static_cast<int>(lines.size());

    std::ostringstream out;

    // Top border with optional title
    out << chars.tl;
    if (config.title.has_value() && !config.title->empty()) {
        const std::string& title = config.title.value();
        int title_space = inner_width - static_cast<int>(title.size()) - 2;
        if (title_space >= 2) {
            out << chars.h << " " << title << " ";
            for (int i = 0; i < title_space - 1; ++i) out << chars.h;
        } else {
            for (int i = 0; i < inner_width; ++i) out << chars.h;
        }
    } else {
        for (int i = 0; i < inner_width; ++i) out << chars.h;
    }
    out << chars.tr << "\n";

    // Content lines
    for (int row = 0; row < inner_height; ++row) {
        out << chars.v;
        if (row < static_cast<int>(lines.size())) {
            int pad = inner_width - static_cast<int>(lines[row].size());
            int left_pad = 1;
            int right_pad = pad - left_pad;
            if (right_pad < 0) right_pad = 0;
            out << std::string(left_pad, ' ') << lines[row]
                << std::string(right_pad, ' ');
        } else if (config.fill) {
            out << std::string(inner_width, ' ');
        } else {
            out << std::string(inner_width, ' ');
        }
        out << chars.v << "\n";
    }

    // Bottom border
    out << chars.bl;
    for (int i = 0; i < inner_width; ++i) out << chars.h;
    out << chars.br;

    return out.str();
}

// Convenience: render content with a simple rounded border
inline auto render_bordered(std::string_view content,
                            BoxStyle style = BoxStyle::Rounded) -> std::string {
    BoxConfig config;
    config.style = style;
    return render_box(content, config);
}

} // namespace cc::ui::design
