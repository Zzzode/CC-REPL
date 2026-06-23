/// @file shell_time_display.cppm
/// @brief Shell time display component — faithful port of ShellTimeDisplay.tsx.
///
/// Shows elapsed time and/or timeout for shell command execution.
/// Mirrors the TS component's formatDuration calls exactly:
///   - timeout → formatDuration(ms, { hideTrailingZeros: true })
///   - elapsed → formatDuration(elapsed_seconds * 1000)
///
/// Visual:
///   - Only elapsed: "(42s)"
///   - Only timeout: "(timeout 5m)"
///   - Both:      "(42s · timeout 5m)"
///   - Neither:   null / empty element
// ────────────────────────────────────────────────────────────────────────
module;

#include <string>
#include <optional>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

#include <ftxui/dom/elements.hpp>

export module cc.ui.components.shell_time_display;

import cc.ui.design.tokens;
import cc.ui.design.theme;

export namespace cc::ui::components::shell {

using namespace ftxui;
using cc::ui::design::theme::Theme;

// ─── Props ──────────────────────────────────────────────────────────────

struct ShellTimeDisplayProps {
    std::optional<double> elapsed_time_seconds;  // matches TS elapsedTimeSeconds?: number
    std::optional<std::uint64_t> timeout_ms;     // matches TS timeoutMs?: number
};

[[nodiscard]] inline ShellTimeDisplayProps make_shell_time_display_props() {
    return ShellTimeDisplayProps{};
}

// ─── formatDuration (faithful port of src/utils/format.ts formatDuration) ───
//
// TS signature:
//   function formatDuration(ms: number, options?: {
//     hideTrailingZeros?: boolean;
//     mostSignificantOnly?: boolean;
//   }): string
//
// Key behaviors (from TS source):
//   - ms < 60_000: integer seconds + "s"
//   - ms === 0: "0s"
//   - ms < 1: (ms/1000).toFixed(1) + "s"  (sub-millisecond edge case)
//   - >= 60s: minutes + seconds
//   - >= 3600s: hours + minutes + seconds
//   - >= 86400s: days + hours + minutes
//   - hideTrailingZeros: drops zero-valued trailing units
//   - mostSignificantOnly: only shows the largest non-zero unit
//
// We implement this here (rather than using cc.utils.format) because the
// TS-side formatDuration has specific rounding/formatting rules that the
// existing C++ format_duration utilities don't match exactly.  Since this
// is a faithful visual port, pixel-level parity matters.

namespace detail {

struct FormatDurationOptions {
    bool hide_trailing_zeros = false;
    bool most_significant_only = false;
};

[[nodiscard]] inline std::string format_duration_ms(double ms,
                                                    FormatDurationOptions opts = {}) {
    // TS special case: ms === 0 → "0s"
    if (ms == 0.0) return "0s";

    // TS: if (ms < 60000)
    if (ms < 60000.0) {
        // TS: if (ms < 1) → sub-millisecond, show 1 decimal
        if (ms < 1.0) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
            return oss.str();
        }
        // TS: Math.floor(ms / 1000).toString() + "s"
        auto secs = static_cast<std::int64_t>(std::floor(ms / 1000.0));
        return std::to_string(secs) + "s";
    }

    // TS breakdown: days / hours / minutes / seconds with rounding carry-over
    auto total_ms = ms;
    auto days = static_cast<std::int64_t>(std::floor(total_ms / 86400000.0));
    auto hours = static_cast<std::int64_t>(std::floor(std::fmod(total_ms, 86400000.0) / 3600000.0));
    auto minutes = static_cast<std::int64_t>(std::floor(std::fmod(total_ms, 3600000.0) / 60000.0));
    auto seconds = static_cast<std::int64_t>(std::round(std::fmod(total_ms, 60000.0) / 1000.0));

    // Rounding carry-over (TS: if seconds === 60 → 0, minutes++, etc.)
    if (seconds == 60) { seconds = 0; minutes++; }
    if (minutes == 60) { minutes = 0; hours++; }
    if (hours == 24)   { hours = 0;   days++; }

    if (opts.most_significant_only) {
        if (days > 0)    return std::to_string(days) + "d";
        if (hours > 0)   return std::to_string(hours) + "h";
        if (minutes > 0) return std::to_string(minutes) + "m";
        return std::to_string(seconds) + "s";
    }

    const bool hide = opts.hide_trailing_zeros;

    if (days > 0) {
        if (hide && hours == 0 && minutes == 0)
            return std::to_string(days) + "d";
        if (hide && minutes == 0)
            return std::to_string(days) + "d " + std::to_string(hours) + "h";
        return std::to_string(days) + "d " + std::to_string(hours) + "h " +
               std::to_string(minutes) + "m";
    }
    if (hours > 0) {
        if (hide && minutes == 0 && seconds == 0)
            return std::to_string(hours) + "h";
        if (hide && seconds == 0)
            return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m " +
               std::to_string(seconds) + "s";
    }
    if (minutes > 0) {
        if (hide && seconds == 0)
            return std::to_string(minutes) + "m";
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }
    return std::to_string(seconds) + "s";
}

} // namespace detail

// ─── Render ─────────────────────────────────────────────────────────────
//
// Faithful rendering of ShellTimeDisplay.tsx:
//
//   if (elapsedTimeSeconds === undefined && !timeoutMs) return null
//   const timeout = timeoutMs ? formatDuration(timeoutMs, {hideTrailingZeros:true}) : undefined
//   if (elapsedTimeSeconds === undefined) return <Text dim>(timeout {timeout})</Text>
//   const elapsed = formatDuration(elapsedTimeSeconds * 1000)
//   if (timeout) return <Text dim>({elapsed} · timeout {timeout})</Text>
//   return <Text dim>({elapsed})</Text>
//
// OffscreenFreeze wrapping is handled by the caller (ShellProgressMessage),
// matching the TS component hierarchy.

[[nodiscard]] inline Element render_shell_time_display(
    const ShellTimeDisplayProps& props,
    const Theme& theme)
{
    // Both absent → null element (empty text that contributes nothing)
    if (!props.elapsed_time_seconds.has_value() && !props.timeout_ms.has_value()) {
        return text("");
    }

    std::optional<std::string> timeout_str;
    if (props.timeout_ms.has_value()) {
        timeout_str = detail::format_duration_ms(
            static_cast<double>(*props.timeout_ms),
            {.hide_trailing_zeros = true});
    }

    // No elapsed — only timeout
    if (!props.elapsed_time_seconds.has_value()) {
        auto label = "(timeout " + *timeout_str + ")";
        return text(label) | dim | color(theme.palette->muted);
    }

    // Elapsed present
    double elapsed_ms = *props.elapsed_time_seconds * 1000.0;
    std::string elapsed_str = detail::format_duration_ms(elapsed_ms);

    if (timeout_str.has_value()) {
        // "(1m 23s · timeout 5m)"
        auto label = "(" + elapsed_str + " \xc2\xb7 timeout " + *timeout_str + ")";
        return text(label) | dim | color(theme.palette->muted);
    }

    // "(1m 23s)"
    auto label = "(" + elapsed_str + ")";
    return text(label) | dim | color(theme.palette->muted);
}

} // namespace cc::ui::components::shell
