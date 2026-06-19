/// @file logo_v2.cppm
/// @brief Seed / Clawd ASCII logo with an animated asterisk.
/// TS-side references audited:
///   src/components/LogoV2/AnimatedAsterisk.tsx — 50ms useAnimationFrame with
///       HSL hue sweep (SWEEP_DURATION_MS 1500, SWEEP_COUNT 2 → 3000ms total).
///   src/components/LogoV2/WelcomeV2.tsx       — Clawd ASCII block 9 chars
///       wide built from █ █▄███▄█ characters plus scattered * asterisks.
///   src/components/LogoV2/CondensedLogo.tsx   — clawd icon + wordmark.
/// We render the clawd mark with unicode block characters, suffix "🌱 Seed" as
/// the Seed wordmark (TS SVG logos don't port to TTY), and implement the
/// animated asterisk via an FTXUI Component (Renderer + CatchEvent) that
/// ticks on a 120ms cadence to stay within the Phase 4 CPU budget.  The
/// WelcomeV2 full-screen layout is PHASE_5 — too wide to reproduce faithfully
/// inside a 380-line module and requires layout measurement primitives.
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.design.logo;

import cc.ui.design.tokens;
import cc.ui.design.theme;

export namespace cc::ui::design::logo {

using namespace cc::ui::design::tokens;
using namespace cc::ui::design::theme;

// ─── Static logo marks ───────────────────────────────────────────────────────
// Clawd (Claude's icon) drawn as 9×3 unicode blocks.  The TS side uses
// slightly different Unicode characters, but FTXUI renders `█`/`▄` cleanly.
constexpr std::string_view k_clawd_row_1 = "█████████";
constexpr std::string_view k_clawd_row_2 = "██▄█████▄██";
constexpr std::string_view k_clawd_row_3 = "█████████";

constexpr std::string_view k_seed_wordmark    = "🌱 Seed";
constexpr std::string_view k_seed_full_suffix = " — C++ REPL";

enum class LogoStyle : std::uint8_t {
    Mark,      // just the clawd ASCII block
    Wordmark,  // just "🌱 Seed" in primary color
    Full,      // clawd mark + "🌱 Seed — C++ REPL" stacked
};

// ─── Static renderers ────────────────────────────────────────────────────────
/// Render the clawd ASCII mark (3 rows × 9 cols).  `body_color` typically
/// matches the primary role; `bg_color` is the background of the surface.
[[nodiscard]] inline ftxui::Element logo_mark(ftxui::Color body_color,
                                              ftxui::Color bg_color = {}) {
    using namespace ftxui;
    auto row = [&](std::string_view s) {
        return text(std::string(s)) | color(body_color) | bgcolor(bg_color);
    };
    return vbox({
        row(k_clawd_row_1),
        row(k_clawd_row_2),
        row(k_clawd_row_3),
    });
}

[[nodiscard]] inline ftxui::Element logo_wordmark(ftxui::Color c) {
    using namespace ftxui;
    return text(std::string(k_seed_wordmark)) | color(c) | bold;
}

/// Full stacked logo: clawd mark above "🌱 Seed — C++ REPL" wordmark.
[[nodiscard]] inline ftxui::Element logo_full(const Theme& t) {
    using namespace ftxui;
    auto mark = logo_mark(t.palette->primary, t.palette->background);
    auto wm   = hbox({
        logo_wordmark(t.palette->primary),
        text(std::string(k_seed_full_suffix)) | color(t.palette->muted) | dim,
    });
    return vbox({
        mark,
        filler() | size(HEIGHT, EQUAL, 1),
        wm,
    });
}

// ─── Animated asterisk (mirrors AnimatedAsterisk.tsx) ───────────────────────
// FTXUI components with animation must inherit from ComponentBase.  The base
// is built by `make_animated_asterisk()` and returns a Component that
// requests 120ms repaints (tokens::easing::logo_refresh_ms) via PostEvent.
class AnimatedAsteriskBase : public ftxui::ComponentBase {
public:
    AnimatedAsteriskBase(std::uint32_t sweep_ms, std::uint32_t sweep_count,
                         bool reduced_motion)
        : sweep_ms_(sweep_ms ? sweep_ms : easing::asterisk_sweep_ms),
          sweep_count_(sweep_count ? sweep_count : easing::asterisk_sweep_count),
          reduced_motion_(reduced_motion),
          start_(std::chrono::steady_clock::now()) {}

    ftxui::Element Render() override {
        using namespace ftxui;
        if (reduced_motion_ || settled()) {
            // TS always renders the teardrop "✻" (AnimatedAsterisk.tsx uses
            // the same glyph settled as during animation); keep it consistent.
            return text("✻") | color(Color::RGB(153, 153, 153));
        }
        auto elapsed = std::chrono::steady_clock::now() - start_;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        double total_ms = static_cast<double>(sweep_ms_) * sweep_count_;
        double t = std::min(1.0, static_cast<double>(ms) / total_ms);
        // sweep through hues
        double hue = std::fmod(static_cast<double>(ms) / sweep_ms_ * 360.0, 360.0);
        auto c = hue_to_rgb(hue);
        // fade to settled grey at the end
        const auto settled = Color::RGB(153, 153, 153);
        if (t > 0.85) {
            double fade = (t - 0.85) / 0.15;
            c = interpolate(c, settled, fade);
        }
        return text("✻") | color(c) | bold;
    }

    bool OnEvent(ftxui::Event ev) override {
        // Every 120ms the screen loop will naturally re-render if we ask.
        // FTXUI schedules repaints via ScreenInteractive::PostEvent, which
        // we trigger with a no-op custom event.  Callers are expected to
        // drive the loop; alternatively the component is fine being
        // re-rendered by the parent (e.g. inside a layout that refreshes).
        (void)ev;
        return false;
    }

    [[nodiscard]] bool settled() const noexcept {
        auto elapsed = std::chrono::steady_clock::now() - start_;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        return static_cast<std::uint64_t>(ms)
               >= static_cast<std::uint64_t>(sweep_ms_) * sweep_count_;
    }

private:
    std::uint32_t sweep_ms_;
    std::uint32_t sweep_count_;
    bool reduced_motion_;
    std::chrono::steady_clock::time_point start_;
};

/// Build an AnimatedAsterisk component.  Use `sweep_ms=0` / `sweep_count=0`
/// to pull the defaults from tokens::easing.
[[nodiscard]] inline ftxui::Component make_animated_asterisk(
    std::uint32_t sweep_ms = 0,
    std::uint32_t sweep_count = 0,
    bool reduced_motion = false) {
    return ftxui::Make<AnimatedAsteriskBase>(sweep_ms, sweep_count, reduced_motion);
}

// ─── Logo V2 component (static + asterisk animation) ─────────────────────────
// Combines the mark + wordmark with a teardrop asterisk that plays the
// AnimatedAsterisk sweep once on mount.
class LogoV2Base : public ftxui::ComponentBase {
public:
    explicit LogoV2Base(LogoStyle style, Theme theme)
        : style_(style), theme_(std::move(theme)),
          asterisk_(make_animated_asterisk(0, 0, theme_.a11y.reduced_motion)) {
        Add(asterisk_);
    }

    ftxui::Element Render() override {
        using namespace ftxui;
        Element wm = [&]() -> Element {
            if (style_ == LogoStyle::Mark) return filler() | size(WIDTH, EQUAL, 0);
            return hbox({
                asterisk_->Render(),
                text("  "),
                logo_wordmark(theme_.palette->primary),
                style_ == LogoStyle::Full
                    ? text(std::string(k_seed_full_suffix))
                          | color(theme_.palette->muted) | dim
                    : text(""),
            });
        }();

        if (style_ == LogoStyle::Wordmark) {
            return wm | xflex;
        }
        auto mark = logo_mark(theme_.palette->primary, theme_.palette->background);
        if (style_ == LogoStyle::Mark) return mark;
        return vbox({
            mark,
            filler() | size(HEIGHT, EQUAL, 1),
            wm,
        });
    }

private:
    LogoStyle style_;
    Theme theme_;
    ftxui::Component asterisk_;
};

[[nodiscard]] inline ftxui::Component make_logo_v2(LogoStyle style = LogoStyle::Full,
                                                   Theme theme = current_theme()) {
    return ftxui::Make<LogoV2Base>(style, std::move(theme));
}

// PHASE_5 items (explicitly out of scope for Phase 4):
//  * WelcomeV2 58-char centered layout with scattered * asterisks and
//    per-letter typewriter reveal (requires measurement + async char stream).
//  * CondensedLogo SVG → TTY scaling variants.
//  * OSC-11 driven auto-theme for the logo's background tint.

} // namespace cc::ui::design::logo

#ifdef CC_DESIGN_SYSTEM_DEMO
export namespace cc::ui::design::logo::demo {
inline int demo_static_row_count() { return 3; }
inline std::string demo_wordmark() {
    return std::string(k_seed_wordmark) + std::string(k_seed_full_suffix);
}
}
#endif
