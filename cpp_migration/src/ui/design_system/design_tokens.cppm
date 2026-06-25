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
    Info,         // Permission / prompt-border info
    Success,      // Green
    Warning,      // Yellow / amber
    Danger,       // Error / rejection
    Muted,        // Inactive / dim (replaces ANSI dim per ThemedText.tsx)
    Subtle,       // Subtle foreground (chrome, borders)
    Suggestion,   // Typeahead suggestion foreground
    RateLimit,    // Rate-limit pill fill
    Chrome,       // UI chrome / background-adjacent lines
    Brief,        // Brief-mode labels
    Ultra,        // Ultra-thinking rainbow head
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
    // rainbow cycle used by ultra-thinking (7 steps)
    std::array<ftxui::Color, 7> rainbow;
    ftxui::Color rainbow_shimmer;
    // diff tokens
    ftxui::Color diff_added;
    ftxui::Color diff_removed;
    ftxui::Color diff_added_word;
    ftxui::Color diff_removed_word;
    ftxui::Color merged;
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
    .info                = ftxui::Color::RGB(110, 151, 255),
    .success             = ftxui::Color::RGB( 78, 186, 101),
    .warning             = ftxui::Color::RGB(255, 193,   7),
    .danger              = ftxui::Color::RGB(255, 107, 128),
    .muted               = ftxui::Color::RGB(153, 153, 153),
    .subtle              = ftxui::Color::RGB( 87,  87,  87),
    .suggestion          = ftxui::Color::RGB(173, 216, 255),
    .text                = ftxui::Color::RGB(230, 237, 243),
    .inverse_text        = ftxui::Color::RGB( 32,  33,  36),
    .background          = ftxui::Color::RGB( 32,  33,  36),
    .chrome              = ftxui::Color::RGB( 55,  57,  61),
    .rate_limit_fill     = ftxui::Color::RGB( 80, 140, 255),
    .rate_limit_empty    = ftxui::Color::RGB( 60,  60,  80),
    .brief_label         = CLAWDED,
    .rainbow = {{
        ftxui::Color::RGB(255,  99, 132),
        ftxui::Color::RGB(255, 159,  64),
        ftxui::Color::RGB(255, 205,  86),
        ftxui::Color::RGB( 75, 192, 192),
        ftxui::Color::RGB( 54, 162, 235),
        ftxui::Color::RGB(153, 102, 255),
        ftxui::Color::RGB(201, 203, 207),
    }},
    .rainbow_shimmer     = ftxui::Color::RGB(230, 230, 250),
    .diff_added          = ftxui::Color::RGB( 46, 160,  67),
    .diff_removed        = ftxui::Color::RGB(248,  81,  73),
    .diff_added_word     = ftxui::Color::RGB( 63, 185,  80),
    .diff_removed_word   = ftxui::Color::RGB(250, 109,  93),
    .merged              = ftxui::Color::RGB(163, 113, 247),
};

inline const Palette light = {
    .primary             = CLAWDED,
    .primary_shimmer     = CLAWDED_SHIM,
    .info                = ftxui::Color::RGB( 53, 113, 232),
    .success             = ftxui::Color::RGB( 40, 150,  70),
    .warning             = ftxui::Color::RGB(200, 140,  10),
    .danger              = ftxui::Color::RGB(210,  60,  80),
    .muted               = ftxui::Color::RGB(120, 120, 120),
    .subtle              = ftxui::Color::RGB(190, 190, 190),
    .suggestion          = ftxui::Color::RGB( 30,  90, 180),
    .text                = ftxui::Color::RGB( 32,  33,  36),
    .inverse_text        = ftxui::Color::RGB(245, 245, 245),
    .background          = ftxui::Color::RGB(245, 245, 245),
    .chrome              = ftxui::Color::RGB(220, 220, 220),
    .rate_limit_fill     = ftxui::Color::RGB( 53, 113, 232),
    .rate_limit_empty    = ftxui::Color::RGB(225, 230, 240),
    .brief_label         = CLAWDED,
    .rainbow = dark.rainbow,
    .rainbow_shimmer     = ftxui::Color::RGB(120,  80, 200),
    .diff_added          = ftxui::Color::RGB( 38, 130,  60),
    .diff_removed        = ftxui::Color::RGB(210,  50,  45),
    .diff_added_word     = ftxui::Color::RGB( 50, 160,  75),
    .diff_removed_word   = ftxui::Color::RGB(230,  80,  70),
    .merged              = ftxui::Color::RGB(120,  60, 210),
};

// Daltonized variants: desaturated, contrast-boosted for CVD users.
// Values are derived from dark/light above using standard daltonize matrices.
inline const Palette dark_daltonized = {
    .primary             = ftxui::Color::RGB(198, 128,  80),
    .primary_shimmer     = ftxui::Color::RGB(225, 170, 130),
    .info                = ftxui::Color::RGB(100, 140, 230),
    .success             = ftxui::Color::RGB( 90, 150, 200), // blue-ish green
    .warning             = ftxui::Color::RGB(210, 170,  40),
    .danger              = ftxui::Color::RGB(200, 120, 140),
    .muted               = dark.muted,
    .subtle              = dark.subtle,
    .suggestion          = dark.suggestion,
    .text                = dark.text,
    .inverse_text        = dark.inverse_text,
    .background          = dark.background,
    .chrome              = dark.chrome,
    .rate_limit_fill     = dark.rate_limit_fill,
    .rate_limit_empty    = dark.rate_limit_empty,
    .brief_label         = ftxui::Color::RGB(198, 128,  80),
    .rainbow = {{
        ftxui::Color::RGB(190, 130, 140),
        ftxui::Color::RGB(205, 160,  90),
        ftxui::Color::RGB(210, 190,  90),
        ftxui::Color::RGB(110, 170, 180),
        ftxui::Color::RGB( 90, 140, 200),
        ftxui::Color::RGB(140, 120, 220),
        ftxui::Color::RGB(201, 203, 207),
    }},
    .rainbow_shimmer     = dark.rainbow_shimmer,
    .diff_added          = ftxui::Color::RGB( 90, 130, 180),
    .diff_removed        = ftxui::Color::RGB(180, 110, 120),
    .diff_added_word     = ftxui::Color::RGB(110, 160, 200),
    .diff_removed_word   = ftxui::Color::RGB(200, 130, 140),
    .merged              = ftxui::Color::RGB(150, 120, 220),
};

inline const Palette light_daltonized = {
    .primary             = dark_daltonized.primary,
    .primary_shimmer     = dark_daltonized.primary_shimmer,
    .info                = ftxui::Color::RGB( 50, 100, 200),
    .success             = ftxui::Color::RGB( 60, 120, 170),
    .warning             = ftxui::Color::RGB(180, 140,  20),
    .danger              = ftxui::Color::RGB(180,  90, 110),
    .muted               = light.muted,
    .subtle              = light.subtle,
    .suggestion          = light.suggestion,
    .text                = light.text,
    .inverse_text        = light.inverse_text,
    .background          = light.background,
    .chrome              = light.chrome,
    .rate_limit_fill     = light.rate_limit_fill,
    .rate_limit_empty    = light.rate_limit_empty,
    .brief_label         = dark_daltonized.primary,
    .rainbow             = dark_daltonized.rainbow,
    .rainbow_shimmer     = light.rainbow_shimmer,
    .diff_added          = ftxui::Color::RGB( 70, 120, 170),
    .diff_removed        = ftxui::Color::RGB(170,  80,  95),
    .diff_added_word     = ftxui::Color::RGB( 90, 140, 190),
    .diff_removed_word   = ftxui::Color::RGB(190, 100, 115),
    .merged              = ftxui::Color::RGB(120, 100, 200),
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
    .rainbow = {{ ftxui::Color::White, ftxui::Color::White, ftxui::Color::White,
                  ftxui::Color::White, ftxui::Color::White, ftxui::Color::White,
                  ftxui::Color::White }},
    .rainbow_shimmer     = ftxui::Color::GrayLight,
    .diff_added          = ftxui::Color::White,
    .diff_removed        = ftxui::Color::White,
    .diff_added_word     = ftxui::Color::White,
    .diff_removed_word   = ftxui::Color::White,
    .merged              = ftxui::Color::White,
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
        case Role::RateLimit:  return pal.rate_limit_fill;
        case Role::Chrome:     return pal.chrome;
        case Role::Brief:      return pal.brief_label;
        case Role::Ultra:      return pal.rainbow_shimmer;
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
/// Simple HSL→RGB with S=0.70, L=0.60 tuned to match the TS AnimatedAsterisk
/// hue sweep (src/components/LogoV2/AnimatedAsterisk.tsx uses ~s=0.70, l=0.60).
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
