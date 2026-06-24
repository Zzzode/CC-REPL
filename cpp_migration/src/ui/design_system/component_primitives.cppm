/// @file component_primitives.cppm
/// @brief Low-level UI building blocks built on FTXUI Elements.
/// TS-side references audited:
///   src/components/design-system/{Divider,ThemedBox,ThemedText}.tsx
///   src/components/Spinner/SpinnerGlyph.tsx    — braille spinner frames
///   src/components/Spinner/FlashingChar.tsx    — color interpolation shimmer
///   src/utils/theme.ts                         — semantic roles used here
/// N/A during audit: src/components/design-system/StatusBadge.tsx (not
/// present); the equivalent here is `status_pill()` using the same Role enum.
module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.design.primitives;

import cc.ui.design.tokens;
import cc.ui.design.theme;

export namespace cc::ui::design::primitives {

using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── heading / subheading / caption ──────────────────────────────────────────
/// Large heading.  On a TTY "size" maps to bold/dim + separator decoration,
/// since we cannot scale the font itself.  Mirrors TS ThemedText + separator.
[[nodiscard]] inline ftxui::Element heading(std::string_view label,
                                            TextSize size,
                                            const Theme& theme) {
    using namespace ftxui;
    Element body = ftxui::text(std::string(label)) | color(theme.palette->text);
    switch (size) {
        case TextSize::Display:
            return vbox({
                body | bold,
                separator()
                    | color(Color::RGB(120, 80, 120))
                    | color(theme.palette->primary),
            });
        case TextSize::Title:
            return vbox({
                body | bold,
                separator() | color(theme.palette->subtle),
            });
        case TextSize::Subtitle:
            return body | bold;
        case TextSize::Body:
            return body;
        case TextSize::Caption:
            break;
    }
    return body | dim | color(theme.palette->muted);
}

[[nodiscard]] inline ftxui::Element subheading(std::string_view label,
                                               const Theme& theme) {
    return heading(label, TextSize::Subtitle, theme);
}
[[nodiscard]] inline ftxui::Element caption(std::string_view label,
                                            const Theme& theme) {
    return heading(label, TextSize::Caption, theme);
}

// ─── pill ────────────────────────────────────────────────────────────────────
/// Rounded pill badge with optional count suffix.  Used by agent avatars,
/// turn-durations, background task footer pills (see pillLabel.ts).
[[nodiscard]] inline ftxui::Element pill(std::string_view label,
                                         Role role,
                                         const Theme& theme,
                                         bool dim = false) {
    using namespace ftxui;
    auto fg = theme.color_for(role);
    auto bg = tint(theme.palette->background, 0.03);
    Element body = ftxui::text(std::string(label)) | color(fg);
    if (dim) body = body | color(theme.palette->muted);
    // "rounded" pill look: borderRounded with 1-cell padding
    return hbox({
        text(" "), body, text(" ")
    }) | borderRounded | color(fg) | bgcolor(bg);
}

// ─── kbd ─────────────────────────────────────────────────────────────────────
/// Render a keyboard-shortcut hint (⌘K style).  Mirrors Ink <Kbd>.
[[nodiscard]] inline ftxui::Element kbd(std::string_view label) {
    using namespace ftxui;
    return hbox({
        ftxui::text(" "),
        ftxui::text(std::string(label)) | inverted | bold,
        ftxui::text(" "),
    });
}

// ─── badge_counter ───────────────────────────────────────────────────────────
/// Small circular badge with an integer count (e.g. unread inbox, background
/// task count).  Falls back to a "[n]" prefix if circle rendering is poor.
[[nodiscard]] inline ftxui::Element badge_counter(std::uint32_t count,
                                                  const Theme& theme,
                                                  Role role = Role::Danger) {
    using namespace ftxui;
    if (count == 0) return text("") | size(WIDTH, EQUAL, 0);
    std::string label = count > 99 ? "99+" : std::format("{}", count);
    auto fg = Color::White;
    auto bg = theme.color_for(role);
    return hbox({
        text(" "),
        text(label) | color(fg) | bgcolor(bg) | bold,
        text(" "),
    }) | color(bg);
}

// ─── divider ─────────────────────────────────────────────────────────────────
/// Mirrors Divider.tsx: either a light '─' line or heavy '━' bar, with an
/// optional centered title.  Heavy bar is taken from ui/components/figures.
[[nodiscard]] inline ftxui::Element divider(bool heavy = false,
                                            std::string_view title = {},
                                            ftxui::Color line_color = ftxui::Color::Default) {
    using namespace ftxui;
    std::string glyph = heavy ? "━" : "─";
    auto line = [&](int n) {
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s += glyph;
        return s;
    };
    // Compare line_color against default (unset) to decide whether to
    // override with a fallback.  We use Print()=="default" as a proxy since
    // FTXUI Color has no IsNot/operator bool.
    const bool has_color = (line_color.Print(false) != "default");
    const Color effective_line = has_color ? line_color : Color(Color::GrayDark);
    const Color effective_title = has_color ? line_color : Color(Color::White);
    if (title.empty()) {
        auto e = ftxui::text(line(40)) | xflex;
        return has_color ? e | ftxui::color(line_color)
                         : e | ftxui::color(Color::GrayDark);
    }
    // centered: ══[ Title ]══════════
    auto label = std::string(" ") + std::string(title) + std::string(" ");
    auto e = hbox({
        ftxui::text(line(3)) | ftxui::color(effective_line),
        ftxui::text(label) | bold | ftxui::color(effective_title),
        ftxui::text(line(40)) | xflex | ftxui::color(effective_line),
    });
    return e;
}

// ─── spinner (braille glyph) ─────────────────────────────────────────────────
/// Mirrors SpinnerGlyph.tsx braille spinner.  10 frames; pass an external
/// frame counter so the caller can control the tick rate.
inline constexpr std::array<std::string_view, 10> k_spinner_frames = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};

[[nodiscard]] inline ftxui::Element spinner_glyph(std::uint64_t frame,
                                                  Role role,
                                                  const Theme& theme,
                                                  bool stall = false) {
    using namespace ftxui;
    auto base = theme.color_for(role);
    if (stall) {
        // stalled → interpolate toward TS ERROR_RED rgb(171,43,63)
        base = interpolate(base, Color::RGB(171, 43, 63), 0.75);
    }
    if (theme.a11y.reduced_motion) {
        return text("●") | color(base);
    }
    const auto idx = frame % k_spinner_frames.size();
    return text(std::string(k_spinner_frames[idx])) | color(base);
}

// ─── flashing_char (mirrors FlashingChar.tsx) ────────────────────────────────
/// Single character shimmering between two role colors.  `flash_opacity` ∈
/// [0,1] drives the interpolation.
[[nodiscard]] inline ftxui::Element flashing_char(char ch,
                                                  double flash_opacity,
                                                  Role base_role,
                                                  Role shimmer_role,
                                                  const Theme& theme) {
    using namespace ftxui;
    auto a = theme.color_for(base_role);
    auto b = theme.color_for(shimmer_role);
    auto t = std::clamp(flash_opacity, 0.0, 1.0);
    auto c = interpolate(a, b, t);
    return text(std::string(1, ch)) | color(c);
}

// ─── progress_bar ────────────────────────────────────────────────────────────
/// Gauge progress bar with optional label and role-driven color.
[[nodiscard]] inline ftxui::Element progress_bar(double progress,
                                                 Role role,
                                                 const Theme& theme,
                                                 std::string_view label = {}) {
    using namespace ftxui;
    auto p = std::clamp(progress, 0.0, 1.0);
    auto fg = theme.color_for(role);
    Element bar = gauge(p) | color(fg);
    Element pct = text(std::format(" {:3.0f}%", p * 100.0))
                  | color(theme.palette->muted);
    if (label.empty()) return hbox({ bar | xflex, pct });
    Element lbl = text(std::string(label) + " ") | color(theme.palette->subtle);
    return hbox({ lbl, bar | xflex, pct });
}

// ─── status_pill ─────────────────────────────────────────────────────────────
/// String-based status → pill.  Mirrors the ad-hoc status badge used in TS
/// sidebar panels and agent heads.
[[nodiscard]] inline ftxui::Element status_pill(std::string_view status,
                                                const Theme& theme) {
    Role role = Role::Muted;
    std::string label(status);
    if (status == "ok" || status == "connected" || status == "success") {
        role = Role::Success;
    } else if (status == "warn" || status == "stalled" || status == "idle") {
        role = Role::Warning;
    } else if (status == "error" || status == "disconnected" || status == "denied") {
        role = Role::Danger;
    } else if (status == "info" || status == "pending") {
        role = Role::Info;
    } else if (status == "running") {
        role = Role::Primary;
    } else {
        // keep defaults, use raw label
    }
    return pill(label, role, theme, false);
}

// ─── Spinner widget (FTXUI Component) ────────────────────────────────────────
/// Full spinner component: drives its own frame counter via periodic Event.
/// Callers mount this in an FTXUI ScreenInteractive loop; the component
/// self-posts animation frames every easing::spinner_frame_interval ms.
class SpinnerBase : public ftxui::ComponentBase {
public:
    SpinnerBase(Role role, Theme theme, std::string_view label = "")
        : role_(role), theme_(std::move(theme)), label_(label),
          start_(std::chrono::steady_clock::now()) {}

    ftxui::Element OnRender() override {
        using namespace ftxui;
        auto elapsed = std::chrono::steady_clock::now() - start_;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        auto frame = static_cast<std::uint64_t>(ms) /
                     static_cast<std::uint64_t>(easing::spinner_frame_interval);
        auto g = spinner_glyph(frame, role_, theme_, stall_);
        Element line = hbox({ g, text(" "), text(label_) | color(theme_.palette->text) });
        if (stall_) line = line | dim;
        return line;
    }

    void set_stall(bool s) { stall_ = s; }

private:
    Role role_;
    Theme theme_;
    std::string label_;
    bool stall_ = false;
    std::chrono::steady_clock::time_point start_;
};

[[nodiscard]] inline ftxui::Component make_spinner(
    Role role, Theme theme = current_theme(), std::string_view label = {}) {
    return ftxui::Make<SpinnerBase>(role, std::move(theme), label);
}

// ─── shimmer line (mirrors ui/prompt/shimmer.tsx) ────────────────────────────
/// A single-line shimmer used for thinking / loading placeholders.  Draws a
/// gradient line from role → shimmer role using repeating "▓" blocks.
[[nodiscard]] inline ftxui::Element shimmer_line(Role role,
                                                 const Theme& theme,
                                                 int width = 24,
                                                 double t = 0) {
    using namespace ftxui;
    std::vector<Element> parts;
    parts.reserve(width);
    auto a = theme.color_for(role);
    auto b = tint(theme.color_for(role), 0.4);
    for (int i = 0; i < width; ++i) {
        double phase = std::fmod(static_cast<double>(i) / width + t, 1.0);
        // triangular wave: stronger in the middle
        double tt = 1.0 - std::fabs(phase - 0.5) * 2.0;
        auto c = interpolate(b, a, tt);
        parts.push_back(text("▓") | color(c));
    }
    return hbox(std::move(parts));
}

} // namespace cc::ui::design::primitives

#ifdef CC_DESIGN_SYSTEM_DEMO
export namespace cc::ui::design::primitives::demo {
inline int demo_spinner_frame_count() {
    return static_cast<int>(cc::ui::design::primitives::k_spinner_frames.size());
}
inline std::string demo_status_lookup() {
    // quick sanity list of supported status names
    return "ok/warn/error/info/idle/running/pending/stalled/connected/disconnected/denied";
}
}
#endif
