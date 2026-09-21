module;
#include <string>
#include <optional>
#include <sstream>

export module cc.ui.design.divider;

export namespace cc::ui::design {

[[nodiscard]] inline std::string repeat_divider_segment(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += text;
    return out;
}

// Render a horizontal divider line, optionally with a centered label
inline auto render_horizontal_divider(int width,
                                       std::optional<std::string> label = std::nullopt)
    -> std::string {
    if (!label.has_value() || label->empty()) {
        return repeat_divider_segment("─", width);
    }

    const std::string& text = label.value();
    int label_len = static_cast<int>(text.size()) + 2; // space on each side
    if (label_len + 4 > width) {
        // Label too long, just render plain line
        return repeat_divider_segment("─", width);
    }

    int left_len = (width - label_len) / 2;
    int right_len = width - label_len - left_len;

    std::ostringstream out;
    out << repeat_divider_segment("─", left_len) << " " << text << " "
        << repeat_divider_segment("─", right_len);
    return out.str();
}

// Render a vertical divider of the given height
inline auto render_vertical_divider(int height) -> std::string {
    std::ostringstream out;
    for (int i = 0; i < height; ++i) {
        out << "│";
        if (i < height - 1) out << "\n";
    }
    return out.str();
}

// Render a section divider with a title aligned to the left
inline auto render_section_divider(std::string_view title, int width) -> std::string {
    std::ostringstream out;
    int title_len = static_cast<int>(title.size());
    // Format: "── Title ───────────────"
    out << "── " << title << " ";
    int remaining = width - 4 - title_len;
    if (remaining > 0) {
        out << repeat_divider_segment("─", remaining);
    }
    return out.str();
}

} // namespace cc::ui::design
