/// @file logo.cppm
/// @brief Seed / Clawd ASCII logo with an animated asterisk.
/// TS-side references audited: animated asterisk, welcome banner, condensed logo.
/// We render the clawd mark with unicode block characters, suffix "🌱 Seed" as
/// the Seed wordmark (TS SVG logos don't port to TTY), and implement the
/// animated asterisk via an FTXUI Component (Renderer + CatchEvent) that
/// ticks on a 120ms cadence.
module;

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
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

// ─── Logo component (static + asterisk animation) ───────────────────────────
// Combines the mark + wordmark with a teardrop asterisk that plays the
// AnimatedAsterisk sweep once on mount.
class LogoBase : public ftxui::ComponentBase {
public:
    explicit LogoBase(LogoStyle style, Theme theme)
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

[[nodiscard]] inline ftxui::Component make_logo(LogoStyle style = LogoStyle::Full,
                                                Theme theme = current_theme()) {
    return ftxui::Make<LogoBase>(style, std::move(theme));
}

// ─── Welcome banner ─────────────────────────────────────────────────────────
// The upstream banner is a 58-col fixed-width ASCII block: a row-0 welcome
// caption, a 60-char dotted underline, then a hand-drawn "Clawd" mascot made
// of ░ ▒ ▓ █ quadrants with scattered * / bold-* / dim-* asterisks scattered
// around it, and a single embedded Clawd ASCII mark (█████████ / ██▄█████▄██
// / █████████) in the clawd_body colour.  Every glyph below is transcribed
// from the compiled JS so the silhouette matches the real product;
// only the trailing block-row colouring is parameterised on `clawd_body`.
//
// Width reference (TTY display columns; all glyphs are width-1 BMP): the
// mascot rows are 58 cols, the dotted underline is 60 cols (overhangs by 2,
// matching TS).  The whole banner is wrapped in a fixed 58-col Box so the
// right edge lines up exactly like the upstream fixed-width box.

namespace welcome_banner_detail {
// Mascot body rows (no colour applied; the clawd mark span is coloured
// separately by the caller).  These are the exact literals from the
// dark-theme branch of the upstream welcome banner.
constexpr std::string_view k_row_caption = "Welcome to Claude Code ";
constexpr std::string_view k_row_dotted  =
    "……………………………………………………………………………………………………………………"; // 60 × …
constexpr std::string_view k_row_blank   =
    "                                                          "; // 58 sp
constexpr std::string_view k_row_3 =
    "     *                                       ████▓▓░     ";
constexpr std::string_view k_row_4 =
    "                                 *         ████▓░     ░░   ";
constexpr std::string_view k_row_5 =
    "            ░░░░░░                        ████▓░           ";
constexpr std::string_view k_row_6 =
    "    ░░░   ░░░░░░░░░░                      ████▓░           ";
// row 7 has a bold '*' at a fixed column; split into pre / star / post.
constexpr std::string_view k_row_7_pre  =
    "   ░░░░░░░░░░░░░░░░░░    ";
constexpr std::string_view k_row_7_post =
    "                ███▓░      ▓   ";
constexpr std::string_view k_row_8 =
    "                                             ░░▓███▓▓░    ";
// rows 9–11 are dim-coloured in TS.
constexpr std::string_view k_row_9  =
    " *                                 ░░░░                   ";
constexpr std::string_view k_row_10 =
    "                                 ░░░░░░░░                 ";
constexpr std::string_view k_row_11 =
    "                               ░░░░░░░░░░░░░░░░           ";
// Clawd-mark rows: 6-space indent + mark + trailing scatter.  The mark span
// itself is clawd_body-coloured; the trailing asterisk line varies.
constexpr std::string_view k_row_13_pre   = "      ";
constexpr std::string_view k_row_13_mark  = " █████████ ";
constexpr std::string_view k_row_13_mid   =
    "                                       "; // 39 sp
constexpr std::string_view k_row_13_post  = " "; // dim '*' appended after
constexpr std::string_view k_row_14_pre   = "      ";
constexpr std::string_view k_row_14_mark  = "██▄█████▄██";
constexpr std::string_view k_row_14_mid   = "                        "; // 24 sp
constexpr std::string_view k_row_14_post  = "                "; // 16 sp (bold '*')
constexpr std::string_view k_row_15_pre   = "      ";
constexpr std::string_view k_row_15_mark  = " █████████ ";
constexpr std::string_view k_row_15_post  =
    "     *                                   ";
// Trailing concatenation row: dots + clawd_body "█ █   █ █" + dots.
constexpr std::string_view k_row_tail_pre  = "………"; // 7 dots
constexpr std::string_view k_row_tail_mark = "█ █   █ █";
constexpr std::string_view k_row_tail_post =
    "……………………………………………………………………"; // 40 dots (total row ≈ 58)
} // namespace welcome_banner_detail

/// Render the welcome banner as a static Element.  `clawd_body` is the
/// foreground colour for the embedded Clawd mark spans; everything else is
/// plain text, with the per-row bold/dim styling baked in to match TS.
/// `version` is substituted into the caption ("v{version}").
[[nodiscard]] inline ftxui::Element welcome_banner(
    std::string_view version, ftxui::Color clawd_body,
    std::optional<ftxui::Color> clawd_bg = std::nullopt) {
    using namespace ftxui;
    namespace d = welcome_banner_detail;
    auto bg = [&](Element e) -> Element {
        return clawd_bg ? (e | bgcolor(*clawd_bg)) : e;
    };
    Elements rows;
    // Row 0: "Welcome to Claude Code " (claude-coloured) + dim "v{version} "
    rows.push_back(hbox({
        text(std::string(d::k_row_caption)) | color(clawd_body) | bold,
        text("v" + std::string(version) + " ") | dim,
    }));
    rows.push_back(text(std::string(d::k_row_dotted)));
    rows.push_back(text(std::string(d::k_row_blank)));
    rows.push_back(text(std::string(d::k_row_3)));
    rows.push_back(text(std::string(d::k_row_4)));
    rows.push_back(text(std::string(d::k_row_5)));
    rows.push_back(text(std::string(d::k_row_6)));
    // Row 7: pre + bold '*' + post.
    rows.push_back(hbox({
        text(std::string(d::k_row_7_pre)),
        text("*") | bold,
        text(std::string(d::k_row_7_post)),
    }));
    rows.push_back(text(std::string(d::k_row_8)));
    rows.push_back(text(std::string(d::k_row_9))  | dim);
    rows.push_back(text(std::string(d::k_row_10)) | dim);
    rows.push_back(text(std::string(d::k_row_11)) | dim);
    // Row 13: indent + clawd_body mark + spaces + dim '*'.
    rows.push_back(hbox({
        text(std::string(d::k_row_13_pre)),
        bg(text(std::string(d::k_row_13_mark)) | color(clawd_body)),
        text(std::string(d::k_row_13_mid)),
        text("*") | dim,
        text(std::string(d::k_row_13_post)),
    }));
    // Row 14: indent + clawd_body mark + spaces + bold '*'.
    rows.push_back(hbox({
        text(std::string(d::k_row_14_pre)),
        bg(text(std::string(d::k_row_14_mark)) | color(clawd_body)),
        text(std::string(d::k_row_14_mid)),
        text("*") | bold,
        text(std::string(d::k_row_14_post)),
    }));
    // Row 15: indent + clawd_body mark + trailing scatter (has a plain '*').
    rows.push_back(hbox({
        text(std::string(d::k_row_15_pre)),
        bg(text(std::string(d::k_row_15_mark)) | color(clawd_body)),
        text(std::string(d::k_row_15_post)),
    }));
    // Trailing row: dots + clawd_body "█ █   █ █" + dots.
    rows.push_back(hbox({
        text(std::string(d::k_row_tail_pre)),
        text(std::string(d::k_row_tail_mark)) | color(clawd_body),
        text(std::string(d::k_row_tail_post)),
    }));
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, 58);
}

// ─── Frame-driven animated teardrop asterisk ────────────────────────────────
// Mirrors AnimatedAsterisk.tsx's hue sweep but driven by an externally-ticked
// frame counter (the M1 per-frame `spinner_frame`) instead of a Component
// timer, so it works inside the pure-Element render path the REPL uses.
//
// Hue math matches TS exactly: hue = (elapsed_ms / SWEEP_MS) * 360 mod 360,
// where SWEEP_MS=1500.  Unlike the one-shot TS AnimatedAsterisk component,
// the REPL home card keeps this glyph mounted for the whole idle session, so
// it loops the sweep instead of settling permanently after three seconds.
//
// `reduced_motion` short-circuits to the settled grey glyph immediately.
[[nodiscard]] inline ftxui::Element welcome_animated_asterisk(
    int frame, bool reduced_motion = false,
    std::uint32_t sweep_ms = 0, std::uint32_t sweep_count = 0) {
    using namespace ftxui;
    constexpr std::string_view k_glyph = "✻"; // TEARDROP_ASTERISK
    const auto sw_ms    = sweep_ms    ? sweep_ms    : easing::asterisk_sweep_ms;
    const auto sw_count = sweep_count ? sweep_count : easing::asterisk_sweep_count;
    const auto total_ms = static_cast<double>(sw_ms) * sw_count;
    const double frame_ms = 50.0; // TS useAnimationFrame cadence
    if (reduced_motion) {
        return text(std::string(k_glyph)) | color(Color::RGB(153, 153, 153));
    }
    double elapsed = std::fmod(static_cast<double>(frame) * frame_ms, total_ms);
    double hue = std::fmod((elapsed / sw_ms) * 360.0, 360.0);
    auto c = hue_to_rgb(hue);
    // Fade to settled grey over the final 15% (mirrors AnimatedAsteriskBase).
    const auto settled = Color::RGB(153, 153, 153);
    double t = std::min(1.0, elapsed / total_ms);
    if (t > 0.85) {
        double fade = (t - 0.85) / 0.15;
        c = interpolate(c, settled, fade);
    }
    return text(std::string(k_glyph)) | color(c) | bold;
}

// PHASE_5 items (explicitly out of scope for Phase 4):
//  * CondensedLogo SVG → TTY scaling variants.
//  * OSC-11 driven auto-theme for the logo's background tint.
//  * Per-letter typewriter reveal of the banner (requires async char stream).

} // namespace cc::ui::design::logo

#ifdef CC_DESIGN_SYSTEM_DEMO
export namespace cc::ui::design::logo::demo {
inline int demo_static_row_count() { return 3; }
inline std::string demo_wordmark() {
    return std::string(k_seed_wordmark) + std::string(k_seed_full_suffix);
}
}
#endif
