module;
#include <string>
#include <optional>
#include <sstream>
#include <iomanip>
#include <array>
#include <cmath>

export module cc.ui.design.progress_bar;

import cc.ui.design.figures;  // kSpinnerFrames canonical set (GAP 4)

export namespace cc::ui::design {

[[nodiscard]] inline std::string repeat_progress_segment(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += text;
    return out;
}

// Configuration for rendering a progress bar
struct ProgressConfig {
    float progress = 0.0f;          // 0.0 to 1.0
    int width = 40;
    std::optional<std::string> label;
    bool show_percentage = true;
    std::string fill_char = "█";
    std::string empty_char = "░";
};

// Render a determinate progress bar
inline auto render_progress_bar(ProgressConfig config) -> std::string {
    std::ostringstream out;

    // Clamp progress to [0, 1]
    float progress = std::max(0.0f, std::min(1.0f, config.progress));

    // Calculate bar dimensions
    int bar_width = config.width;
    if (config.label.has_value()) {
        bar_width -= static_cast<int>(config.label->size()) + 1;
    }
    if (config.show_percentage) {
        bar_width -= 5; // " 100%"
    }
    bar_width = std::max(bar_width, 10);

    // Render label if present
    if (config.label.has_value()) {
        out << config.label.value() << " ";
    }

    // Render the bar
    int filled = static_cast<int>(std::round(progress * bar_width));
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) {
            out << config.fill_char;
        } else {
            out << config.empty_char;
        }
    }

    // Render percentage
    if (config.show_percentage) {
        out << " " << std::setw(3) << static_cast<int>(progress * 100) << "%";
    }

    return out.str();
}

// Render a spinner animation frame with optional label.
// TS REF: SpinnerGlyph.tsx — canonical 10-frame braille spinner from
//   cc::ui::design::figures::kSpinnerFrames (GAP 4: fig-spinner-frame-inconsistency).
//   Previously 8 frames, now unified to 10.
inline auto render_spinner(int frame, std::string_view label = "") -> std::string {
    namespace figs = cc::ui::design::figures;
    const auto glyph = figs::spinner_frame_glyph(frame);

    std::ostringstream out;
    out << glyph;
    if (!label.empty()) {
        out << " " << label;
    }
    return out.str();
}

// Render an indeterminate progress indicator (bouncing bar)
inline auto render_indeterminate(int frame, int width) -> std::string {
    const int bar_len = std::max(3, width / 5);
    int total_positions = width - bar_len;
    if (total_positions <= 0) {
        return repeat_progress_segment("═", width);
    }

    // Bounce: position oscillates between 0 and total_positions
    int cycle = total_positions * 2;
    int pos = frame % cycle;
    if (pos >= total_positions) {
        pos = cycle - pos; // Reverse direction
    }

    std::ostringstream out;
    for (int i = 0; i < width; ++i) {
        if (i >= pos && i < pos + bar_len) {
            out << "━";
        } else {
            out << "─";
        }
    }
    return out.str();
}

} // namespace cc::ui::design
