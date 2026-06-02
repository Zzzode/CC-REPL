module;
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

export module cc.utils.render_options;

export namespace cc::utils {

struct RenderOptions {
    int width;
    bool color;
    bool unicode;
    std::string theme;
};

// Detect if stdout supports color output
bool should_use_color() {
    // Respect NO_COLOR convention
    if (std::getenv("NO_COLOR") != nullptr) return false;

    // Check FORCE_COLOR
    if (std::getenv("FORCE_COLOR") != nullptr) return true;

    // Check if stdout is a terminal
    if (!isatty(STDOUT_FILENO)) return false;

    // Check TERM
    const char* term = std::getenv("TERM");
    if (term && std::string_view(term) == "dumb") return false;

    return true;
}

// Get terminal width
int get_terminal_width() {
    // Try ioctl
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }

    // Try COLUMNS env var
    const char* cols = std::getenv("COLUMNS");
    if (cols) {
        try {
            int w = std::stoi(cols);
            if (w > 0) return w;
        } catch (...) {}
    }

    return 80; // Default fallback
}

// Get render options from current terminal environment
RenderOptions get_render_options() {
    RenderOptions opts;
    opts.width = get_terminal_width();
    opts.color = should_use_color();

    // Check for unicode support
    const char* lang = std::getenv("LANG");
    const char* lc_all = std::getenv("LC_ALL");
    std::string locale = lc_all ? lc_all : (lang ? lang : "");
    opts.unicode = locale.find("UTF-8") != std::string::npos ||
                   locale.find("utf-8") != std::string::npos ||
                   locale.find("utf8") != std::string::npos;

    // Theme
    const char* theme = std::getenv("CLAUDE_THEME");
    opts.theme = theme ? theme : "dark";

    return opts;
}

} // namespace cc::utils
