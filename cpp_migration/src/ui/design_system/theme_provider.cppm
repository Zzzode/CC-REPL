/// @file theme_provider.cppm
/// @brief Lightweight "theme provider" analog of the TS ThemeContext.
/// TS side reference:
///   src/components/design-system/ThemeProvider.tsx   – React context +
///       auto mode driven by an OSC-11 terminal bg watcher
///   src/components/design-system/ThemedBox.tsx       – resolveColor()
///   src/components/design-system/ThemedText.tsx      – dimColor → theme.inactive
///   src/utils/theme.ts                               – ThemeName enum
/// We intentionally do NOT carry the full 89-field TS Theme struct across the
/// FFI; the Provider here exposes the compact Palette + accessibility flags
/// that the design primitives actually consume.  Full field access remains
/// available via the legacy ui/design/themed_text.cppm module.
module;

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <mutex>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.design.theme;

import cc.ui.design.tokens;

export namespace cc::ui::design::theme {

using namespace cc::ui::design::tokens;

// ─── Theme variants ──────────────────────────────────────────────────────────
enum class ThemeVariant : std::uint8_t {
    Dark = 0,
    Light,
    DarkDaltonized,
    LightDaltonized,
    Monochrome,
    // auto mode uses terminal OSC-11 detection; for now equivalent to Dark.
    Auto = Dark,
};

// ─── Accessibility / environment overrides ───────────────────────────────────
struct Accessibility {
    bool reduced_motion    = false;   // suppress spinner shimmer, asterisk sweep
    bool high_contrast     = false;   // prefer stark palette (Monochrome if set)
    bool force_monochrome  = false;   // braille-only / e-ink terminals
};

// ─── Theme handle (value type; cheap to copy) ────────────────────────────────
struct Theme {
    ThemeVariant variant = ThemeVariant::Dark;
    const Palette* palette = &palette::dark;   // never null
    Accessibility a11y{};

    /// Convenience: resolve a semantic role through this theme's palette.
    [[nodiscard]] ftxui::Color color_for(Role r) const noexcept {
        return token_by_role(*palette, r);
    }
};

// ─── Theme selection helpers ─────────────────────────────────────────────────
/// Map a ThemeVariant → canonical static Palette pointer.  The returned
/// pointer lives for the duration of the program.
[[nodiscard]] inline const Palette* palette_for_variant(ThemeVariant v) noexcept {
    switch (v) {
        case ThemeVariant::Dark:            return &palette::dark;
        case ThemeVariant::Light:           return &palette::light;
        case ThemeVariant::DarkDaltonized:  return &palette::dark_daltonized;
        case ThemeVariant::LightDaltonized: return &palette::light_daltonized;
        case ThemeVariant::Monochrome:      return &palette::monochrome;
    }
    return &palette::dark;
}

/// Parse a TS-side ThemeName string ("dark", "light-daltonized", …).
/// Returns Dark on unrecognized input (matches TS getTheme() fallback).
[[nodiscard]] inline ThemeVariant parse_variant(std::string_view name) noexcept {
    if (name == "dark")                     return ThemeVariant::Dark;
    if (name == "light")                    return ThemeVariant::Light;
    if (name == "dark-daltonized")          return ThemeVariant::DarkDaltonized;
    if (name == "light-daltonized")         return ThemeVariant::LightDaltonized;
    if (name == "monochrome" || name == "ansi") return ThemeVariant::Monochrome;
    if (name == "auto")                     return ThemeVariant::Auto;
    return ThemeVariant::Dark;
}

[[nodiscard]] inline std::string_view variant_name(ThemeVariant v) noexcept {
    switch (v) {
        case ThemeVariant::Dark:            return "dark";
        case ThemeVariant::Light:           return "light";
        case ThemeVariant::DarkDaltonized:  return "dark-daltonized";
        case ThemeVariant::LightDaltonized: return "light-daltonized";
        case ThemeVariant::Monochrome:      return "monochrome";
    }
    return "dark";
}

// ─── Global mutable provider (replaces React Context) ────────────────────────
// The design-system primitives are pure functions of `const Theme&`; the
// provider simply exposes a thread-local default.  Coordinators that need to
// drive preview themes (ThemeProvider.tsx preview mode) should keep their own
// `Theme` value and pass it explicitly to each primitive.
namespace detail {

struct ThemeHolder {
    std::mutex mu;
    Theme current{};
};

inline ThemeHolder& global_holder() noexcept {
    // Lazily initialized so the static storage is alive by the time the
    // provider is touched from any translation unit.
    static ThemeHolder h;
    return h;
}

} // namespace detail

[[nodiscard]] inline Theme current_theme() noexcept {
    auto& h = detail::global_holder();
    std::lock_guard lk(h.mu);
    return h.current;
}

inline void set_theme(ThemeVariant v, Accessibility a = {}) noexcept {
    auto& h = detail::global_holder();
    std::lock_guard lk(h.mu);
    h.current.variant = v;
    if (a.force_monochrome) {
        h.current.palette = &palette::monochrome;
    } else {
        h.current.palette = palette_for_variant(v);
    }
    h.current.a11y = std::move(a);
}

inline void set_theme(Theme t) noexcept {
    auto& h = detail::global_holder();
    std::lock_guard lk(h.mu);
    h.current = std::move(t);
}

// ─── resolve_color (mirrors ThemedBox.tsx) ───────────────────────────────────
/// Given a color string of unknown provenance — either a literal form like
/// "rgb(r,g,b)" / "#RRGGBB" / "ansi:code" / "ansi256(n)" OR a Theme key such
/// as "claude", "success" — return the corresponding ftxui::Color.  Unknown
/// key names fall back to theme.text.
[[nodiscard]] inline ftxui::Color resolve_color(std::string_view color,
                                               const Theme& theme) noexcept {
    if (color.empty()) return {};  // caller decides what "no color" means

    // ── literal forms ─────────────────────────────────────────────────────
    if (color.size() >= 4 && color.substr(0, 4) == "rgb(") {
        // parse rgb(r,g,b)
        std::size_t i = 4;
        auto rd = [&](std::uint32_t& out) {
            out = 0;
            while (i < color.size() && color[i] >= '0' && color[i] <= '9') {
                out = out * 10 + static_cast<std::uint32_t>(color[i] - '0');
                ++i;
            }
            if (i < color.size() && (color[i] == ',' || color[i] == ' '
                                     || color[i] == ')')) ++i;
        };
        std::uint32_t r=0,g=0,b=0;
        rd(r); rd(g); rd(b);
        return ftxui::Color::RGB(static_cast<std::uint8_t>(r&0xff),
                                 static_cast<std::uint8_t>(g&0xff),
                                 static_cast<std::uint8_t>(b&0xff));
    }
    if (color.front() == '#' && color.size() == 7) {
        auto nibble = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return 0;
        };
        return ftxui::Color::RGB(
            static_cast<std::uint8_t>((nibble(color[1]) << 4) | nibble(color[2])),
            static_cast<std::uint8_t>((nibble(color[3]) << 4) | nibble(color[4])),
            static_cast<std::uint8_t>((nibble(color[5]) << 4) | nibble(color[6]))
        );
    }
    if (color.size() >= 5 && color.substr(0, 5) == "ansi:") {
        // ansi:N  —  palette16 index (0..15)
        int n = 0;
        for (std::size_t i = 5; i < color.size(); ++i) {
            if (color[i] >= '0' && color[i] <= '9') n = n*10 + (color[i]-'0');
        }
        return ftxui::Color{static_cast<ftxui::Color::Palette16>(n & 0xf)};
    }
    if (color.size() >= 8 && color.substr(0, 7) == "ansi256") {
        int n = 0;
        for (char c : color) {
            if (c >= '0' && c <= '9') n = n*10 + (c - '0');
        }
        return ftxui::Color{static_cast<ftxui::Color::Palette256>(n & 0xff)};
    }

    // ── Theme key lookup (subset of the 89 TS fields we actually use) ────
    auto& p = *theme.palette;
    if (color == "claude" || color == "clawd_body" || color == "primary")
        return p.primary;
    if (color == "claudeShimmer" || color == "primary_shimmer")
        return p.primary_shimmer;
    if (color == "success") return p.success;
    if (color == "error" || color == "danger") return p.danger;
    if (color == "warning") return p.warning;
    if (color == "permission") return p.permission;
    if (color == "info") return p.info;
    if (color == "inactive" || color == "muted") return p.muted;
    if (color == "subtle") return p.subtle;
    if (color == "suggestion") return p.suggestion;
    if (color == "text")   return p.text;
    if (color == "inverseText") return p.inverse_text;
    if (color == "background") return p.background;
    if (color == "chrome") return p.chrome;
    if (color == "merged") return p.merged;
    if (color == "diffAdded")  return p.diff_added;
    if (color == "diffRemoved") return p.diff_removed;
    if (color == "diffAddedWord") return p.diff_added_word;
    if (color == "diffRemovedWord") return p.diff_removed_word;
    // ── New tokens (GAP: clr-missing-42-tokens-struct + clr-shimmer-tokens-missing)
    // TS REF: src/utils/theme.ts Theme type full field list
    if (color == "permissionShimmer" || color == "permission_shimmer")
        return p.permission_shimmer;
    if (color == "inactiveShimmer" || color == "inactive_shimmer")
        return p.inactive_shimmer;
    if (color == "warningShimmer" || color == "warning_shimmer")
        return p.warning_shimmer;
    if (color == "promptBorderShimmer" || color == "prompt_border_shimmer")
        return p.prompt_border_shimmer;
    if (color == "fastModeShimmer" || color == "fast_mode_shimmer")
        return p.fast_mode_shimmer;
    if (color == "claudeBlueShimmer_FOR_SYSTEM_SPINNER" || color == "claude_blue_shimmer")
        return p.claude_blue_shimmer;
    if (color == "claudeBlue_FOR_SYSTEM_SPINNER" || color == "claude_blue")
        return p.claude_blue;
    if (color == "planMode" || color == "plan_mode")
        return p.plan_mode;
    if (color == "ide")
        return p.ide;
    if (color == "remember")
        return p.remember;
    if (color == "fastMode" || color == "fast_mode")
        return p.fast_mode;
    if (color == "diffAddedDimmed" || color == "diff_added_dimmed")
        return p.diff_added_dimmed;
    if (color == "diffRemovedDimmed" || color == "diff_removed_dimmed")
        return p.diff_removed_dimmed;
    if (color == "red_FOR_SUBAGENTS_ONLY" || color == "subagent_red")
        return p.subagent_red;
    if (color == "blue_FOR_SUBAGENTS_ONLY" || color == "subagent_blue")
        return p.subagent_blue;
    if (color == "green_FOR_SUBAGENTS_ONLY" || color == "subagent_green")
        return p.subagent_green;
    if (color == "yellow_FOR_SUBAGENTS_ONLY" || color == "subagent_yellow")
        return p.subagent_yellow;
    if (color == "purple_FOR_SUBAGENTS_ONLY" || color == "subagent_purple")
        return p.subagent_purple;
    if (color == "orange_FOR_SUBAGENTS_ONLY" || color == "subagent_orange")
        return p.subagent_orange;
    if (color == "pink_FOR_SUBAGENTS_ONLY" || color == "subagent_pink")
        return p.subagent_pink;
    if (color == "cyan_FOR_SUBAGENTS_ONLY" || color == "subagent_cyan")
        return p.subagent_cyan;
    if (color == "professionalBlue" || color == "professional_blue")
        return p.professional_blue;
    if (color == "chromeYellow" || color == "chrome_yellow")
        return p.chrome_yellow;
    if (color == "clawd_background")
        return p.clawd_background;
    if (color == "selectionBg" || color == "selection_bg")
        return p.selection_bg;
    if (color == "bashMessageBackgroundColor" || color == "bash_message_background")
        return p.bash_message_background;
    if (color == "memoryBackgroundColor" || color == "memory_background")
        return p.memory_background;
    if (color == "briefLabelYou" || color == "brief_label_you")
        return p.brief_label_you;
    if (color == "briefLabelClaude" || color == "brief_label_claude")
        return p.brief_label_claude;
    // Rainbow per-stop shimmers
    if (color == "rainbow_red_shimmer")    return p.rainbow_shimmer_stops[0];
    if (color == "rainbow_orange_shimmer") return p.rainbow_shimmer_stops[1];
    if (color == "rainbow_yellow_shimmer") return p.rainbow_shimmer_stops[2];
    if (color == "rainbow_green_shimmer")  return p.rainbow_shimmer_stops[3];
    if (color == "rainbow_blue_shimmer")   return p.rainbow_shimmer_stops[4];
    if (color == "rainbow_indigo_shimmer") return p.rainbow_shimmer_stops[5];
    if (color == "rainbow_violet_shimmer") return p.rainbow_shimmer_stops[6];

    return p.text;
}

// ─── Apply a theme role to a raw ftxui::Component (catch-event-style wrapper)
// PHASE_5 NOTE: FTXUI does not have a React-style context provider; instead,
// callers can use with_theme_role(comp, role) to wrap a component renderer
// so that every repaint pulls a fresh color from current_theme().
[[nodiscard]] inline ftxui::Component with_theme_role(ftxui::Component inner,
                                                      Role role) {
    // No direct way to "apply a theme" to an arbitrary component; instead we
    // return the inner unchanged and document that the component must itself
    // call current_theme() during Render().  A stricter version could inject
    // an OnAnimation step, but that would require holding the component on
    // the heap via make_ref, which FTXUI discourages.
    (void)role;
    return inner;
}

// ─── surface_box: wrap an element in a chrome-styled border + bg ─────────────
/// Produces a "surface" container matching TS ThemedBox: border color from
/// theme.chrome, background from tint(theme.background, 0.02), and the
/// rounded border style used in the majority of the REPL surfaces.
[[nodiscard]] inline ftxui::Element surface_box(ftxui::Element inner,
                                                const Theme& theme,
                                                tokens::Radius r = tokens::Radius::Rounded,
                                                Role border_role = Role::Chrome) {
    auto bc = theme.color_for(border_role);
    switch (r) {
        case tokens::Radius::None:
            return inner | ftxui::bgcolor(theme.palette->background);
        case tokens::Radius::Light:
            return inner | ftxui::borderLight | ftxui::color(bc)
                   | ftxui::bgcolor(theme.palette->background);
        case tokens::Radius::Rounded:
            return inner | ftxui::borderRounded | ftxui::color(bc)
                   | ftxui::bgcolor(theme.palette->background);
        case tokens::Radius::Double:
            return inner | ftxui::borderDouble | ftxui::color(bc)
                   | ftxui::bgcolor(theme.palette->background);
        case tokens::Radius::Heavy:
            return inner | ftxui::borderHeavy | ftxui::color(bc)
                   | ftxui::bgcolor(theme.palette->background);
    }
    return inner;
}

} // namespace cc::ui::design::theme

// ─── Demo stub (compile-time only under CC_DESIGN_SYSTEM_DEMO) ───────────────
#ifdef CC_DESIGN_SYSTEM_DEMO
export namespace cc::ui::design::theme::demo {
inline std::string demo_theme_names() {
    std::string out;
    auto add = [&](ThemeVariant v) {
        if (!out.empty()) out += ", ";
        out += variant_name(v);
    };
    add(ThemeVariant::Dark);
    add(ThemeVariant::Light);
    add(ThemeVariant::DarkDaltonized);
    add(ThemeVariant::LightDaltonized);
    add(ThemeVariant::Monochrome);
    return out;
}
}
#endif
