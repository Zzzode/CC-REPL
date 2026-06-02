module;
#include <string>
#include <sstream>

export module cc.ui.design.status_icon;

export namespace cc::ui::design {

// Status severity levels
enum class Status { Success, Error, Warning, Info, Loading, Pending };

// Get the Unicode icon character for a given status
inline auto get_status_icon(Status status) -> std::string {
    switch (status) {
        case Status::Success: return "✓";
        case Status::Error:   return "✗";
        case Status::Warning: return "⚠";
        case Status::Info:    return "ℹ";
        case Status::Loading: return "◌";
        case Status::Pending: return "○";
    }
    return "?";
}

// Render a colored status badge with label
inline auto render_status_badge(Status status, std::string_view label) -> std::string {
    std::ostringstream out;

    // Color per status
    const char* color_code = "\033[0m";
    switch (status) {
        case Status::Success: color_code = "\033[32m"; break; // green
        case Status::Error:   color_code = "\033[31m"; break; // red
        case Status::Warning: color_code = "\033[33m"; break; // yellow
        case Status::Info:    color_code = "\033[34m"; break; // blue
        case Status::Loading: color_code = "\033[36m"; break; // cyan
        case Status::Pending: color_code = "\033[2m";  break; // dim
    }

    out << color_code << get_status_icon(status) << " " << label << "\033[0m";
    return out.str();
}

// Render a small colored dot indicator
inline auto render_status_dot(Status status) -> std::string {
    std::ostringstream out;

    switch (status) {
        case Status::Success: out << "\033[32m●\033[0m"; break;
        case Status::Error:   out << "\033[31m●\033[0m"; break;
        case Status::Warning: out << "\033[33m●\033[0m"; break;
        case Status::Info:    out << "\033[34m●\033[0m"; break;
        case Status::Loading: out << "\033[36m◐\033[0m"; break;
        case Status::Pending: out << "\033[2m○\033[0m";  break;
    }

    return out.str();
}

} // namespace cc::ui::design
