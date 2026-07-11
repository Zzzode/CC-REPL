/// @file osc_9_4.cppm
/// @brief OSC 9;4 terminal progress reporting (iTerm2 / Ghostty / ConEmu).
///
/// Emits ANSI escape sequences for the terminal progress bar feature
/// supported by iTerm2 3.6.6+, Ghostty 1.2.0+, and ConEmu.
///
/// TS REF: src/ink/termio/osc.ts (ITERM2.PROGRESS constants, CLEAR_ITERM2_PROGRESS)
/// TS REF: src/ink/useTerminalNotification.ts (progress() callback)
/// TS REF: src/ink/terminal.ts (isProgressReportingAvailable())

module;

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

export module cc.utils.osc_9_4;

// TS REF: src/utils/clipboard.cppm uses `export namespace cc::utils::clipboard`
// pattern — functions inside can call each other without qualification.
namespace cc::utils::osc_9_4 {

// ============================================================
// Constants
// ============================================================

/// ESC character (0x1B).
constexpr char kEsc = '\x1b';

/// BEL character (0x07) — OSC terminator for non-kitty terminals.
constexpr char kBEL = '\x07';

/// OSC prefix: ESC ]
constexpr std::string_view kOscPrefix = "\x1b]";

// ============================================================
// Types
// ============================================================

/// Progress operation codes (TS REF: PROGRESS in osc.ts).
enum class ProgressOp : int {
    Clear         = 0,  ///< Remove progress indicator
    Set           = 1,  ///< Set percentage (0-100)
    Error         = 2,  ///< Mark as errored
    Indeterminate = 3,  ///< Spinner-style (no percentage)
};

// ============================================================
// Internal helpers
// ============================================================

namespace detail {

/// Parse "N.N.N" version into comparable int. Returns 0 on failure.
[[nodiscard]] inline int parse_version(std::string_view v) {
    int major = 0, minor = 0, patch = 0;
    int parsed = 0;
    int* fields[3] = {&major, &minor, &patch};
    for (char c : v) {
        if (c == '.') { if (parsed < 2) ++parsed; continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            *fields[parsed] = *fields[parsed] * 10 + (c - '0');
        } else { break; }
    }
    return major * 10000 + minor * 100 + patch;
}

/// Build raw OSC 9;4 sequence: ESC ] 9 ; 4 ; op ; param BEL
[[nodiscard]] inline std::string build_osc_9_4(ProgressOp op, int param = 0) {
    std::string seq = std::string(kOscPrefix) + "9;4;";
    seq += std::to_string(static_cast<int>(op));
    seq += ";";
    if (op == ProgressOp::Set || op == ProgressOp::Error) {
        seq += std::to_string(param);
    }
    seq += kBEL;
    return seq;
}

} // namespace detail

// ============================================================
// Multiplexer wrapping
// ============================================================

/// Wrap escape for tmux/screen DCS passthrough.
/// TS REF: wrapForMultiplexer() in osc.ts.
[[nodiscard]] inline std::string wrap_for_multiplexer(std::string_view sequence) {
    if (const char* tmux = std::getenv("TMUX"); tmux != nullptr) {
        std::string escaped;
        escaped.reserve(sequence.size() * 2);
        for (char c : sequence) {
            if (c == kEsc) { escaped.push_back(kEsc); escaped.push_back(kEsc); }
            else { escaped.push_back(c); }
        }
        return std::string("\x1bPtmux;") + escaped + "\x1b\\";
    }
    if (const char* sty = std::getenv("STY"); sty != nullptr) {
        return std::string("\x1bP") + std::string(sequence) + "\x1b\\";
    }
    return std::string(sequence);
}

// ============================================================
// Terminal support detection
// ============================================================

/// True if terminal supports OSC 9;4 progress.
/// TS REF: isProgressReportingAvailable() in terminal.ts.
[[nodiscard]] inline bool is_progress_reporting_available() {
    if (const char* dumb = std::getenv("TERM"); dumb != nullptr) {
        if (std::string_view(dumb) == "dumb") return false;
    }
    if (std::getenv("WT_SESSION") != nullptr) return false;
    if (std::getenv("ConEmuANSI") != nullptr ||
        std::getenv("ConEmuPID")  != nullptr ||
        std::getenv("ConEmuTask") != nullptr) {
        return true;
    }
    const char* program = std::getenv("TERM_PROGRAM");
    if (program == nullptr) return false;
    std::string_view prog(program);
    const char* ver_str = std::getenv("TERM_PROGRAM_VERSION");
    if (ver_str == nullptr) return false;
    int ver = detail::parse_version(ver_str);
    if (prog == "ghostty")   return ver >= 10200;
    if (prog == "iTerm.app") return ver >= 30606;
    return false;
}

} // namespace cc::utils::osc_9_4

// ============================================================
// Exported public API
// ============================================================

// Re-open the namespace for the exported declarations.
// The `export` keyword on individual declarations is what makes them
// visible to importers of this module.
export namespace cc::utils::osc_9_4 {

/// Emit progress (0-100). Returns multiplexer-wrapped OSC 9;4 sequence.
/// TS REF: progress('running', pct) in useTerminalNotification.ts
[[nodiscard]] inline std::string emit_terminal_progress(int percent) {
    int pct = std::clamp(percent, 0, 100);
    auto raw = detail::build_osc_9_4(ProgressOp::Set, pct);
    return wrap_for_multiplexer(raw);
}

/// Emit progress-clear (removes terminal progress bar).
/// TS REF: CLEAR_ITERM2_PROGRESS in osc.ts
[[nodiscard]] inline std::string emit_progress_clear() {
    auto raw = detail::build_osc_9_4(ProgressOp::Clear);
    return wrap_for_multiplexer(raw);
}

/// Emit indeterminate progress (spinner, no %).
/// TS REF: progress('indeterminate') in useTerminalNotification.ts
[[nodiscard]] inline std::string emit_progress_indeterminate() {
    auto raw = detail::build_osc_9_4(ProgressOp::Indeterminate);
    return wrap_for_multiplexer(raw);
}

/// Emit error-state progress.
/// TS REF: progress('error', pct) in useTerminalNotification.ts
[[nodiscard]] inline std::string emit_progress_error(int percent = 0) {
    int pct = std::clamp(percent, 0, 100);
    auto raw = detail::build_osc_9_4(ProgressOp::Error, pct);
    return wrap_for_multiplexer(raw);
}

/// Write progress to stdout + flush. No-op if unsupported terminal.
inline void write_progress(int percent) {
    if (!is_progress_reporting_available()) return;
    auto seq = emit_terminal_progress(percent);
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
}

/// Write progress-clear to stdout + flush.
inline void write_progress_clear() {
    if (!is_progress_reporting_available()) return;
    auto seq = emit_progress_clear();
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
}

/// Write indeterminate progress to stdout + flush.
inline void write_progress_indeterminate() {
    if (!is_progress_reporting_available()) return;
    auto seq = emit_progress_indeterminate();
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
}

} // export namespace cc::utils::osc_9_4
