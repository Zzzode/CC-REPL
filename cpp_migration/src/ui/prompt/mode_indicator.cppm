module;
#include <string>
#include <sstream>

export module cc.ui.prompt.mode_indicator;

export namespace cc::ui::prompt {

// Input mode for the prompt
enum class InputMode { Normal, Plan, Vim, Search };

// Get the display color for a mode
inline auto get_mode_color(InputMode mode) -> std::string {
    switch (mode) {
        case InputMode::Normal: return "\033[32m"; // green
        case InputMode::Plan:   return "\033[35m"; // magenta
        case InputMode::Vim:    return "\033[33m"; // yellow
        case InputMode::Search: return "\033[36m"; // cyan
    }
    return "\033[0m";
}

// Render the mode indicator badge
inline auto render_mode_indicator(InputMode mode) -> std::string {
    std::ostringstream out;
    std::string color = get_mode_color(mode);

    switch (mode) {
        case InputMode::Normal:
            out << color << "● NORMAL\033[0m";
            break;
        case InputMode::Plan:
            out << color << "◆ PLAN\033[0m";
            break;
        case InputMode::Vim:
            out << color << "▶ VIM\033[0m";
            break;
        case InputMode::Search:
            out << color << "◎ SEARCH\033[0m";
            break;
    }

    return out.str();
}

} // namespace cc::ui::prompt
