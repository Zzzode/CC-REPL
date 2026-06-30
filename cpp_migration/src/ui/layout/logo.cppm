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

// --- formatWelcomeMessage (faithful port of upstream logo utilities) ---
// TS rule: empty/null username OR username longer than MAX_USERNAME_LENGTH(20)
// → "Welcome back!".  Otherwise → "Welcome back {user}!".
// The first-run banner caption ("Welcome to Claude Code") is handled
// separately by the banner renderer and only applies on a first-run session.
inline constexpr std::size_t k_max_username_length = 20;
[[nodiscard]] inline auto format_welcome_message(std::string_view username)
    -> std::string {
    if (username.empty() || username.size() > k_max_username_length) {
        return "Welcome back!";
    }
    return "Welcome back " + std::string(username) + "!";
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
[[nodiscard]] inline auto render_condensed_logo(const CondensedLogoProps&,
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

// ============================================================
// Faithful: TS CondensedLogo (LogoV2/CondensedLogo.tsx + Clawd.tsx)
// Layout: hbox of [9×3 Clawd block-art glyph] + [gap=2 cols] + [3-line text column].
// Text column rows:
//   1: Claude Code (bold, text) + v{version} (dim, muted)  — SINGLE line, no break
//   2: {model} [· {billing}]                                    — all dimColor
//   3: [@{agent} · ] {truncatePath(cwd)}                       — all dimColor
// ============================================================
[[nodiscard]] inline auto RenderCondensedLogoElement(
    const LogoDisplayData& data, int term_cols) -> ftxui::Element {
    using namespace ftxui;

    // TS theme.ts dark tokens — exact hex matches from utils/theme.ts L443/L453/L455
    const Color kText  (255, 255, 255);   // theme.text → #FFFFFF
    const Color kMuted (153, 153, 153);   // dimColor → theme.inactive #999999
    const Color kClawd (215, 119,  87);   // theme.claude / clawd_body #D77757

    // --- Clawd graphic (9 cols × 3 rows), faithful to Clawd.tsx
    //
    // Col count per row is exactly 9.  Char selection matches the "std"
    // renderer (not the apple-terminal narrow fallback of 7 cols) since we
    // have full unicode block support in FTXUI.
    //
    //   Row 1 (eyes/top):    2ws + ▛███▜ + 1ws + 1seg  = 9
    //   Row 2 (body/mid):    ▝▜ + █████ + ▛▘            = 9
    //   Row 3 (feet/bottom): 2ws + ▘▘ + ws + ▝▝ + 2ws  = 9
    //
    // NOTE: When terminals have ambiguous emoji/block width issues, the
    // overall Clawd box width can be off by ±1; we accept that.
    const std::string_view kClawdRow1 = "  \xE2\x96\x9B\xE2\x96\x88\xE2\x96\x88"
                                        "\xE2\x96\x88\xE2\x96\x9C ";
    // U+2588 FULL BLOCK, U+259B QUADRANT UPPER LEFT, U+259C QUADRANT UPPER RIGHT
    const std::string_view kClawdRow2 = "\xE2\x96\x9D\xE2\x96\x9C"   // ▝▜ (U+259D, U+259C reversed)
                                        "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"  // 5 × █ U+2588
                                        "\xE2\x96\x9B\xE2\x96\x98";  // ▛▘ (U+259B, U+2598)
    const std::string_view kClawdRow3 = "  \xE2\x96\x98\xE2\x96\x98 \xE2\x96\x9D\xE2\x96\x9D  ";
                                        // 2ws + ▘▘ + 1ws + ▝▝ + 2ws = 9

    Element clawd_col = vbox({
        text(std::string(kClawdRow1)) | color(kClawd),
        text(std::string(kClawdRow2)) | color(kClawd),
        text(std::string(kClawdRow3)) | color(kClawd),
    });

    // --- Text column: 3 rows, width = max(term_cols - 15, 20)
    // (15 = 9 clawd + 2 gap + 4 pad).  Matches TS textWidth formula exactly.
    const int text_width = std::max(term_cols - 15, 20);

    // Row 1: <Text bold>Claude Code</Text> <Text dimColor>v{version}</Text>
    // NOTE: TS appends " v" + version literally after "Claude Code", with a
    // single space separator; NO line break, NO trailing tag like "-cpp".
    const std::string ver = data.version.empty()
        ? std::string("0.0.0") : data.version;
    Element row1 = hbox({
        text("Claude Code") | bold | color(kText),
        text(" v" + ver) | dim | color(kMuted),
    });

    // Row 2: model [· billing]
    // TS CondensedLogo: formatModelAndBilling — when model+billing fits on one
    // line we render "{model} · {billing}", else two separate dim lines
    // (here simplify: single line since truncation is handled elsewhere by
    // the text container; text_width limit truncates overflow naturally).
    Element row2_body;
    if (data.billing_type.empty()) {
        row2_body = text(data.model_display_name);
    } else {
        row2_body = hbox({
            text(data.model_display_name),
            text(" \xC2\xB7 ") | color(kMuted),  // · U+00B7 MIDDLE DOT
            text(data.billing_type),
        });
    }
    Element row2 = std::move(row2_body) | dim | color(kMuted);

    // Row 3: [@{agent} · ] truncatePath(cwd)
    // TS logoV2Utils::truncatePath — MIDDLE truncation with U+2026 "…" as the
    // ellipsis between the first segment and the trailing segments.  For
    // simplicity we approximate with "prefix … /last/segment" when overflow.
    std::optional<std::string> agent_prefix;
    int agent_width = 0;
    if (data.agent_name.has_value() && !data.agent_name->empty()) {
        agent_prefix = "@" + *data.agent_name + " \xC2\xB7 ";
        agent_width  = static_cast<int>(agent_prefix->size());
    }
    int cwd_max = std::max(text_width - agent_width, 8);
    std::string cwd_display = data.cwd;
    if (static_cast<int>(cwd_display.size()) > cwd_max) {
        // Middle-truncate approx: keep first segment + " … " + last segment
        auto first_slash = cwd_display.find('/', 1);
        auto last_slash  = cwd_display.rfind('/');
        if (first_slash != std::string::npos && last_slash != std::string::npos
            && last_slash > first_slash + 1) {
            std::string head = cwd_display.substr(0, first_slash + 1);
            std::string tail = cwd_display.substr(last_slash);
            cwd_display = head + "\xE2\x80\xA6" + tail;  // … U+2026
            // If still too long, fall back to suffix truncation.
            if (static_cast<int>(cwd_display.size()) > cwd_max) {
                cwd_display = "\xE2\x80\xA6" + cwd_display.substr(
                    cwd_display.size() - static_cast<std::size_t>(cwd_max - 3));
            }
        } else {
            cwd_display = "\xE2\x80\xA6" + cwd_display.substr(
                cwd_display.size() - static_cast<std::size_t>(cwd_max - 3));
        }
    }
    Elements row3_parts;
    if (agent_prefix) {
        row3_parts.push_back(text(*agent_prefix));
    }
    row3_parts.push_back(text(cwd_display));
    Element row3 = hbox(std::move(row3_parts)) | dim | color(kMuted);

    Element text_col = vbox({
        std::move(row1),
        std::move(row2),
        std::move(row3),
    });

    // TS outer: <Box flexDirection='row' gap={2} alignItems='center'>
    // gap={2} means two whitespace columns between clawd and text col.
    return hbox({
        std::move(clawd_col),
        text("  "),
        std::move(text_col),
    });
}

/// Small pill chip used in the footer left column (TS outer chrome).
[[nodiscard]] inline auto RenderBrandChip() -> ftxui::Element {
    using namespace ftxui;
    const Color kBg(20, 20, 22);
    const Color kBr(60, 60, 60);
    // FTXUI's `borderStyled` is not available in all vendored versions; use
    // `border` + color + bgcolor as the portable fallback.
    return hbox({text(" CC-REPL ") | bgcolor(kBg)})
         | border | color(kBr) | bgcolor(kBg);
}

/// TS Opus1mMergeNotice banner shown under the condensed logo.
///
/// Faithful structure (Opus1mMergeNotice.tsx):
///   <Box paddingLeft={2}>
///     <AnimatedAsterisk char=UP_ARROW color-sweep>
///     <Text dimColor> Opus now defaults to 1M context · 5x more room, same pricing</Text>
///   </Box>
/// The entire text segment is dimColor (chalk.dim SGR 2), NOT plain text.
/// AnimatedAsterisk performs a 2×1500ms hue 0..360° color sweep; we
/// TS Opus1mMergeNotice uses AnimatedAsterisk which does a 2x hue sweep, then
/// settles to SETTLED_GREY rgb(153,153,153) = #999999.  We render the static
/// post-sweep (settled) colour for the ↑ arrow — matching what users see 2–3s
/// after the page loads.  Main body and the " · " sub-clause are BOTH in the
/// same dimColor Text (TS CondensedLogo L106).
[[nodiscard]] inline auto RenderOpus1MNotice() -> ftxui::Element {
    using namespace ftxui;
    // AnimatedAsterisk SETTLED_GREY (TS src/components/ui/AnimatedAsterisk.tsx)
    const Color kArrowSettledGrey(153, 153, 153);
    const Color kDim(153, 153, 153);  // dimColor → theme.inactive

    // paddingLeft={2}
    return hbox({
        text("  "),
        text("\xE2\x86\x91 ") | color(kArrowSettledGrey) | bold,   // ↑ U+2191
        hbox({
            text("Opus now defaults to 1M context"),
            text(" \xC2\xB7 "),                                     // · U+00B7
            text("5x more room, same pricing"),
        }) | dim | color(kDim),   // ENTIRE segment dimColor — incl. separator
    });
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
