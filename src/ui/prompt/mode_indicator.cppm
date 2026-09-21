/// @file mode_indicator.cppm
/// @brief Faithful port of TS PromptInputModeIndicator.tsx — the 3-way
///        prompt prefix glyph renderer (❯ / ! / agent-tinted ❯).
///
/// TS REF: src/components/PromptInput/PromptInputModeIndicator.tsx
///
/// The 3-way prefix render (TS lines 80-91):
///   1. viewingAgentName set        → ❯ with viewedTeammateThemeColor
///   2. mode === 'bash'             → !  with bashBorder color
///   3. otherwise                   → ❯ with teammateColor (if swarms
///                                    enabled) or default text color
///
/// CPP PRIORITY NOTE: viewingAgentName (viewing_agent_color) takes
///   precedence over bash mode — matching TS where `viewingAgentName ?`
///   is checked BEFORE `mode === 'bash'`.  When a viewing agent is active,
///   the prefix is ALWAYS ❯ (never !), tinted with that agent's color.
///
/// This module provides TWO rendering paths:
///   - render_prefix_glyph_ansi()  → ANSI-escaped string (for string-based
///                                    renderers like render_prompt_input_full)
///   - render_prefix_glyph_element() → ftxui::Element (for FTXUI component
///                                      renderers like RenderPromptInput)
///
/// CPP NOTE: The TS "viewing agent" concept maps to active_agent in
/// PromptInputFullProps.  When active_agent is set with a color, we tint
/// the prefix glyph with that agent's color — matching TS's
/// viewedTeammateThemeColor path.  When no agent is active but a teammate
/// color is supplied (teammate_color param), we use that for the default
/// path — matching TS's getTeammateThemeColor() + isAgentSwarmsEnabled().
module;

#include <optional>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

import cc.ui.design.figures;
import cc.ui.common.types;

export module cc.ui.prompt.mode_indicator;

export namespace cc::ui::prompt::mode_indicator {

using namespace ftxui;
namespace figs = cc::ui::design::figures;
using PromptInputMode = cc::ui::common::PromptInputMode;

// ─── Agent color → ANSI mapping (TS REF: agentColorManager.ts) ───────────
// TS AGENT_COLORS = ['red', 'orange', 'yellow', 'green', 'cyan',
//                    'blue', 'purple', 'pink']
// We map these to ANSI 256-color codes for the string-based renderer.
[[nodiscard]] inline std::string agent_color_to_ansi(std::string_view color_name) {
    // ANSI 256-color palette: 30-37 are dim, 90-97 are bright.
    // We use the bright variants for agent tints to match TS theme colors.
    if (color_name == "red")    return "\033[91m";
    if (color_name == "orange") return "\033[38;5;208m";  // orange
    if (color_name == "yellow") return "\033[93m";
    if (color_name == "green")  return "\033[92m";
    if (color_name == "cyan")   return "\033[96m";
    if (color_name == "blue")   return "\033[94m";
    if (color_name == "purple") return "\033[38;5;135m";  // purple
    if (color_name == "pink")   return "\033[38;5;212m";  // pink
    return "\033[37m";  // fallback: white
}

// ─── Bash border ANSI (TS REF: theme.ts bashBorder) ──────────────────────
// TS dark theme: rgb(255, 0, 135) → hot pink.  ANSI 256: color 198.
inline constexpr const char* kBashBorderAnsi = "\033[38;5;198m";

// ─── Default text color reset ────────────────────────────────────────────
inline constexpr const char* kAnsiReset = "\033[0m";

// ─── String-based prefix renderer (for render_prompt_input_full) ─────────
//
// Returns the ANSI-escaped prefix string: glyph + space + color reset.
// e.g. "\033[92m❯ \033[0m"  (green-tinted pointer)
//
// Parameters:
//   mode                — current PromptInputMode (Bash → '!', others → '❯')
//   viewing_agent_color — optional color name of the agent being viewed
//                         (TS "viewingAgentName" + "viewingAgentColor").
//                         When set, takes priority over bash mode and tints
//                         ❯ with that agent's color.
//   teammate_color      — optional teammate color name for the default path
//                         (TS "isAgentSwarmsEnabled() ? teammateColor").
//                         Only used when no viewing agent and not bash mode.
//   is_loading          — when true, apply dim attribute (TS dimColor)
//
// Priority (TS REF: PromptInputModeIndicator.tsx line 82):
//   1. viewingAgentName set  → ❯ with viewedTeammateThemeColor
//   2. mode === 'bash'       → !  with bashBorder color
//   3. otherwise             → ❯ with teammateColor (if swarms) or default
[[nodiscard]] inline std::string render_prefix_glyph_ansi(
    PromptInputMode mode,
    std::optional<std::string_view> viewing_agent_color = std::nullopt,
    std::optional<std::string_view> teammate_color = std::nullopt,
    bool is_loading = false)
{
    const bool is_bash = (mode == PromptInputMode::Bash);
    std::string result;

    // Priority check: viewing agent takes precedence over bash mode.
    const bool has_viewing_agent =
        viewing_agent_color.has_value() && !viewing_agent_color->empty();
    const bool show_bash = is_bash && !has_viewing_agent;

    // Pick ANSI color prefix.
    if (has_viewing_agent) {
        // TS: <PromptChar themeColor={viewedTeammateThemeColor} />
        result += agent_color_to_ansi(*viewing_agent_color);
    } else if (show_bash) {
        // TS: <Text color="bashBorder" dimColor={isLoading}>!&nbsp;</Text>
        result += kBashBorderAnsi;
    } else if (teammate_color.has_value() && !teammate_color->empty()) {
        // TS: <PromptChar themeColor={isAgentSwarmsEnabled() ? teammateColor
        //     : undefined} /> — only when swarms enabled + teammate has color.
        result += agent_color_to_ansi(*teammate_color);
    }
    // else: default terminal color (no ANSI escape — matches TS
    //   color={undefined}, i.e. Ink's default text color).  We deliberately
    //   do NOT emit "\033[37m" (white) because that overpowers the terminal
    //   theme in light-mode / daltonized themes.

    if (is_loading) {
        // TS dimColor → ANSI dim attribute (\033[2m).
        result += "\033[2m";
    }

    // Append glyph + space (2 display cells, matching TS figures.pointer +
    // NBSP or '!' + NBSP).  When a viewing agent is active, ALWAYS use ❯
    // regardless of mode (TS parity: viewingAgentName branch renders ❯).
    if (show_bash) {
        result += std::string(figs::kBashPrefix);
    } else {
        result += std::string(figs::kPointerPrefix);
    }

    result += kAnsiReset;
    return result;
}

// ─── FTXUI Element prefix renderer (for component-based paths) ───────────
//
// Returns an ftxui::Element containing the colored prefix glyph.
// This is the FTXUI counterpart of render_prefix_glyph_ansi().
//
// Parameters:
//   mode                — current PromptInputMode (Bash → '!', others → '❯')
//   viewing_agent_color — optional color of the agent being viewed (TS
//                         "viewingAgentName" + "viewingAgentColor").  When
//                         set, takes priority over bash mode.
//   teammate_color      — optional teammate color for the default path
//                         (TS "isAgentSwarmsEnabled() ? teammateColor").
//   is_loading          — when true, apply dim attribute (TS dimColor)
//
// TS REF: PromptInputModeIndicator.tsx — the <Box> wrapping the <Text>
//         or <PromptChar> element with alignItems="flex-start" etc.
[[nodiscard]] inline Element render_prefix_glyph_element(
    PromptInputMode mode,
    std::optional<Color> viewing_agent_color = std::nullopt,
    std::optional<Color> teammate_color = std::nullopt,
    bool is_loading = false)
{
    const bool is_bash = (mode == PromptInputMode::Bash);
    std::string glyph_str;

    // Priority: viewing agent takes precedence over bash mode.
    const bool has_viewing_agent = viewing_agent_color.has_value();
    const bool show_bash = is_bash && !has_viewing_agent;

    // Glyph selection: viewing agent → ❯, bash → !, default → ❯.
    if (show_bash) {
        glyph_str = std::string(figs::kBashPrefix);
    } else {
        glyph_str = std::string(figs::kPointerPrefix);
    }

    Element el = text(glyph_str);

    // Apply color (same priority as render_prefix_glyph_ansi).
    if (has_viewing_agent) {
        el = el | color(*viewing_agent_color);
    } else if (show_bash) {
        // TS: color="bashBorder" — hot pink rgb(255,0,135).
        el = el | color(Color::RGB(255, 0, 135));
    } else if (teammate_color.has_value()) {
        el = el | color(*teammate_color);
    }
    // else: default text color (FTXUI uses the terminal's default).

    if (is_loading) {
        el = el | dim;
    }

    return el;
}

// ─── Convenience: render just the glyph (no space) for inline use ───────
//
// TS REF: figures.pointer (U+276F) alone — used in StickyPromptHeader,
//         user message prefix, etc.  Not to be confused with the 2-cell
//         "glyph + space" prefix used at the prompt input position.
[[nodiscard]] inline std::string render_pointer_glyph_ansi(
    std::optional<std::string_view> color_name = std::nullopt,
    bool is_loading = false)
{
    std::string result;
    if (color_name.has_value() && !color_name->empty()) {
        result += agent_color_to_ansi(*color_name);
    }
    // else: default terminal color (no ANSI escape — matches TS
    //   color={undefined}).  Do NOT emit "\033[37m" (white) here.
    if (is_loading) result += "\033[2m";
    result += std::string(figs::kPointer);
    result += kAnsiReset;
    return result;
}

} // namespace cc::ui::prompt::mode_indicator
