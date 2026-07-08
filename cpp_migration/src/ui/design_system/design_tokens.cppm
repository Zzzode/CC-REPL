/// @file design_tokens.cppm
/// @brief Design tokens: semantic palette, spacing, typography, animation,
/// role-based color resolution. Mirrors the TS side tokens gathered from
///   src/utils/theme.ts       – 6 theme variants × 89 explicit color fields
///   src/constants/figures    – border / glyph tokens re-used where sensible
/// The following TS-side locations were searched and are N/A in this codebase:
///   src/ui/theme/            – not present (tokens live in utils/theme.ts)
///   styled-components / framer-motion – not present (animation is
///     useAnimationFrame-driven; here we emit numeric easing constants)
///   src/components/StatusBar/– not present (status-line lives under
///     ui/prompt/prompt_input_footer.cppm as RenderStatusLine)
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cmath>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.design.tokens;

export namespace cc::ui::design::tokens {

// ─── Spacing (cells in the TTY grid) ─────────────────────────────────────────
// Mirrors the 4/8/16/24/32/48 px grid used on the TS side.  Since the terminal
// is cell-based we simply expose integer counts of cells.
namespace spacing {
inline constexpr std::int32_t xs   = 1;
inline constexpr std::int32_t sm   = 2;
inline constexpr std::int32_t md   = 3;
inline constexpr std::int32_t lg   = 4;
inline constexpr std::int32_t xl   = 6;
inline constexpr std::int32_t xxl  = 8;
} // namespace spacing

// ─── Radius (FTXUI border decorator approximation) ───────────────────────────
// FTXUI only exposes border/borderRounded/borderDouble/borderHeavy/borderLight
// so we expose an enum mapping directly to those decorator names.
enum class Radius { None, Light, Rounded, Double, Heavy };

// ─── Typography scale ────────────────────────────────────────────────────────
// On a TTY we can only change the *apparent* size via bold/dim/underlined and
// separator lines.  The enum values are therefore "hints" consumed by
// ui::design::primitives::heading() rather than true font-sizes.
enum class TextSize : std::uint8_t {
    Caption,   // dim, reduced weight
    Body,      // default
    Subtitle,  // bold
    Title,     // bold + underlined separator
    Display,   // bold + heavy separator + blockquote bar
};

// ─── Animation easing constants ──────────────────────────────────────────────
// PHASE_5 NOTE: the TS side drives all animation via useAnimationFrame with
// explicit per-frame t∈[0,1] lerps.  We expose the easing function numbers
// directly as cubic-bezier constants so callers can plug them into a
// chrono-driven Renderer without pulling in a full tweening engine.
namespace easing {
inline constexpr double linear            = 1.0;  // linear easing coefficient
inline constexpr double ease_out_cubic    = 0.33; // deceleration
inline constexpr double ease_in_out_quad  = 0.5;  // symmetric
inline constexpr double standard          = 0.2;  // Material-ish
// HSL sweep timings (copied verbatim from AnimatedAsterisk.tsx)
inline constexpr std::int32_t asterisk_sweep_ms      = 1500;
inline constexpr std::int32_t asterisk_sweep_count   = 2;
inline constexpr std::int32_t spinner_frame_interval = 80;
inline constexpr std::int32_t logo_refresh_ms        = 120;
} // namespace easing

// ─── Semantic roles ──────────────────────────────────────────────────────────
// Each Theme exposes a single color per role.  New call sites should use
// token_by_role() rather than reaching into the palette struct by name.
enum class Role : std::uint8_t {
    Primary,      // Claude accent (clawd_body)
    Info,         // Info / auxiliary blue accents
    Success,      // Green
    Warning,      // Yellow / amber
    Danger,       // Error / rejection (= TS "error" token)
    Muted,        // Inactive / dim (replaces ANSI dim per ThemedText.tsx)
    Subtle,       // Subtle foreground (chrome, borders)
    Suggestion,   // Typeahead suggestion foreground
    Permission,   // Dialog frame color (= TS "permission" token, same as suggestion in dark)
    RateLimit,    // Rate-limit pill fill
    Chrome,       // UI chrome / background-adjacent lines
    Brief,        // Brief-mode labels
    Ultra,        // Ultra-thinking rainbow head
    UserMessageBackground,      // user-bubble fill (TS: userMessageBackground)
    UserMessageBackgroundHover, // user-bubble hover fill (TS: userMessageBackgroundHover)
};

// ─── Palette ─────────────────────────────────────────────────────────────────
// A pared-down version of the 89-field TS Theme type.  Only the colors that
// are actually referenced by the UI design primitives are exposed here; the
// rest are left for per-component imports from the older ui/design/* modules.
struct Palette {
    ftxui::Color primary;
    ftxui::Color primary_shimmer;
    ftxui::Color info;
    ftxui::Color success;
    ftxui::Color warning;
    ftxui::Color danger;
    ftxui::Color muted;      // "inactive" on the TS side
    ftxui::Color subtle;
    ftxui::Color suggestion;
    ftxui::Color text;
    ftxui::Color inverse_text;
    ftxui::Color background;
    ftxui::Color chrome;
    ftxui::Color rate_limit_fill;
    ftxui::Color rate_limit_empty;
    ftxui::Color brief_label;
    // User/assistant message bubble & selection chrome (TS theme tokens):
    //   userMessageBackground      — dark-mode user bubble tint
    //   messageActionsBackground   — cool-gray bubble when selected/focused
    //   userMessageBackgroundHover — bubble tint on hover / interactive header
    ftxui::Color user_message_background;
    ftxui::Color user_message_background_hover;
    ftxui::Color message_actions_background;
    // rainbow cycle used by ultra-thinking (7 steps)
    std::array<ftxui::Color, 7> rainbow;
    ftxui::Color rainbow_shimmer;
    // diff tokens
    ftxui::Color diff_added;
    ftxui::Color diff_removed;
    ftxui::Color diff_added_word;
    ftxui::Color diff_removed_word;
    ftxui::Color merged;
    // ─── Prompt / input chrome (TS theme tokens) ─────────────────────────
    // bashBorder — accent color for '!' bash-mode prefix glyph, prompt border,
    //              transcript bash-input message frames.  Named for semantic
    //              role even though the REPL prompt no longer uses a full
    //              rectangular "border" around the input area.
    //              TS dark:   rgb(255,   0, 135)  hot pink / magenta
    //              TS light:  rgb(255,   0, 135)  same (always saturated pink)
    // promptBorder — outer frame of the brief prompt (separator top/bottom
    //                lines rendered around input).  TS variants:
    //                dark/light: ansi:whiteBright ≈ rgb(136,136,136) mid-gray
    ftxui::Color bash_border;
    ftxui::Color prompt_border;
    // TS theme token: "permission" is the default dialog frame color.
    // In TS dark palette: permission == suggestion == rgb(177, 185, 249).
    // Kept as a separate struct field so future palette reworks (ANSI,
    // daltonized) can diverge the two values without breaking call sites.
    ftxui::Color permission;
};

// ─── Concrete palettes (copied verbatim from src/utils/theme.ts rgb() values)
// Light/daltonized variants included; monochrome reduces everything to
// grayscale for accessibility testing and reduced-color terminals.
namespace palette {

// rgb(215,119,87) — Claude's canonical "clawd" orange
inline const auto CLAWDED      = ftxui::Color::RGB(215, 119,  87);
inline const auto CLAWDED_SHIM = ftxui::Color::RGB(232, 164, 134);

inline const Palette dark = {
    .primary             = CLAWDED,
    .primary_shimmer     = CLAWDED_SHIM,
    .info                = ftxui::Color::RGB(177, 185, 249),
    .success             = ftxui::Color::RGB( 78, 186, 101),
    .warning             = ftxui::Color::RGB(255, 193,   7),
    .danger              = ftxui::Color::RGB(255, 107, 128),
    .muted               = ftxui::Color::RGB(153, 153, 153),
    // TS dark tokens (src/utils/theme.ts rgb() values — exact copy):
    //   subtle                   = rgb(80, 80, 80)
    //   suggestion               = rgb(177, 185, 249)  (lavender, NOT sky blue)
    //   text                     = rgb(255, 255, 255)  (pure white)
    // NOTE (clr-dark-palette-11-p1): CPP `info` carries TS's info/permission
    // accent — corrected sky-blue rgb(110,151,255) → TS lavender
    // rgb(177,185,249) (== permission/suggestion) so dialog/info accents match.
    .subtle              = ftxui::Color::RGB( 80,  80,  80),
    .suggestion          = ftxui::Color::RGB(177, 185, 249),
    .text                = ftxui::Color::RGB(255, 255, 255),
    // TS dark inverseText = rgb(0,0,0) (pure black), used as fg on light
    // accent chips.  (Distinct from `background`, which stays app-chrome dark.)
    .inverse_text        = ftxui::Color::RGB(  0,   0,   0),
    // TS dark background = rgb(0,204,204) is a KNOWN non-rendered TS artifact
    // (Ink never paints it; the audit flags it as a likely false value).  CPP
    // keeps the real app-chrome dark rgb(32,33,36) rather than paint the UI
    // bright cyan — faithful-in-spirit, avoids a destructive visual change.
    .background          = ftxui::Color::RGB( 32,  33,  36),
    .chrome              = ftxui::Color::RGB( 55,  57,  61),
    // TS dark rate_limit_fill = rgb(177,185,249), rate_limit_empty = rgb(80,83,112).
    .rate_limit_fill     = ftxui::Color::RGB(177, 185, 249),
    .rate_limit_empty    = ftxui::Color::RGB( 80,  83, 112),
    .brief_label         = CLAWDED,
    // TS dark message-chrome tokens:
    //   userMessageBackground      = rgb(55, 55, 55)      (dark slate bubble)
    //   messageActionsBackground   = rgb(44, 50, 62)      (cool-gray selection)
    //   userMessageBackgroundHover = rgb(70, 70, 75)      (slightly lighter slate)
    .user_message_background        = ftxui::Color::RGB( 55,  55,  55),
    .user_message_background_hover  = ftxui::Color::RGB( 70,  70,  75),
    .message_actions_background     = ftxui::Color::RGB( 44,  50,  62),
    // TS ultrathink keyword rainbow (theme.ts:177-183 — identical across all
    // themes).  Corrected from the Chart.js default palette to the TS
    // warm→cool 7-stop gradient (clr-rainbow-28-p1).
    .rainbow = {{
        ftxui::Color::RGB(235,  95,  87),   // rainbow_red
        ftxui::Color::RGB(245, 139,  87),   // rainbow_orange
        ftxui::Color::RGB(250, 195,  95),   // rainbow_yellow
        ftxui::Color::RGB(145, 200, 130),   // rainbow_green
        ftxui::Color::RGB(130, 170, 220),   // rainbow_blue
        ftxui::Color::RGB(155, 130, 200),   // rainbow_indigo
        ftxui::Color::RGB(200, 130, 180),   // rainbow_violet
    }},
    .rainbow_shimmer     = ftxui::Color::RGB(230, 230, 250),
    // TS dark diff tokens (src/utils/theme.ts, verbatim) — corrected from the
    // GitHub-bright greens/reds to TS's saturated dark shades (clr-dark-11):
    //   diffAdded=rgb(34,92,43)  diffRemoved=rgb(122,41,54)
    //   diffAddedWord=rgb(56,166,96)  diffRemovedWord=rgb(179,89,107)
    .diff_added          = ftxui::Color::RGB( 34,  92,  43),
    .diff_removed        = ftxui::Color::RGB(122,  41,  54),
    .diff_added_word     = ftxui::Color::RGB( 56, 166,  96),
    .diff_removed_word   = ftxui::Color::RGB(179,  89, 107),
    // TS dark merged = rgb(175,135,255) (electric violet, == autoAccept).
    .merged              = ftxui::Color::RGB(175, 135, 255),
    // Prompt chrome (TS theme tokens — verbatim):
    //   dark bashBorder            = rgb(255,  0, 135)
    //   dark promptBorder          = rgb(136,136, 136)  ansi:whiteBright
    .bash_border         = ftxui::Color::RGB(255,   0, 135),
    .prompt_border       = ftxui::Color::RGB(136, 136, 136),
    // TS dark permission = rgb(177, 185, 249) — identical to suggestion.
    .permission          = ftxui::Color::RGB(177, 185, 249),
};

inline const Palette light = {
    .primary             = CLAWDED,
    .primary_shimmer     = CLAWDED_SHIM,
    // TS lightTheme (src/utils/theme.ts:115-191) — exact rgb() values
    // (clr-light-palette-17-p1).  `info` carries TS permission/suggestion
    // accent = rgb(87,105,247) medium blue.
    .info                = ftxui::Color::RGB( 87, 105, 247),
    .success             = ftxui::Color::RGB( 44, 122,  57),
    .warning             = ftxui::Color::RGB(150, 108,  30),
    .danger              = ftxui::Color::RGB(171,  43,  63),
    .muted               = ftxui::Color::RGB(102, 102, 102),
    .subtle              = ftxui::Color::RGB(175, 175, 175),
    .suggestion          = ftxui::Color::RGB( 87, 105, 247),
    .text                = ftxui::Color::RGB(  0,   0,   0),
    .inverse_text        = ftxui::Color::RGB(255, 255, 255),
    // TS light background = rgb(0,153,153) is the same non-rendered TS artifact
    // as dark; keep CPP's real light app-chrome rather than paint the UI cyan.
    .background          = ftxui::Color::RGB(245, 245, 245),
    .chrome              = ftxui::Color::RGB(220, 220, 220),
    .rate_limit_fill     = ftxui::Color::RGB( 87, 105, 247),
    .rate_limit_empty    = ftxui::Color::RGB( 39,  47, 111),
    .brief_label         = CLAWDED,
    // Light-mode equivalents of the TS message chrome tokens:
    //   userMessageBackground      = rgb(240, 240, 240)  near-white bubble
    //   messageActionsBackground   = rgb(232, 236, 244)  cool gray
    //   userMessageBackgroundHover = rgb(220, 220, 228)  slightly darker shade
    .user_message_background        = ftxui::Color::RGB(240, 240, 240),
    .user_message_background_hover  = ftxui::Color::RGB(220, 220, 228),
    .message_actions_background     = ftxui::Color::RGB(232, 236, 244),
    .rainbow              = dark.rainbow,
    .rainbow_shimmer     = ftxui::Color::RGB(120,  80, 200),
    // TS light diff tokens (theme.ts:141-146, verbatim):
    //   diffAdded=rgb(105,219,124)  diffRemoved=rgb(255,168,180)
    //   diffAddedWord=rgb(47,157,68)  diffRemovedWord=rgb(209,69,75)
    .diff_added          = ftxui::Color::RGB(105, 219, 124),
    .diff_removed        = ftxui::Color::RGB(255, 168, 180),
    .diff_added_word     = ftxui::Color::RGB( 47, 157,  68),
    .diff_removed_word   = ftxui::Color::RGB(209,  69,  75),
    // TS light merged = rgb(135,0,255) (electric violet, == autoAccept).
    .merged              = ftxui::Color::RGB(135,   0, 255),
    // Prompt chrome (TS light theme tokens — verbatim):
    //   light bashBorder            = rgb(255,  0, 135)   same saturated pink
    //   light promptBorder        = rgb(153,153, 153)   ansi:whiteBright
    .bash_border         = ftxui::Color::RGB(255,   0, 135),
    .prompt_border       = ftxui::Color::RGB(153, 153, 153),
    // TS light permission = rgb( 87, 105, 247) — deep blue.
    .permission          = ftxui::Color::RGB( 87, 105, 247),
};

// Daltonized variants (deuteranopia-safe).  TS uses EXPLICIT rgb() literals
// (theme.ts darkDaltonizedTheme 521-596 / lightDaltonizedTheme 359-434), NOT a
// matrix approximation — ported verbatim here (clr-daltonized-27-p1).
inline const Palette dark_daltonized = {
    // TS claude (daltonized) = rgb(255,153,51) orange adjusted for deuteranopia.
    .primary             = ftxui::Color::RGB(255, 153,  51),
    .primary_shimmer     = ftxui::Color::RGB(255, 183, 101),  // claudeShimmer
    // info/suggestion/permission all = TS rgb(153,204,255) light blue.
    .info                = ftxui::Color::RGB(153, 204, 255),
    .success             = ftxui::Color::RGB( 51, 153, 255),  // blue, not green
    .warning             = ftxui::Color::RGB(255, 204,   0),  // yellow-orange
    .danger              = ftxui::Color::RGB(255, 102, 102),  // error
    .muted               = ftxui::Color::RGB(153, 153, 153),  // inactive
    .subtle              = ftxui::Color::RGB( 80,  80,  80),
    .suggestion          = ftxui::Color::RGB(153, 204, 255),
    .text                = ftxui::Color::RGB(255, 255, 255),
    .inverse_text        = ftxui::Color::RGB(  0,   0,   0),
    // TS background rgb(0,204,204) is the non-rendered artifact — keep chrome.
    .background          = dark.background,
    .chrome              = dark.chrome,
    .rate_limit_fill     = ftxui::Color::RGB(153, 204, 255),
    .rate_limit_empty    = ftxui::Color::RGB( 69,  92, 115),
    .brief_label         = ftxui::Color::RGB(255, 153,  51),
    // Daltonized: reuse dark-daltonized base bubble chromes (TS uses the same
    // userMessageBackground/messageActionsBackground as dark).
    .user_message_background        = dark.user_message_background,
    .user_message_background_hover  = dark.user_message_background_hover,
    .message_actions_background     = dark.message_actions_background,
    // TS rainbow is identical across all themes (theme.ts:177-183), including
    // daltonized — alias the corrected dark gradient (clr-rainbow-28-p1).
    .rainbow = dark.rainbow,
    .rainbow_shimmer     = dark.rainbow_shimmer,
    // TS daltonized diffs: dark blue / dark red (deuteranopia-safe).
    //   diffAdded=rgb(0,68,102)  diffRemoved=rgb(102,0,0)
    //   diffAddedWord=rgb(0,119,179)  diffRemovedWord=rgb(179,0,0)
    .diff_added          = ftxui::Color::RGB(  0,  68, 102),
    .diff_removed        = ftxui::Color::RGB(102,   0,   0),
    .diff_added_word     = ftxui::Color::RGB(  0, 119, 179),
    .diff_removed_word   = ftxui::Color::RGB(179,   0,   0),
    .merged              = ftxui::Color::RGB(175, 135, 255),  // == autoAccept
    // Prompt chrome — dark daltonized: bashBorder per TS darkDaltonizedTheme
    //   = rgb(51,153,255) bright blue (not pink — CVD-safe). promptBorder = dark gray.
    .bash_border         = ftxui::Color::RGB( 51, 153, 255),
    .prompt_border       = ftxui::Color::RGB(136, 136, 136),
    // TS darkDaltonizedTheme permission = rgb(153, 204, 255).
    .permission          = ftxui::Color::RGB(153, 204, 255),
};

inline const Palette light_daltonized = {
    // TS lightDaltonizedTheme (theme.ts:359-434) — explicit rgb() literals.
    .primary             = ftxui::Color::RGB(255, 153,  51),  // claude (deuteranopia)
    .primary_shimmer     = ftxui::Color::RGB(255, 183, 101),  // claudeShimmer
    .info                = ftxui::Color::RGB( 51, 102, 255),  // permission/suggestion
    .success             = ftxui::Color::RGB(  0, 102, 153),  // blue, not green
    .warning             = ftxui::Color::RGB(255, 153,   0),  // orange
    .danger              = ftxui::Color::RGB(204,   0,   0),  // pure red (error)
    .muted               = ftxui::Color::RGB(102, 102, 102),  // inactive
    .subtle              = ftxui::Color::RGB(175, 175, 175),
    .suggestion          = ftxui::Color::RGB( 51, 102, 255),
    .text                = ftxui::Color::RGB(  0,   0,   0),
    .inverse_text        = ftxui::Color::RGB(255, 255, 255),
    // TS background rgb(0,153,153) is the non-rendered artifact — keep chrome.
    .background          = light.background,
    .chrome              = light.chrome,
    .rate_limit_fill     = ftxui::Color::RGB( 51, 102, 255),
    .rate_limit_empty    = ftxui::Color::RGB( 23,  46, 114),
    .brief_label         = ftxui::Color::RGB(255, 153,  51),
    .user_message_background        = light.user_message_background,
    .user_message_background_hover  = light.user_message_background_hover,
    .message_actions_background     = light.message_actions_background,
    .rainbow             = dark.rainbow,   // identical across all themes
    .rainbow_shimmer     = light.rainbow_shimmer,
    // TS light-daltonized diffs: light blue / light red (deuteranopia-safe).
    //   diffAdded=rgb(153,204,255)  diffRemoved=rgb(255,204,204)
    //   diffAddedWord=rgb(51,102,204)  diffRemovedWord=rgb(153,51,51)
    .diff_added          = ftxui::Color::RGB(153, 204, 255),
    .diff_removed        = ftxui::Color::RGB(255, 204, 204),
    .diff_added_word     = ftxui::Color::RGB( 51, 102, 204),
    .diff_removed_word   = ftxui::Color::RGB(153,  51,  51),
    .merged              = ftxui::Color::RGB(135,   0, 255),  // == autoAccept
    // Prompt chrome — light daltonized: bashBorder = TS lightDaltonizedTheme
    //   rgb(0,102,204) medium blue. promptBorder = light mid-gray.
    .bash_border         = ftxui::Color::RGB(  0, 102, 204),
    .prompt_border       = ftxui::Color::RGB(153, 153, 153),
    // TS lightDaltonizedTheme permission = rgb(51, 102, 255).
    .permission          = ftxui::Color::RGB( 51, 102, 255),
};

// Monochrome palette: for reduced-color / braille-only terminals.
inline const Palette monochrome = {
    .primary             = ftxui::Color::White,
    .primary_shimmer     = ftxui::Color::GrayLight,
    .info                = ftxui::Color::GrayLight,
    .success             = ftxui::Color::White,
    .warning             = ftxui::Color::White,
    .danger              = ftxui::Color::White,
    .muted               = ftxui::Color::GrayDark,
    .subtle              = ftxui::Color::GrayDark,
    .suggestion          = ftxui::Color::GrayLight,
    .text                = ftxui::Color::White,
    .inverse_text        = ftxui::Color::Black,
    .background          = ftxui::Color::Black,
    .chrome              = ftxui::Color::GrayDark,
    .rate_limit_fill     = ftxui::Color::White,
    .rate_limit_empty    = ftxui::Color::GrayDark,
    .brief_label         = ftxui::Color::White,
    .user_message_background        = ftxui::Color::GrayDark,
    .user_message_background_hover  = ftxui::Color::GrayLight,
    .message_actions_background     = ftxui::Color::GrayDark,
    .rainbow = {{ ftxui::Color::White, ftxui::Color::White, ftxui::Color::White,
                  ftxui::Color::White, ftxui::Color::White, ftxui::Color::White,
                  ftxui::Color::White }},
    .rainbow_shimmer     = ftxui::Color::GrayLight,
    .diff_added          = ftxui::Color::White,
    .diff_removed        = ftxui::Color::White,
    .diff_added_word     = ftxui::Color::White,
    .diff_removed_word   = ftxui::Color::White,
    .merged              = ftxui::Color::White,
    // Prompt chrome (monochrome): pure White for accent, GrayDark for frame.
    .bash_border         = ftxui::Color::White,
    .prompt_border       = ftxui::Color::GrayDark,
    .permission          = ftxui::Color::White,
};

} // namespace palette

// ─── Role → Color resolution ─────────────────────────────────────────────────
/// Resolve a semantic Role to its concrete ftxui::Color inside *palette*.
[[nodiscard]] inline ftxui::Color token_by_role(const Palette& pal, Role role) noexcept {
    switch (role) {
        case Role::Primary:    return pal.primary;
        case Role::Info:       return pal.info;
        case Role::Success:    return pal.success;
        case Role::Warning:    return pal.warning;
        case Role::Danger:     return pal.danger;
        case Role::Muted:      return pal.muted;
        case Role::Subtle:     return pal.subtle;
        case Role::Suggestion: return pal.suggestion;
        case Role::Permission: return pal.permission;
        case Role::RateLimit:  return pal.rate_limit_fill;
        case Role::Chrome:     return pal.chrome;
        case Role::Brief:      return pal.brief_label;
        case Role::Ultra:      return pal.rainbow_shimmer;
        case Role::UserMessageBackground:     return pal.user_message_background;
        case Role::UserMessageBackgroundHover:return pal.user_message_background_hover;
    }
    return pal.text;
}

// ─── Shading helpers ─────────────────────────────────────────────────────────
/// Lighten or darken an RGB color.  amount ∈ [-1, 1]; positive lightens.
/// If the input color is a palette index (not true RGB) it is returned
/// unchanged to avoid breaking ANSI-mode themes.
[[nodiscard]] inline ftxui::Color shade(ftxui::Color c, double amount) noexcept {
    // FTXUI's Color stores TrueColor as RGB inside .red()/.green()/.blue()
    // when Color::Print is not set.  The simplest portable approach is to
    // reconstruct via RGB() ourselves if the color is 24-bit capable.
    if (amount == 0.0) return c;

    // We only shade 24-bit colors.  Named / palette colors pass through.
    auto rgb = c.Print(false);
    // Heuristic: if the string is an "rgb(R,G,B)"-style output we can parse
    // it.  For simplicity do the shading via the FTXUI RGB constructor and
    // a manual clamp.
    // PHASE_5 NOTE: FTXUI v5 does not expose Color::red() accessors on the
    // public ABI; we therefore implement shading via re-parsing Print().
    // If the format is not "rgb(r,g,b)" just return c unmodified.
    if (rgb.size() < 6 || rgb.substr(0, 4) != "rgb(") return c;
    std::uint32_t r = 0, g = 0, b = 0;
    std::size_t i = 4;
    auto read_u8 = [&](std::uint32_t& out) {
        out = 0;
        while (i < rgb.size() && rgb[i] >= '0' && rgb[i] <= '9') {
            out = out * 10 + static_cast<std::uint32_t>(rgb[i] - '0');
            ++i;
        }
        if (i < rgb.size() && (rgb[i] == ',' || rgb[i] == ')')) ++i;
    };
    read_u8(r); read_u8(g); read_u8(b);

    auto apply = [amount](std::uint32_t v) -> std::uint8_t {
        double d = static_cast<double>(v);
        if (amount > 0) d = d + (255.0 - d) * amount;
        else            d = d * (1.0 + amount);
        if (d < 0)   d = 0;
        if (d > 255) d = 255;
        return static_cast<std::uint8_t>(std::lround(d));
    };
    return ftxui::Color::RGB(apply(r), apply(g), apply(b));
}

/// Shorthand: shade() with positive amount.
[[nodiscard]] inline ftxui::Color tint(ftxui::Color c, double amount) noexcept {
    return shade(c, std::clamp(amount, 0.0, 1.0));
}
[[nodiscard]] inline ftxui::Color tone(ftxui::Color c, double amount) noexcept {
    return shade(c, -std::clamp(amount, 0.0, 1.0));
}

// ─── Interpolate between two RGB colors (used by shimmer + FlashingChar) ────
/// Linearly interpolate between a and b.  t ∈ [0,1].  Falls back to `a` for
/// non-RGB inputs.  Mirrors FlashingChar.tsx::interpolateColor.
[[nodiscard]] inline ftxui::Color interpolate(ftxui::Color a, ftxui::Color b, double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    if (t == 0.0) return a;
    if (t == 1.0) return b;

    auto parse = [](ftxui::Color c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) -> bool {
        auto s = c.Print(false);
        if (s.size() < 6 || s.substr(0, 4) != "rgb(") return false;
        std::uint32_t rr = 0, gg = 0, bb = 0;
        std::size_t i = 4;
        auto rd = [&](std::uint32_t& out) {
            out = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                out = out * 10 + static_cast<std::uint32_t>(s[i] - '0');
                ++i;
            }
            if (i < s.size() && (s[i] == ',' || s[i] == ')')) ++i;
        };
        rd(rr); rd(gg); rd(bb);
        r = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(rr, 0, 255));
        g = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(gg, 0, 255));
        b = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(bb, 0, 255));
        return true;
    };

    std::uint8_t ar{}, ag{}, ab{}, br{}, bg{}, bb{};
    if (!parse(a, ar, ag, ab) || !parse(b, br, bg, bb)) return a;

    auto lerp = [t](std::uint8_t x, std::uint8_t y) -> std::uint8_t {
        double v = static_cast<double>(x) * (1.0 - t) + static_cast<double>(y) * t;
        return static_cast<std::uint8_t>(std::lround(v));
    };
    return ftxui::Color::RGB(lerp(ar, br), lerp(ag, bg), lerp(ab, bb));
}

// ─── HSL → RGB (used by AnimatedAsterisk hue sweep) ──────────────────────────
/// Simple HSL→RGB with S=0.70, L=0.60 tuned to match the TS animated asterisk
/// hue sweep.
[[nodiscard]] inline ftxui::Color hue_to_rgb(double hue_deg) noexcept {
    double h = std::fmod(hue_deg, 360.0) / 60.0;
    if (h < 0) h += 6.0;
    constexpr double s = 0.70;
    constexpr double l = 0.60;
    double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
    double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
    double m = l - c / 2.0;
    double r=0, g=0, b=0;
    if      (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else            { r = c; b = x; }
    auto to_u8 = [m](double v) -> std::uint8_t {
        double w = (v + m) * 255.0;
        if (w < 0)   w = 0;
        if (w > 255) w = 255;
        return static_cast<std::uint8_t>(std::lround(w));
    };
    return ftxui::Color::RGB(to_u8(r), to_u8(g), to_u8(b));
}

} // namespace cc::ui::design::tokens
