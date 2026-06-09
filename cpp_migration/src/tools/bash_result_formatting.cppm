// BashTool result formatting helpers (migrated from BashToolResultMessage.tsx).
//
// ONLY pure functions, types, and string-formatting logic are ported here.
// The React JSX rendering (Box / Text / OutputLine / KeyboardShortcutHint /
// collapsed sections, etc.) is deferred to Phase 4 (FTXUI UI layer).
//
// Source split:
//   PART A (migrated here) : extractSandboxViolations, extractCwdResetWarning,
//                            format_exit_code, truncate_output_block,
//                            format_duration_ms, build_result_header,
//                            BashResultInfo struct
//   PART B (Phase 4 only)  : <Box>, <Text>, <OutputLine>, <MessageResponse>,
//                            <ShellTimeDisplay>, useXXX() hooks
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.bash_result_formatting;

import cc.utils.format;

export namespace cc::tools::bash {

using std::operator""sv;

// ---------------------------------------------------------------------------
// Constants (migrated from BashToolResultMessage.tsx + BashTool utils.ts)
// ---------------------------------------------------------------------------

/// Pattern to match "Shell cwd was reset to <path>" message at line start or
/// after a newline, anchored at end-of-string or end-of-line.
constexpr std::string_view kShellCwdResetPatternLiteral =
    "(?:^|\\n)(Shell cwd was reset to .+)$";

/// Sandbox-violations XML-ish tag boundaries as produced by the sandbox layer.
constexpr std::string_view kSandboxViolationsOpen  = "<sandbox_violations>";
constexpr std::string_view kSandboxViolationsClose = "</sandbox_violations>";

// ---------------------------------------------------------------------------
// BashResultInfo — structured view of a BashTool execution result.
// Produced by bash_tool execute() and consumed by the Phase 4 FTXUI UI layer.
// ---------------------------------------------------------------------------

struct BashResultInfo {
    std::string command;              // the original command string
    int exit_code = 0;                // decoded exit status (0 = OK)
    std::uint64_t duration_ms = 0;    // wall-clock duration of execution
    std::uint64_t stdout_bytes = 0;   // total stdout bytes before truncation
    std::uint64_t stderr_bytes = 0;   // total stderr bytes before truncation
    std::uint64_t stdout_lines = 0;   // total line count (before truncation)
    std::uint64_t stderr_lines = 0;   // total line count (before truncation)
    bool interrupted = false;         // killed by signal / timeout / user
    bool is_image = false;            // stdout is a data:image/...;base64,... URI
    bool no_output_expected = false;  // from silent-command detection
    bool background = false;          // started via run_in_background
    std::optional<std::string> return_code_interpretation; // from command_semantics
    std::optional<std::string> cwd_reset_warning;          // extracted from stderr
    std::optional<std::string> sandbox_violations;         // extracted from stderr
    std::optional<std::string> interrupted_reason;         // e.g. "timed out"
};

// ---------------------------------------------------------------------------
// 1. stderr content extraction helpers (PART A of BashToolResultMessage.tsx)
// ---------------------------------------------------------------------------

struct ExtractedStderr {
    std::string cleaned_stderr;
    std::optional<std::string> sandbox_violations;
    std::optional<std::string> cwd_reset_warning;
};

/// Strip <sandbox_violations>...</sandbox_violations> block from stderr.
/// Returns cleaned stderr and, if found, the raw violations content.
///
/// migrated: extractSandboxViolations() from BashToolResultMessage.tsx
inline auto extract_sandbox_violations(std::string_view stderr_sv)
    -> std::pair<std::string, std::optional<std::string>>
{
    const auto open  = stderr_sv.find(kSandboxViolationsOpen);
    const auto close = stderr_sv.find(kSandboxViolationsClose);
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return { std::string(stderr_sv), std::nullopt };
    }

    const auto content_start = open + kSandboxViolationsOpen.size();
    std::string violations(stderr_sv.substr(content_start, close - content_start));

    // Reassemble stderr without the <sandbox_violations>...</sandbox_violations> span
    std::string cleaned;
    cleaned.reserve(stderr_sv.size());
    cleaned.append(stderr_sv.substr(0, open));
    const auto after_close = close + kSandboxViolationsClose.size();
    if (after_close < stderr_sv.size()) cleaned.append(stderr_sv.substr(after_close));

    // Trim leading/trailing whitespace on cleaned output (like TS .trim())
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.front()))) cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.back())))  cleaned.pop_back();

    return { std::move(cleaned), std::move(violations) };
}

/// Extract the "Shell cwd was reset to ..." warning line from stderr, removing
/// it from the cleaned output so it can be rendered with a dedicated style.
///
/// migrated: extractCwdResetWarning() from BashToolResultMessage.tsx
inline auto extract_cwd_reset_warning(std::string_view stderr_sv)
    -> std::pair<std::string, std::optional<std::string>>
{
    try {
        const std::regex pattern(std::string(kShellCwdResetPatternLiteral),
                                 std::regex::multiline);
        std::string input(stderr_sv);
        std::smatch match;
        if (!std::regex_search(input, match, pattern) || match.size() < 2) {
            return { std::string(stderr_sv), std::nullopt };
        }

        std::string warning(match[1].first, match[1].second);
        // Remove the full match from stderr and trim
        std::string cleaned = std::regex_replace(
            input, pattern, "",
            std::regex_constants::format_first_only);

        // Trim
        while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.front()))) cleaned.erase(cleaned.begin());
        while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.back())))  cleaned.pop_back();

        return { std::move(cleaned), std::move(warning) };
    } catch (const std::regex_error&) {
        // Fallback on malformed regex (shouldn't happen, but be safe).
        return { std::string(stderr_sv), std::nullopt };
    }
}

/// Combined extraction pipeline: sandbox -> cwd-reset, in the same order the
/// TS React component applies them. Returns both cleaned stderr and any
/// extracted meta-strings the UI layer needs to render separately.
inline auto extract_all_stderr_meta(std::string_view stderr_sv) -> ExtractedStderr {
    auto [no_violations, violations] = extract_sandbox_violations(stderr_sv);
    auto [cleaned, cwd_warning]      = extract_cwd_reset_warning(no_violations);
    return {
        .cleaned_stderr       = std::move(cleaned),
        .sandbox_violations   = std::move(violations),
        .cwd_reset_warning    = std::move(cwd_warning),
    };
}

// ---------------------------------------------------------------------------
// 2. Exit-code / duration / truncation display helpers
// ---------------------------------------------------------------------------

/// Human-friendly, optionally ANSI-coloured exit-code label.
///   exit 0  -> "exit 0  OK"  (green when colour is enabled)
///   exit !0 -> "exit N  FAILED"  (red)
///   interrupted -> "interrupted  (SIGKILL / timed-out / ...)"
///
/// migrated: implicit per the TS OutputLine colouring, lifted to an explicit fn
inline auto format_exit_code(int code, bool interrupted = false) -> std::string {
    if (interrupted) {
        return cc::utils::ansi::yellow("interrupted");
    }
    if (code == 0) {
        return std::string("exit 0  ") + cc::utils::ansi::green("OK");
    }
    return std::string("exit ") + std::to_string(code) + "  " +
           cc::utils::ansi::red("FAILED");
}

/// Format a millisecond duration into short human-readable form:
///   <1ms        -> "0ms"
///   <1000ms     -> "456ms"
///   >=1000ms    -> "1.23s"     (2 decimals)
///   >=60_000ms  -> "2m 05s"    (delegates to utils format_duration)
///
/// migrated: aligns with ShellTimeDisplay's output format heuristic.
inline auto format_duration_ms(std::uint64_t ms) -> std::string {
    if (ms < 1000) {
        return std::to_string(ms) + "ms";
    }
    if (ms < 60'000) {
        double secs = static_cast<double>(ms) / 1000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << secs << "s";
        return oss.str();
    }
    // Delegate to the existing seconds-granularity formatter for long runs.
    auto secs = std::chrono::seconds(static_cast<long long>(ms / 1000));
    return cc::utils::format_duration(secs);
}

/// Count newlines; a non-empty string with no trailing newline still has one
/// logical "line" (matches TS countCharInString(content, '\n') + 1).
constexpr auto count_lines(std::string_view s) -> std::uint64_t {
    if (s.empty()) return 0;
    std::uint64_t n = 1;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

/// Head+tail truncation with a "N lines / M bytes truncated" notice, designed
/// for display in a terminal UI. Caller supplies both a line cap and a byte
/// cap; whichever is hit first applies. Output preserves both the start and
/// end of long output so error context (which is often at the end) is visible.
///
/// migrated: combines TS formatOutput (content cap) + detail::truncate_output
/// from bash_tool.cppm (half+half), with added line-level awareness.
inline auto truncate_output_block(
    std::string_view text,
    std::size_t max_lines,
    std::size_t max_chars
) -> std::string
{
    if (text.empty()) return {};

    const std::uint64_t total_lines = count_lines(text);
    const std::size_t  total_chars  = text.size();

    // Fast path: everything fits
    if (total_lines <= static_cast<std::uint64_t>(max_lines) &&
        total_chars <= max_chars) {
        return std::string(text);
    }

    // Decide how many characters to actually keep. Use half+half strategy so
    // the user sees both the start (command echo / headers) and end (errors).
    const std::size_t char_cap = std::min(max_chars, total_chars);
    const std::size_t half     = char_cap / 2;

    // Find head boundary: walk back half bytes, then snap to line boundary
    // so we don't split a line mid-stream unless we really have to.
    std::size_t head_end = half;
    if (head_end < text.size()) {
        auto snap = text.rfind('\n', head_end);
        if (snap != std::string_view::npos && snap > 0) head_end = snap + 1;
    }

    // Find tail boundary (offset from end)
    std::size_t tail_start = (char_cap < text.size()) ? (text.size() - (char_cap - half)) : 0;
    if (tail_start < text.size() && tail_start > 0) {
        auto snap = text.find('\n', tail_start);
        if (snap != std::string_view::npos) tail_start = snap + 1;
    }

    // Avoid overlap (tiny output)
    if (tail_start <= head_end) {
        tail_start = head_end;
    }

    const auto omitted_lines = total_lines > max_lines
        ? total_lines - max_lines
        : 0;
    const auto omitted_chars = total_chars > char_cap
        ? total_chars - char_cap
        : 0;

    std::string notice;
    if (omitted_lines > 0 && omitted_chars > 0) {
        notice = std::format("\n\n... [{} lines / {} bytes truncated] ...\n\n",
                             omitted_lines, omitted_chars);
    } else if (omitted_lines > 0) {
        notice = std::format("\n\n... [{} lines truncated] ...\n\n", omitted_lines);
    } else if (omitted_chars > 0) {
        notice = std::format("\n\n... [{} bytes truncated] ...\n\n", omitted_chars);
    } else {
        notice = "\n\n... [truncated] ...\n\n";
    }

    std::string result;
    result.reserve(head_end + notice.size() + (text.size() - tail_start));
    result.append(text.substr(0, head_end));
    result.append(notice);
    if (tail_start < text.size()) result.append(text.substr(tail_start));
    return result;
}

// ---------------------------------------------------------------------------
// 3. Result header — a small list of formatted lines the FTXUI (Phase 4) layer
//    can render as the top of each BashTool result card.
//    Returns lines WITHOUT trailing newlines; FTXUI concatenates them.
// ---------------------------------------------------------------------------

inline auto build_result_header(const BashResultInfo& info) -> std::vector<std::string> {
    std::vector<std::string> lines;
    lines.reserve(4);

    // Line 1: command + (if background) tag
    {
        std::ostringstream oss;
        if (info.background) {
            oss << cc::utils::ansi::cyan("[background]") << " ";
        }
        // Prefer sanitized (no secrets) command display. Caller can override by
        // pre-sanitizing info.command; we do a best-effort simple trim here.
        oss << cc::utils::ansi::bold(info.command.empty()
                    ? std::string("<empty command>")
                    : info.command);
        lines.push_back(oss.str());
    }

    // Line 2: exit code + duration + output sizes
    {
        std::ostringstream oss;
        oss << format_exit_code(info.exit_code, info.interrupted);
        if (info.duration_ms > 0) {
            oss << "  " << cc::utils::ansi::dim(format_duration_ms(info.duration_ms));
        }

        // Output-size summary (useful for collapsed cards)
        const auto fmt_bytes = [](std::uint64_t n) { return cc::utils::format_bytes(n); };
        oss << "  " << cc::utils::ansi::dim(
            std::format("stdout {} ({} line{})",
                        fmt_bytes(info.stdout_bytes),
                        info.stdout_lines,
                        info.stdout_lines == 1 ? "" : "s"));
        if (info.stderr_bytes > 0) {
            oss << "  " << cc::utils::ansi::red(cc::utils::ansi::dim(
                std::format("stderr {} ({} line{})",
                            fmt_bytes(info.stderr_bytes),
                            info.stderr_lines,
                            info.stderr_lines == 1 ? "" : "s")));
        }

        lines.push_back(oss.str());
    }

    // Line 3: interrupted reason, if any
    if (info.interrupted && info.interrupted_reason) {
        lines.push_back(cc::utils::ansi::yellow(
            std::format("note: {}", *info.interrupted_reason)));
    }

    // Line 4: return-code interpretation / semantic message
    if (info.return_code_interpretation) {
        lines.push_back(cc::utils::ansi::dim(
            std::format("note: {}", *info.return_code_interpretation)));
    }

    // NOTE: cwd_reset_warning / sandbox_violations / stdout / stderr bodies are
    // deliberately NOT in the header — they are emitted as separate blocks
    // by the UI layer (Phase 4). Callers can use extract_all_stderr_meta()
    // above if they need them formatted before display.

    return lines;
}

// ---------------------------------------------------------------------------
// 4. Convenience: build a BashResultInfo from raw fields (counts lines/bytes
//    for the caller). Useful at the bash_tool.cppm format_result boundary.
// ---------------------------------------------------------------------------

inline auto make_result_info(
    std::string command,
    int exit_code,
    std::uint64_t duration_ms,
    std::string_view stdout_sv,
    std::string_view stderr_sv,
    bool interrupted = false,
    bool is_image = false,
    bool no_output_expected = false,
    bool background = false,
    std::optional<std::string> return_code_interpretation = std::nullopt,
    std::optional<std::string> interrupted_reason = std::nullopt
) -> BashResultInfo {
    BashResultInfo info;
    info.command                      = std::move(command);
    info.exit_code                    = exit_code;
    info.duration_ms                  = duration_ms;
    info.stdout_bytes                 = stdout_sv.size();
    info.stderr_bytes                 = stderr_sv.size();
    info.stdout_lines                 = count_lines(stdout_sv);
    info.stderr_lines                 = count_lines(stderr_sv);
    info.interrupted                  = interrupted;
    info.is_image                     = is_image;
    info.no_output_expected           = no_output_expected;
    info.background                   = background;
    info.return_code_interpretation   = std::move(return_code_interpretation);
    info.interrupted_reason           = std::move(interrupted_reason);

    // Extract metadata from stderr so UI layer doesn't have to.
    auto meta = extract_all_stderr_meta(stderr_sv);
    info.cwd_reset_warning  = std::move(meta.cwd_reset_warning);
    info.sandbox_violations = std::move(meta.sandbox_violations);
    return info;
}

} // namespace cc::tools::bash
