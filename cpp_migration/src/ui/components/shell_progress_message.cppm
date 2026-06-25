/// @file shell_progress_message.cppm
/// @brief Shell progress message renderer — faithful port of ShellProgressMessage.tsx.
///
/// Displays streaming output of a running shell command with:
///   - "Running…" + timer when no output yet
///   - Last 5 lines of output (dimmed) when not verbose
///   - Full output when verbose
///   - Line status: "+N lines" or "~N lines" (truncated estimate)
///   - ShellTimeDisplay for elapsed/timeout
///   - File size badge when totalBytes is available
///
/// Faithful port details:
///   - Output is stripped of ANSI codes (stripAnsi) and trimmed
///   - Line count uses split("\n").filter(line => line)
///   - Non-verbose mode shows last 5 lines inside a height-limited Box
///   - OffscreenFreeze semantics: content is wrapped so repeated renders
///     with the same visible output don't force terminal resets
///   - MessageResponse wrapping (left-gutter "⎿") handled here
// ────────────────────────────────────────────────────────────────────────
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cmath>

#include <ftxui/dom/elements.hpp>

export module cc.ui.components.shell_progress_message;

import cc.ui.components.shell_time_display;
import cc.ui.design.tokens;
import cc.ui.design.theme;
import cc.utils.format;

export namespace cc::ui::components::shell {

using namespace ftxui;
using cc::ui::design::theme::Theme;

// ─── Props ──────────────────────────────────────────────────────────────
// Mirrors the TS ShellProgressMessage Props type exactly.
// All optional fields except verbose match the TS interface.

struct ShellProgressMessageProps {
    std::string output;              // current output tail (may be partial)
    std::string full_output;         // full accumulated output
    std::optional<double> elapsed_time_seconds;  // TS: elapsedTimeSeconds?: number
    std::optional<std::uint64_t> total_lines;    // TS: totalLines?: number
    std::optional<std::uint64_t> total_bytes;    // TS: totalBytes?: number
    std::optional<std::uint64_t> timeout_ms;     // TS: timeoutMs?: number
    std::optional<std::string> task_id;          // TS: taskId?: string
    bool verbose = false;                        // TS: verbose: boolean
};

[[nodiscard]] inline ShellProgressMessageProps make_shell_progress_props() {
    return ShellProgressMessageProps{};
}

// ─── formatFileSize (faithful port of src/utils/format.ts formatFileSize)
//
// TS behavior:
//   - < 1024 bytes: "{n} bytes"
//   - < 1024 KB:   "{kb.toFixed(1).replace(/\.0$/, '')}KB"
//   - < 1024 MB:   "{mb.toFixed(1).replace(/\.0$/, '')}MB"
//   - else:        "{gb.toFixed(1).replace(/\.0$/, '')}GB"
//
// The key detail is stripping ".0" from formatted numbers.

namespace detail {

[[nodiscard]] inline std::string format_file_size(std::uint64_t size_in_bytes) {
    double kb = static_cast<double>(size_in_bytes) / 1024.0;
    if (kb < 1.0) {
        return std::to_string(size_in_bytes) + " bytes";
    }
    if (kb < 1024.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << kb;
        std::string s = oss.str();
        // strip trailing ".0"
        if (s.size() >= 2 && s.substr(s.size() - 2) == ".0") {
            s = s.substr(0, s.size() - 2);
        }
        return s + "KB";
    }
    double mb = kb / 1024.0;
    if (mb < 1024.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << mb;
        std::string s = oss.str();
        if (s.size() >= 2 && s.substr(s.size() - 2) == ".0") {
            s = s.substr(0, s.size() - 2);
        }
        return s + "MB";
    }
    double gb = mb / 1024.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gb;
    // Note: TS uses toFixed(1) for GB too but our existing util uses 2.
    // Faithful port: use toFixed(1) for consistency.
    oss.str("");
    oss << std::fixed << std::setprecision(1) << gb;
    std::string s = oss.str();
    if (s.size() >= 2 && s.substr(s.size() - 2) == ".0") {
        s = s.substr(0, s.size() - 2);
    }
    return s + "GB";
}

/// Split string by newline and filter out empty lines.
/// Mirrors TS: strippedOutput.split("\n").filter(line => line)
[[nodiscard]] inline std::vector<std::string> split_nonempty_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            if (!current.empty()) {
                lines.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    return lines;
}

/// Trim whitespace from both ends of a string.
[[nodiscard]] inline std::string trim(std::string_view s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    auto end = s.end();
    do {
        --end;
    } while (std::distance(start, end) > 0 && std::isspace(static_cast<unsigned char>(*end)));
    ++end;
    return std::string(start, end);
}

} // namespace detail

// ─── Render ─────────────────────────────────────────────────────────────
//
// Faithful rendering of ShellProgressMessage.tsx:
//
//   - Strip ANSI + trim from both output and fullOutput
//   - If no lines: "Running… " + ShellTimeDisplay (in MessageResponse)
//   - Otherwise:
//     - displayLines = verbose ? fullOutput : last 5 lines
//     - Box height = verbose ? undefined : min(5, lines.length)
//     - Line status (non-verbose):
//         totalBytes + totalLines → "~{totalLines} lines"
//         extraLines > 0        → "+{extraLines} lines"
//     - Row with line status + ShellTimeDisplay + file size (gap=1)
//
// We don't implement OffscreenFreeze as a separate component because in
// FTXUI the equivalent is natural — once content is rendered it doesn't
// force terminal resets unless the DOM actually changes.  The perf
// rationale (avoiding full terminal resets on every elapsed-time tick)
// is implicitly handled by FTXUI's diff-based rendering.

[[nodiscard]] inline Element render_shell_progress_message(
    const ShellProgressMessageProps& props,
    const Theme& theme)
{
    // Strip ANSI + trim (mirrors TS stripAnsi(fullOutput.trim()))
    std::string stripped_full = cc::utils::strip_ansi(detail::trim(props.full_output));
    std::string stripped_out  = cc::utils::strip_ansi(detail::trim(props.output));

    auto lines = detail::split_nonempty_lines(stripped_out);

    // displayLines: verbose → full output, non-verbose → last 5 lines
    std::string display_lines_text;
    if (props.verbose) {
        display_lines_text = stripped_full;
    } else {
        std::size_t n = lines.size();
        std::size_t start = (n > 5) ? (n - 5) : 0;
        for (std::size_t i = start; i < n; ++i) {
            if (i > start) display_lines_text += '\n';
            display_lines_text += lines[i];
        }
    }

    // ── Empty output state ──────────────────────────────────────────
    // "Running… " + ShellTimeDisplay inside MessageResponse
    if (lines.empty()) {
        ShellTimeDisplayProps time_props;
        time_props.elapsed_time_seconds = props.elapsed_time_seconds;
        time_props.timeout_ms = props.timeout_ms;
        auto time_el = render_shell_time_display(time_props, theme);

        auto running_text = text("Running\xe2\x80\xa6 ") | dim | color(theme.palette->muted);
        auto body = hbox({ running_text, time_el });

        // MessageResponse wrapping: left-gutter "  ⎿  " prefix
        auto prefix = text("  \xe2\x8f\xbf  ") | dim | color(theme.palette->muted);
        return vbox({ hbox({ prefix, body }) });
    }

    // ── With output state ───────────────────────────────────────────

    // extraLines = totalLines ? max(0, totalLines - 5) : 0
    std::uint64_t extra_lines = 0;
    if (props.total_lines.has_value()) {
        std::uint64_t tl = *props.total_lines;
        extra_lines = (tl > 5) ? (tl - 5) : 0;
    }

    // lineStatus computation
    std::string line_status;
    if (!props.verbose && props.total_bytes.has_value() && props.total_lines.has_value()) {
        // Truncated estimate: "~2000 lines"
        line_status = "~" + std::to_string(*props.total_lines) + " lines";
    } else if (!props.verbose && extra_lines > 0) {
        // Not truncated but more than displayed: "+2 lines"
        line_status = "+" + std::to_string(extra_lines) + " lines";
    }

    // Display box: height-limited in non-verbose mode
    // Build as vbox of individual lines so FTXUI lays them out correctly
    Elements display_lines_elements;
    if (props.verbose) {
        auto full_lines = detail::split_nonempty_lines(stripped_full);
        for (const auto& line : full_lines) {
            display_lines_elements.push_back(
                text(line) | dim | color(theme.palette->muted));
        }
    } else {
        // Non-verbose: last 5 lines
        std::size_t n = lines.size();
        std::size_t start = (n > 5) ? (n - 5) : 0;
        for (std::size_t i = start; i < n; ++i) {
            display_lines_elements.push_back(
                text(lines[i]) | dim | color(theme.palette->muted));
        }
    }

    Element display_el = vbox(std::move(display_lines_elements));

    if (!props.verbose) {
        int height = std::min(5, static_cast<int>(lines.size()));
        display_el = display_el | size(HEIGHT, EQUAL, height);
    }

    // Line status element (null-element if empty)
    Element line_status_el = line_status.empty()
        ? text("")
        : text(line_status) | dim | color(theme.palette->muted);

    // ShellTimeDisplay
    ShellTimeDisplayProps time_props;
    time_props.elapsed_time_seconds = props.elapsed_time_seconds;
    time_props.timeout_ms = props.timeout_ms;
    auto time_el = render_shell_time_display(time_props, theme);

    // File size element
    Element size_el = text("");
    if (props.total_bytes.has_value()) {
        size_el = text(detail::format_file_size(*props.total_bytes))
                | dim | color(theme.palette->muted);
    }

    // Status row: line_status + time_display + size, gap = 1
    Elements status_row;
    if (!line_status.empty()) {
        status_row.push_back(line_status_el);
        status_row.push_back(text(" "));  // gap of 1
    }
    status_row.push_back(time_el);
    if (props.total_bytes.has_value()) {
        status_row.push_back(text(" "));  // gap of 1
        status_row.push_back(size_el);
    }

    auto status_hbox = hbox(std::move(status_row));

    // Main column: display + status row
    auto column = vbox({
        display_el,
        status_hbox,
    });

    // MessageResponse prefix
    auto prefix = text("  \xe2\x8f\xbf  ") | dim | color(theme.palette->muted);
    return vbox({ hbox({ prefix, column }) });
}

} // namespace cc::ui::components::shell
