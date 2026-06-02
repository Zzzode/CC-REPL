module;
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

export module cc.utils.hyperlink;

export namespace cc::utils {

namespace fs = std::filesystem;

// Check if the terminal supports OSC 8 hyperlinks
bool supports_hyperlinks() {
    // iTerm2, WezTerm, Kitty, Windows Terminal support hyperlinks
    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string_view tp(term_program);
        if (tp == "iTerm.app" || tp == "WezTerm" || tp == "vscode") return true;
    }

    const char* wt_session = std::getenv("WT_SESSION");
    if (wt_session) return true;

    // Check VTE version (GNOME Terminal, etc.)
    const char* vte = std::getenv("VTE_VERSION");
    if (vte) {
        try {
            int ver = std::stoi(vte);
            if (ver >= 5000) return true;
        } catch (...) {}
    }

    return false;
}

// Create an OSC 8 hyperlink (clickable in supported terminals)
std::string make_hyperlink(std::string_view url, std::string_view text) {
    if (!supports_hyperlinks()) {
        // Fallback: just show text
        return std::string(text);
    }

    // OSC 8 hyperlink format
    std::string result;
    result += "\x1b]8;;";
    result += url;
    result += "\x1b\\";
    result += text;
    result += "\x1b]8;;\x1b\\";
    return result;
}

// Create a file:// hyperlink (with optional line number)
std::string make_file_link(fs::path file, std::optional<int> line) {
    std::string url = "file://";
    url += fs::absolute(file).string();
    if (line.has_value()) {
        url += ":" + std::to_string(*line);
    }

    // Display text: relative path if possible
    std::string display = file.filename().string();
    if (line.has_value()) {
        display += ":" + std::to_string(*line);
    }

    return make_hyperlink(url, display);
}

} // namespace cc::utils
