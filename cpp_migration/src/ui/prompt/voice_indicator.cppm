module;
#include <string>
#include <sstream>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt.voice_indicator;

import cc.ui.design.theme;

export namespace cc::ui::prompt {

// TS REF: src/components/PromptInput/VoiceIndicator.tsx:7-9
//   type Props = { voiceState: 'idle' | 'recording' | 'processing' };
// The TS 'recording' prop value is named Listening here, matching the
// existing cc::context::VoiceState::Listening vocabulary.  The previously
// invented Speaking state had no TS counterpart and zero users, so it was
// removed.
enum class FooterVoiceState { Idle, Listening, Processing };

// TS REF: VoiceIndicator.tsx:12-22
// Processing shimmer colors: dim gray to lighter gray (matches
// ThinkingShimmerText); 2 second period for the pulsing animation.
struct VoiceRgb { int r, g, b; };
inline constexpr VoiceRgb kVoiceProcessingDim{153, 153, 153};
inline constexpr VoiceRgb kVoiceProcessingBright{185, 185, 185};
inline constexpr double kVoicePulsePeriodSec = 2.0;

// M_PI is not portable under strict standard modes; keep a local constant.
inline constexpr double kVoicePi = 3.14159265358979323846;

// TS REF: src/components/Spinner/utils.ts:14-24 interpolateColor()
//   r: Math.round(color1.r + (color2.r - color1.r) * t), per channel.
// Do NOT reuse cc::ui::components::interpolate_color() — that is a binary
// threshold fake (t < 0.5), not an interpolation.
[[nodiscard]] inline ftxui::Color InterpolateVoiceColor(
    VoiceRgb a, VoiceRgb b, double t) {
    const auto lerp = [t](int c1, int c2) -> int {
        return static_cast<int>(std::lround(c1 + (c2 - c1) * t));
    };
    const int r = lerp(a.r, b.r);
    const int g = lerp(a.g, b.g);
    const int bl = lerp(a.b, b.b);
    return ftxui::Color::RGB(
        static_cast<std::uint8_t>(r),
        static_cast<std::uint8_t>(g),
        static_cast<std::uint8_t>(bl));
}

// Idle renders nothing (VoiceIndicator.tsx:67-70 returns null); the caller
// uses this to skip mounting the indicator entirely.
[[nodiscard]] inline bool VoiceIndicatorVisible(FooterVoiceState s) {
    return s != FooterVoiceState::Idle;
}

/// Faithful FTXUI port of TS VoiceIndicator.tsx.
///
/// TS REF: VoiceIndicator.tsx:44-72 (VoiceIndicatorImpl switch):
///   case "recording": return <Text dimColor={true}>listening…</Text>;
///   case "processing": return <ProcessingShimmer />;
///   case "idle": return null;
///
/// TS REF: VoiceIndicator.tsx:92-136 (ProcessingShimmer):
///   const reducedMotion = settings.prefersReducedMotion ?? false;
///   const [, time] = useAnimationFrame(reducedMotion ? null : 50);
///   if (reducedMotion)
///     return <Text color="warning">Voice: processing…</Text>;
///   const elapsedSec = time / 1000;
///   const opacity =
///     (Math.sin(elapsedSec * Math.PI * 2 / PULSE_PERIOD_S) + 1) / 2;
///   const color = toRGBColor(
///     interpolateColor(PROCESSING_DIM, PROCESSING_BRIGHT, opacity));
///   return <Text color={color}>Voice: processing…</Text>;
///
/// TIMING: TS drives the shimmer from a shared 50ms keepAlive animation
/// clock (useAnimationFrame(50); ClockContext setInterval 50ms, paused
/// under reduced motion and frozen offscreen).  In CPP the equivalent is
/// the 50ms StartUiAnimationTicker in app.cppm, which must keep repainting
/// only while voice state is Processing.  elapsed_sec is wall-clock
/// seconds stamped at the Processing transition (ProjectVoiceFooterStatus),
/// matching TS elapsedSec = time / 1000.
///
/// Placement: Notifications.tsx NotificationContent (lines ~283-285)
/// returns <VoiceIndicator> INSTEAD of every other notification while
/// recording/processing (highest-priority early return).  It is mounted in
/// the right-aligned Notifications column of PromptInputFooter.
///
/// Both strings use U+2026 HORIZONTAL ELLIPSIS (UTF-8 E2 80 A6), not three
/// ASCII dots.
[[nodiscard]] inline ftxui::Element RenderVoiceIndicator(
    FooterVoiceState state, double elapsed_sec, bool reduced_motion) {
    using namespace ftxui;

    switch (state) {
        case FooterVoiceState::Listening:
            // TS: <Text dimColor={true}>listening…</Text>
            return text("listening\xE2\x80\xA6")
                 | dim
                 | size(HEIGHT, EQUAL, 1);

        case FooterVoiceState::Processing: {
            const std::string label = "Voice: processing\xE2\x80\xA6";
            if (reduced_motion) {
                // TS: static <Text color="warning"> (dark token
                // RGB(255,193,7)); no time dependence, animation clock
                // paused via useAnimationFrame(null).
                return text(label)
                     | color(cc::ui::design::theme::current_theme()
                                 .palette->warning)
                     | size(HEIGHT, EQUAL, 1);
            }
            // TS: opacity = (sin(elapsed * 2pi / 2) + 1) / 2; at
            // elapsed = 0 opacity is 0 -> PROCESSING_DIM RGB(153,153,153).
            const double opacity =
                (std::sin(elapsed_sec * 2.0 * kVoicePi /
                          kVoicePulsePeriodSec) + 1.0) / 2.0;
            return text(label)
                 | color(InterpolateVoiceColor(
                       kVoiceProcessingDim, kVoiceProcessingBright, opacity))
                 | size(HEIGHT, EQUAL, 1);
        }

        case FooterVoiceState::Idle:
            // TS idle -> null: zero glyphs, zero rows.
            return text("");
    }
    return text("");
}

// Render an audio level meter (VU meter style)
inline auto render_audio_level(float level, int width) -> std::string {
    // Clamp level to [0, 1]
    level = std::max(0.0f, std::min(1.0f, level));

    std::ostringstream out;
    int filled = static_cast<int>(level * width);

    for (int i = 0; i < width; ++i) {
        float threshold = static_cast<float>(i) / width;
        if (i < filled) {
            // Color gradient: green -> yellow -> red
            if (threshold < 0.6f) {
                out << "\033[32m▮\033[0m"; // green
            } else if (threshold < 0.8f) {
                out << "\033[33m▮\033[0m"; // yellow
            } else {
                out << "\033[31m▮\033[0m"; // red
            }
        } else {
            out << "\033[2m▯\033[0m"; // empty
        }
    }

    return out.str();
}

} // namespace cc::ui::prompt
