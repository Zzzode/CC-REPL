module;
#include <string>
#include <vector>
#include <optional>
#include <sstream>

export module cc.ui.design.tabs;

export namespace cc::ui::design {

[[nodiscard]] inline std::string repeat_tabs(std::string_view text, int count) {
    std::string result;
    if (count <= 0) return result;
    result.reserve(text.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        result += text;
    }
    return result;
}

// Configuration for tab bar rendering
struct TabConfig {
    std::vector<std::string> labels;
    int active_index = 0;
    bool closeable = false;
};

// Render tabs with a classic underline style
inline auto render_tabs(TabConfig config) -> std::string {
    std::ostringstream out;

    for (size_t i = 0; i < config.labels.size(); ++i) {
        bool active = (static_cast<int>(i) == config.active_index);
        if (active) {
            out << "┌─";
        } else {
            out << "  ";
        }

        out << config.labels[i];

        if (config.closeable) {
            out << " ×";
        }

        if (active) {
            out << "─┐";
        } else {
            out << "  ";
        }

        if (i < config.labels.size() - 1) {
            out << " ";
        }
    }
    out << "\n";

    // Underline: active tab has a gap, others have a line
    int total_width = 0;
    for (size_t i = 0; i < config.labels.size(); ++i) {
        int tab_width = static_cast<int>(config.labels[i].size()) + 4;
        if (config.closeable) tab_width += 2;
        total_width += tab_width;
        if (i < config.labels.size() - 1) total_width += 1;
    }

    int pos = 0;
    for (size_t i = 0; i < config.labels.size(); ++i) {
        int tab_width = static_cast<int>(config.labels[i].size()) + 4;
        if (config.closeable) tab_width += 2;

        bool active = (static_cast<int>(i) == config.active_index);
        if (active) {
            out << "┘" << std::string(tab_width - 2, ' ') << "└";
        } else {
            out << repeat_tabs("─", tab_width);
        }
        pos += tab_width;

        if (i < config.labels.size() - 1) {
            out << "─";
            pos += 1;
        }
    }

    return out.str();
}

// Render pill-style tabs (rounded indicator for active tab)
inline auto render_pill_tabs(TabConfig config) -> std::string {
    std::ostringstream out;

    for (size_t i = 0; i < config.labels.size(); ++i) {
        bool active = (static_cast<int>(i) == config.active_index);
        if (active) {
            out << "⌈" << config.labels[i] << "⌉";
        } else {
            out << " " << config.labels[i] << " ";
        }

        if (config.closeable) {
            out << " ×";
        }

        if (i < config.labels.size() - 1) {
            out << " │ ";
        }
    }

    return out.str();
}

} // namespace cc::ui::design
