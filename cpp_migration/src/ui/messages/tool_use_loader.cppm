module;
#include <string>
#include <optional>
#include <sstream>
#include <array>
#include <algorithm>

export module cc.ui.messages.tool_use_loader;

import cc.ui.design.figures;  // kSpinnerFrames canonical set (GAP 4)

export namespace cc::ui::messages {

// Display state for an in-progress tool invocation
struct ToolUseDisplay {
    std::string tool_name;
    std::string status;
    std::optional<std::string> preview;
    int frame = 0;
};

// Render a tool-use loader with spinner and status
inline auto render_tool_use_loader(ToolUseDisplay display, int width) -> std::string {
    std::ostringstream out;

    // Spinner animation — canonical 10-frame braille set
    // TS REF: SpinnerGlyph.tsx (GAP 4: fig-spinner-frame-inconsistency)
    // Previously 8 frames, now unified to 10 via cc.ui.design.figures.
    namespace figs = cc::ui::design::figures;
    const auto glyph = figs::spinner_frame_glyph(display.frame);

    // Tool name with icon
    out << "\033[36m" << glyph << " ⚡ " << display.tool_name << "\033[0m";

    // Status text
    if (!display.status.empty()) {
        out << " \033[2m(" << display.status << ")\033[0m";
    }
    out << "\n";

    // Preview content (truncated)
    if (display.preview.has_value() && !display.preview->empty()) {
        const std::string& preview = display.preview.value();
        int max_preview_width = width - 4;

        out << "  \033[2m";
        if (static_cast<int>(preview.size()) > max_preview_width) {
            out << preview.substr(0, max_preview_width - 1) << "…";
        } else {
            out << preview;
        }
        out << "\033[0m";
    }

    return out.str();
}

// Render a tool result preview (collapsed view of output)
inline auto render_tool_result_preview(std::string_view result, int max_lines) -> std::string {
    std::ostringstream out;
    int line_count = 0;
    size_t start = 0;

    out << "\033[2m┌─ Result ─────\033[0m\n";

    while (start < result.size() && line_count < max_lines) {
        auto end = result.find('\n', start);
        std::string_view line;
        if (end == std::string_view::npos) {
            line = result.substr(start);
            start = result.size();
        } else {
            line = result.substr(start, end - start);
            start = end + 1;
        }

        out << "\033[2m│\033[0m " << line << "\n";
        ++line_count;
    }

    // Indicate truncation
    if (start < result.size()) {
        // Count remaining lines
        int remaining = 0;
        size_t pos = start;
        while (pos < result.size()) {
            auto nl = result.find('\n', pos);
            ++remaining;
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        out << "\033[2m│ ... " << remaining << " more lines\033[0m\n";
    }

    out << "\033[2m└─────────────\033[0m";

    return out.str();
}

} // namespace cc::ui::messages
