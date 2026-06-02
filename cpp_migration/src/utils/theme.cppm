module;
#include <map>
#include <string>
#include <string_view>

export module cc.utils.theme;

export namespace cc::utils {

struct Theme {
    std::string name;
    std::map<std::string, std::string> colors;
};

namespace detail {
    inline const Theme& dark_theme() {
        static Theme t = {
            "dark",
            {
                {"primary",     "\033[38;5;75m"},   // Light blue
                {"secondary",   "\033[38;5;248m"},  // Light gray
                {"success",     "\033[38;5;78m"},   // Green
                {"warning",     "\033[38;5;220m"},  // Yellow
                {"error",       "\033[38;5;196m"},  // Red
                {"info",        "\033[38;5;117m"},  // Cyan
                {"dim",         "\033[2m"},         // Dim
                {"bold",        "\033[1m"},         // Bold
                {"reset",       "\033[0m"},         // Reset
                {"heading",     "\033[1;38;5;75m"}, // Bold blue
                {"code",        "\033[38;5;223m"},  // Warm white
                {"comment",     "\033[38;5;243m"},  // Gray
                {"string",      "\033[38;5;114m"},  // Green
                {"keyword",     "\033[38;5;176m"},  // Purple
                {"number",      "\033[38;5;209m"},  // Orange
            }
        };
        return t;
    }

    inline const Theme& light_theme() {
        static Theme t = {
            "light",
            {
                {"primary",     "\033[38;5;25m"},   // Dark blue
                {"secondary",   "\033[38;5;240m"},  // Dark gray
                {"success",     "\033[38;5;28m"},   // Dark green
                {"warning",     "\033[38;5;172m"},  // Dark yellow
                {"error",       "\033[38;5;160m"},  // Dark red
                {"info",        "\033[38;5;30m"},   // Dark cyan
                {"dim",         "\033[2m"},
                {"bold",        "\033[1m"},
                {"reset",       "\033[0m"},
                {"heading",     "\033[1;38;5;25m"},
                {"code",        "\033[38;5;52m"},
                {"comment",     "\033[38;5;247m"},
                {"string",      "\033[38;5;22m"},
                {"keyword",     "\033[38;5;90m"},
                {"number",      "\033[38;5;130m"},
            }
        };
        return t;
    }

    inline const Theme& mono_theme() {
        static Theme t = {
            "monochrome",
            {
                {"primary",     "\033[1m"},
                {"secondary",   "\033[0m"},
                {"success",     "\033[1m"},
                {"warning",     "\033[4m"},  // Underline for emphasis
                {"error",       "\033[1;7m"}, // Bold reverse
                {"info",        "\033[0m"},
                {"dim",         "\033[2m"},
                {"bold",        "\033[1m"},
                {"reset",       "\033[0m"},
                {"heading",     "\033[1;4m"},
                {"code",        "\033[0m"},
                {"comment",     "\033[2m"},
                {"string",      "\033[0m"},
                {"keyword",     "\033[1m"},
                {"number",      "\033[0m"},
            }
        };
        return t;
    }

    inline std::string& current_theme_name() {
        static std::string name = "dark";
        return name;
    }
} // namespace detail

// Get current active theme
Theme get_current_theme() {
    auto& name = detail::current_theme_name();
    if (name == "light") return detail::light_theme();
    if (name == "monochrome") return detail::mono_theme();
    return detail::dark_theme();
}

// Set theme by name
void set_theme(std::string_view name) {
    detail::current_theme_name() = std::string(name);
}

// Get a specific color code from the current theme
std::string get_color(std::string_view key) {
    auto theme = get_current_theme();
    auto it = theme.colors.find(std::string(key));
    if (it != theme.colors.end()) return it->second;
    return "\033[0m"; // Reset as fallback
}

} // namespace cc::utils
