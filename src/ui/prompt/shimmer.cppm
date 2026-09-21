module;
#include <string>
#include <sstream>
#include <array>
#include <cmath>

export module cc.ui.prompt.shimmer;

export namespace cc::ui::prompt {

// Render a shimmer animation across the input area (loading state placeholder)
inline auto render_shimmer_input(int width, int frame) -> std::string {
    std::ostringstream out;

    // Shimmer uses a moving highlight across the width
    // Characters cycle through dim blocks to create wave effect
    static constexpr std::array<const char*, 4> gradient = {
        "░", "▒", "▓", "▒"
    };

    int highlight_pos = frame % (width + 8); // Highlight moves left to right

    for (int i = 0; i < width; ++i) {
        int dist = std::abs(i - highlight_pos);
        if (dist < 4) {
            // Within highlight zone - use gradient
            int grad_idx = dist;
            out << "\033[36m" << gradient[grad_idx] << "\033[0m";
        } else {
            out << "\033[2m░\033[0m";
        }
    }

    return out.str();
}

// Render a placeholder text with shimmer effect
inline auto render_placeholder_shimmer(std::string_view placeholder, int frame) -> std::string {
    if (placeholder.empty()) return "";

    std::ostringstream out;
    int len = static_cast<int>(placeholder.size());
    int highlight_center = frame % (len + 6) - 3;

    for (int i = 0; i < len; ++i) {
        int dist = std::abs(i - highlight_center);
        if (dist <= 1) {
            // Bright highlight on the shimmer wave
            out << "\033[37m" << placeholder[i] << "\033[0m";
        } else if (dist <= 3) {
            // Medium brightness
            out << "\033[90m" << placeholder[i] << "\033[0m";
        } else {
            // Base dim color
            out << "\033[2m" << placeholder[i] << "\033[0m";
        }
    }

    return out.str();
}

} // namespace cc::ui::prompt
