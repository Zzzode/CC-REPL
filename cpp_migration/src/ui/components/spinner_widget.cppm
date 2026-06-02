/// @file spinner_widget.cppm
/// @brief Extended spinner widget with multiple animation styles, color hue
/// rotation, shimmer effects, and configurable speed. Migrated from
/// Spinner/index.ts (FlashingChar, ShimmerChar, SpinnerGlyph, GlimmerMessage).
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <array>
#include <numbers>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.spinner_widget;

export namespace cc::ui::spinner_widget {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Spinner animation style
enum class SpinnerStyle : std::uint8_t {
    Dots,           // ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏
    Glyph,          // ·✢*✶✻✽ (default CC-REPL style)
    Shimmer,        // Flowing color shimmer across text
    Flash,          // Single char with pulsing brightness
    Glimmer,        // Sparkle effect on message text
    Bar,            // ▏▎▍▌▋▊▉█
    Bounce,         // ⠁⠂⠄⡀⢀⠠⠐⠈
};

/// Configuration for a spinner widget
struct SpinnerWidgetOptions {
    SpinnerStyle style = SpinnerStyle::Glyph;
    std::string message;
    std::string label;              // Short label shown beside spinner
    bool reduced_motion = false;
    int speed_ms = 80;              // Frame interval in milliseconds
    float hue_start = 180.0f;       // Starting hue for color animation
    float hue_speed = 90.0f;        // Degrees per second
    bool use_color = true;
};

// ============================================================
// Color Utilities
// ============================================================

/// HSL hue (0-360) to RGB
[[nodiscard]] inline Color hue_to_color(float hue, float saturation = 0.7f,
                                         float lightness = 0.6f) {
    // HSL to RGB conversion
    float c = (1.0f - std::abs(2.0f * lightness - 1.0f)) * saturation;
    float h = hue / 60.0f;
    float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    float m = lightness - c / 2.0f;

    float r = 0, g = 0, b = 0;
    if (h < 1)      { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else            { r = c; b = x; }

    return Color::RGB(
        static_cast<uint8_t>((r + m) * 255),
        static_cast<uint8_t>((g + m) * 255),
        static_cast<uint8_t>((b + m) * 255));
}

/// Interpolate between two colors
[[nodiscard]] inline Color interpolate_color(Color from, Color to, float t) {
    // Simplified: just return based on threshold
    return t < 0.5f ? from : to;
}

// ============================================================
// Frame Data
// ============================================================

namespace frames {

inline const std::vector<std::string>& dots() {
    static const std::vector<std::string> f =
        {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return f;
}

inline const std::vector<std::string>& glyph() {
    static const std::vector<std::string> f =
        {"·", "✢", "*", "✶", "✻", "✽", "✻", "✶", "*", "✢"};
    return f;
}

inline const std::vector<std::string>& bar() {
    static const std::vector<std::string> f =
        {"▏", "▎", "▍", "▌", "▋", "▊", "▉", "█", "▉", "▊", "▋", "▌", "▍", "▎"};
    return f;
}

inline const std::vector<std::string>& bounce() {
    static const std::vector<std::string> f =
        {"⠁", "⠂", "⠄", "⡀", "⢀", "⠠", "⠐", "⠈"};
    return f;
}

} // namespace frames

/// Get frame set for style
[[nodiscard]] inline const std::vector<std::string>& get_frames(SpinnerStyle style) {
    switch (style) {
        case SpinnerStyle::Dots:    return frames::dots();
        case SpinnerStyle::Glyph:   return frames::glyph();
        case SpinnerStyle::Bar:     return frames::bar();
        case SpinnerStyle::Bounce:  return frames::bounce();
        default:                    return frames::glyph();
    }
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a basic frame-based spinner element
[[nodiscard]] inline Element RenderFrameSpinner(
    const SpinnerWidgetOptions& opts,
    std::chrono::steady_clock::time_point start_time) {

    if (opts.reduced_motion) {
        auto el = text("●") | color(Color::GrayLight);
        if (!opts.message.empty()) {
            return hbox({el, text(" " + opts.message) | dim});
        }
        return el;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();

    const auto& frame_set = get_frames(opts.style);
    int frame_idx = (elapsed_ms / opts.speed_ms) %
                    static_cast<int>(frame_set.size());

    // Color based on hue rotation
    Color spinner_color = Color::CyanLight;
    if (opts.use_color) {
        float elapsed_sec = static_cast<float>(elapsed_ms) / 1000.0f;
        float hue = std::fmod(opts.hue_start + elapsed_sec * opts.hue_speed, 360.0f);
        spinner_color = hue_to_color(hue);
    }

    auto spinner_el = text(frame_set[frame_idx]) | color(spinner_color);

    if (!opts.message.empty()) {
        return hbox({spinner_el, text(" " + opts.message) | color(Color::White)});
    }
    if (!opts.label.empty()) {
        return hbox({spinner_el, text(" " + opts.label) | dim});
    }
    return spinner_el;
}

/// Render a shimmer effect across a message string
[[nodiscard]] inline Element RenderShimmerSpinner(
    const SpinnerWidgetOptions& opts,
    std::chrono::steady_clock::time_point start_time) {

    if (opts.reduced_motion || opts.message.empty()) {
        return text(opts.message.empty() ? "..." : opts.message) | dim;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();
    float elapsed_sec = static_cast<float>(elapsed_ms) / 1000.0f;

    // Each character gets a slightly offset hue
    Elements chars;
    for (size_t i = 0; i < opts.message.size(); ++i) {
        float offset = static_cast<float>(i) * 15.0f; // 15 degrees per char
        float hue = std::fmod(opts.hue_start + elapsed_sec * opts.hue_speed + offset,
                              360.0f);
        auto clr = hue_to_color(hue, 0.8f, 0.65f);
        chars.push_back(text(std::string(1, opts.message[i])) | color(clr));
    }
    return hbox(chars);
}

/// Render a flashing single character
[[nodiscard]] inline Element RenderFlashSpinner(
    const SpinnerWidgetOptions& opts,
    std::chrono::steady_clock::time_point start_time) {

    if (opts.reduced_motion) {
        return text("●") | color(Color::GrayLight);
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();

    // Pulse brightness using sine wave
    float t = static_cast<float>(elapsed_ms) / 1000.0f;
    float brightness = 0.4f + 0.6f * (std::sin(t * std::numbers::pi_v<float> * 2.0f)
                                       * 0.5f + 0.5f);

    auto clr = Color::RGB(
        static_cast<uint8_t>(brightness * 100),
        static_cast<uint8_t>(brightness * 220),
        static_cast<uint8_t>(brightness * 255));

    auto el = text("●") | color(clr);
    if (!opts.label.empty()) {
        return hbox({el, text(" " + opts.label) | dim});
    }
    return el;
}

/// Render a glimmer message (sparkle effect on random chars)
[[nodiscard]] inline Element RenderGlimmerSpinner(
    const SpinnerWidgetOptions& opts,
    std::chrono::steady_clock::time_point start_time) {

    if (opts.message.empty()) {
        return text("") | dim;
    }
    if (opts.reduced_motion) {
        return text(opts.message) | color(Color::GrayLight);
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time).count();

    // Pseudo-random sparkle positions based on time
    Elements chars;
    for (size_t i = 0; i < opts.message.size(); ++i) {
        // Simple hash to determine sparkle timing per character
        int phase = static_cast<int>((elapsed_ms / 150 + i * 7) % 20);
        bool sparkling = (phase < 3);

        if (sparkling && opts.message[i] != ' ') {
            float hue = std::fmod(opts.hue_start + static_cast<float>(i) * 30.0f,
                                  360.0f);
            auto clr = hue_to_color(hue, 0.9f, 0.75f);
            chars.push_back(text(std::string(1, opts.message[i]))
                            | color(clr) | bold);
        } else {
            chars.push_back(text(std::string(1, opts.message[i]))
                            | color(Color::GrayLight));
        }
    }
    return hbox(chars);
}

/// Render any spinner style
[[nodiscard]] inline Element RenderSpinnerWidget(
    const SpinnerWidgetOptions& opts,
    std::chrono::steady_clock::time_point start_time) {

    switch (opts.style) {
        case SpinnerStyle::Shimmer:
            return RenderShimmerSpinner(opts, start_time);
        case SpinnerStyle::Flash:
            return RenderFlashSpinner(opts, start_time);
        case SpinnerStyle::Glimmer:
            return RenderGlimmerSpinner(opts, start_time);
        default:
            return RenderFrameSpinner(opts, start_time);
    }
}

// ============================================================
// Interactive Component
// ============================================================

/// Create a spinner widget component
[[nodiscard]] inline Component SpinnerWidget(SpinnerWidgetOptions options) {
    struct State {
        SpinnerWidgetOptions opts;
        std::chrono::steady_clock::time_point start_time;
    };
    auto state = std::make_shared<State>();
    state->opts = std::move(options);
    state->start_time = std::chrono::steady_clock::now();

    return Renderer([state] {
        return RenderSpinnerWidget(state->opts, state->start_time);
    });
}

/// Create a labeled spinner with message
[[nodiscard]] inline Component LabeledSpinner(
    std::string message, SpinnerStyle style) {

    SpinnerWidgetOptions opts;
    opts.style = style;
    opts.message = std::move(message);
    return SpinnerWidget(std::move(opts));
}

/// Create a simple thinking spinner
[[nodiscard]] inline Component ThinkingSpinner() {
    SpinnerWidgetOptions opts;
    opts.style = SpinnerStyle::Glyph;
    opts.message = "Thinking...";
    opts.hue_start = 200.0f;
    return SpinnerWidget(std::move(opts));
}

} // namespace cc::ui::spinner_widget
