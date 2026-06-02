module;
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.logo;

import cc.ui.layout;

export namespace cc::ui::logo {

// --- Gradient colors for ASCII art rendering ---
inline constexpr std::array<std::string_view, 5> kGradientColors = {
    "#6366f1", "#8b5cf6", "#a855f7", "#d946ef", "#ec4899"
};

// Convert hex color string to ANSI 24-bit foreground escape
[[nodiscard]] inline auto hex_to_ansi_fg(std::string_view hex) -> std::string {
    if (hex.size() < 7 || hex[0] != '#') return "";
    auto parse = [](std::string_view s) -> std::uint8_t {
        std::uint8_t val = 0;
        for (char c : s) {
            val <<= 4;
            if (c >= '0' && c <= '9') val |= static_cast<std::uint8_t>(c - '0');
            else if (c >= 'a' && c <= 'f') val |= static_cast<std::uint8_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= static_cast<std::uint8_t>(c - 'A' + 10);
        }
        return val;
    };
    auto r = parse(hex.substr(1, 2));
    auto g = parse(hex.substr(3, 2));
    auto b = parse(hex.substr(5, 2));
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

// --- Animated Clawd frames (4 animation frames) ---
inline constexpr std::array<std::string_view, 4> kClawdFrames = {
    R"(  /\_/\  
 ( o.o ) 
  > ^ <  )",
    R"(  /\_/\  
 ( o.o ) 
  > v <  )",
    R"(  /\_/\  
 ( -.- ) 
  > ^ <  )",
    R"(  /\_/\  
 ( o.o ) 
  > ~ <  )"
};

// --- Props equivalent structs ---

struct LogoProps {
    bool is_pip;
};

inline auto make_logo_props() -> LogoProps {
    return LogoProps{.is_pip = false};
}

struct AnimatedClawdProps {
    std::size_t frame;
};

struct WelcomeProps {
    bool is_new_user;
};

inline auto make_welcome_props() -> WelcomeProps {
    return WelcomeProps{.is_new_user = false};
}

struct CondensedLogoProps {
    bool verbose;
};

inline auto make_condensed_logo_props() -> CondensedLogoProps {
    return CondensedLogoProps{.verbose = false};
}

// --- Logo display data ---
struct LogoDisplayData {
    std::string version;
    std::string cwd;
    std::string billing_type;
    std::optional<std::string> agent_name;
    std::string model_display_name;
};

// --- Welcome tips ---
inline constexpr std::array<std::string_view, 6> kWelcomeTips = {
    "Use /help to see available commands",
    "Press Ctrl+C to interrupt the assistant",
    "Use @file to reference files in your prompt",
    "Press Escape to cancel the current input",
    "Use /model to switch between models",
    "Press Ctrl+R to search command history"
};

// --- ASCII art logo lines (CC-REPL branding) ---
inline constexpr std::array<std::string_view, 5> kLogoArt = {
    R"(  ____ ____       ____  _____ ____  _     )",
    R"( / ___/ ___|     |  _ \| ____|  _ \| |    )",
    R"(| |  | |   _____ | |_) |  _| | |_) | |    )",
    R"(| |__| |__|_____|_|  _ <| |___|  __/| |___ )",
    R"( \____\____|    |_| \_\_____|_|   |_____|)"
};

// --- Rendering functions ---

// Render a single Clawd animation frame
[[nodiscard]] inline auto render_clawd_frame(const AnimatedClawdProps& props)
    -> std::string {
    if (props.frame >= kClawdFrames.size()) return std::string(kClawdFrames[0]);
    return std::string(kClawdFrames[props.frame]);
}

// Render the gradient-colored ASCII logo
[[nodiscard]] inline auto render_logo_art() -> std::string {
    std::string result;
    for (std::size_t i = 0; i < kLogoArt.size(); ++i) {
        auto color_idx = i % kGradientColors.size();
        result += hex_to_ansi_fg(kGradientColors[color_idx]);
        result += kLogoArt[i];
        result += "\033[0m\n";
    }
    return result;
}

// Render the full logo with version
[[nodiscard]] inline auto render_logo(const LogoProps& props,
                                       std::string_view version)
    -> std::string {
    std::string result;
    if (!props.is_pip) {
        result += render_logo_art();
        result += "\n";
    }
    result += "\033[1mCC-REPL\033[0m v" + std::string(version) + "\n";
    return result;
}

// Render the welcome message with a random tip
[[nodiscard]] inline auto render_welcome(const WelcomeProps& props,
                                          std::string_view version,
                                          std::size_t tip_index)
    -> std::string {
    std::string result;
    if (props.is_new_user) {
        result += "\033[1mWelcome to CC-REPL!\033[0m\n\n";
    } else {
        result += "\033[1mCC-REPL\033[0m v" + std::string(version) + "\n\n";
    }
    if (tip_index < kWelcomeTips.size()) {
        result += "\033[2m💡 " + std::string(kWelcomeTips[tip_index]) + "\033[0m\n";
    }
    return result;
}

// Render the condensed logo (compact display for narrow terminals)
[[nodiscard]] inline auto render_condensed_logo(const CondensedLogoProps& props,
                                                 const LogoDisplayData& data,
                                                 int terminal_width)
    -> std::string {
    std::string result;
    int text_width = std::max(terminal_width - 15, 20);

    // Title + version
    result += "\033[1mCC-REPL\033[0m";
    auto version_display = data.version;
    if (static_cast<int>(version_display.size()) > text_width - 13) {
        version_display = version_display.substr(0, static_cast<std::size_t>(text_width - 13));
    }
    result += " \033[2mv" + version_display + "\033[0m\n";

    // Model + billing
    result += "\033[2m" + data.model_display_name;
    if (!data.billing_type.empty()) {
        result += " \xC2\xB7 " + data.billing_type;
    }
    result += "\033[0m\n";

    // CWD + agent
    if (data.agent_name.has_value()) {
        result += "\033[2m@" + *data.agent_name + " \xC2\xB7 ";
    } else {
        result += "\033[2m";
    }
    auto cwd_display = data.cwd;
    int cwd_max = data.agent_name.has_value()
        ? text_width - 1 - static_cast<int>(data.agent_name->size()) - 3
        : text_width;
    if (static_cast<int>(cwd_display.size()) > cwd_max) {
        cwd_display = "..." + cwd_display.substr(cwd_display.size() - static_cast<std::size_t>(cwd_max - 3));
    }
    result += cwd_display + "\033[0m\n";

    return result;
}

// --- FTXUI Component factory (forward declaration) ---
// Creates an interactive logo component with animation timer
// Returns Component for integration into FTXUI component tree
struct LogoComponentOptions {
    bool animated;
    bool condensed;
    bool show_welcome;
    bool is_new_user;
    bool is_pip;
};

inline auto make_logo_component_options() -> LogoComponentOptions {
    return LogoComponentOptions{
        .animated = true,
        .condensed = false,
        .show_welcome = true,
        .is_new_user = false,
        .is_pip = false
    };
}

// Render the full logo element for FTXUI (returns Element)
[[nodiscard]] auto render_logo_element(const LogoComponentOptions& options,
                                        const LogoDisplayData& data,
                                        int terminal_width) -> ftxui::Element;

// Create an interactive FTXUI Component with animation support
[[nodiscard]] auto make_logo_component(const LogoComponentOptions& options,
                                        const LogoDisplayData& data) -> ftxui::Component;

} // namespace cc::ui::logo
