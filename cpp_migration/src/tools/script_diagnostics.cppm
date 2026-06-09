// Script diagnostics — parsing, formatting, and display helpers for
// LSP/TypeScript/GCC/Python style compiler diagnostics.
//
// Migrated from:
//   src/tools/ScriptTool/formatDiagnostics.ts  (formatSyntaxError,
//                                                formatTypeCheckFailure,
//                                                adjustLineNumbers, etc.)
// Merged with pre-existing parse + summary formatters that lived here.
module;
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.tools.script_diagnostics;

export namespace cc::tools {

// =========================================================================
// Diagnostic data structure
// =========================================================================

/// Diagnostic severity level — matches the LSP DiagnosticSeverity enum
/// and the Level previously defined in this module (kept as Level for
/// backward compatibility with parse_compiler_output callers).
enum class DiagnosticLevel : uint8_t {
    Error   = 1,
    Warning = 2,
    Info    = 3,
    Hint    = 4,
};

/// Backward-compatible alias — parse_compiler_output historically used
/// `Diagnostic::Level` so we keep the short name.
using DiagnosticSeverity [[deprecated("use DiagnosticLevel")]] = DiagnosticLevel;

/// A single compiler / LSP / type-checker diagnostic.
///
/// This struct intentionally unifies two previously separate notions:
///   * the GCC-style Diagnostic (file + line + column + level) that
///     `parse_compiler_output` produced, and
///   * the BuildMessage-like / LSP Diagnostic that carries `code`,
///     `source`, the offending source `line_text` and the `length` of
///     the error range (for caret rendering).
struct Diagnostic {
    std::filesystem::path file;
    size_t line   = 0;  ///< 1-based line number (0 = unknown)
    size_t column = 0;  ///< 1-based column (0 = unknown)
    size_t length = 0;  ///< length of the error range in characters
    std::string message;
    DiagnosticLevel level = DiagnosticLevel::Error;

    /// Diagnostic *code*, e.g. "TS2322" or "2322". Empty if absent.
    std::optional<std::string> code;

    /// Producing tool / source, e.g. "typescript", "gcc", "pyright".
    std::optional<std::string> source;

    /// The source line the diagnostic points at. Used to render a
    /// `line_text / ^^^` snippet under the diagnostic header.
    std::optional<std::string> line_text;

    // ------------------------------------------------------------------
    // Legacy `Level` alias — keeps parse_compiler_output compiling.
    // ------------------------------------------------------------------
    using Level [[deprecated("use DiagnosticLevel")]] = DiagnosticLevel;
};

// =========================================================================
// ANSI styling helpers — intentionally light-weight so we don't pull in
// any new dependency. Consumers that run without a TTY can strip the
// escape codes later via cc.utils.ansi_rendering::strip_ansi_codes.
// =========================================================================
namespace ansi {
    constexpr std::string_view reset   = "\033[0m";
    constexpr std::string_view bold    = "\033[1m";
    constexpr std::string_view red     = "\033[31m";
    constexpr std::string_view yellow  = "\033[33m";
    constexpr std::string_view blue    = "\033[34m";
    constexpr std::string_view magenta = "\033[35m";
    constexpr std::string_view cyan    = "\033[36m";
    constexpr std::string_view gray    = "\033[90m";
    constexpr std::string_view green   = "\033[32m";
} // namespace ansi

// =========================================================================
// Compiler-output parsing (preserved from previous revision, updated to
// populate the extended Diagnostic fields).
// =========================================================================

inline auto parse_compiler_output(std::string_view output)
    -> std::vector<Diagnostic>
{
    std::vector<Diagnostic> diagnostics;

    // GCC/Clang: file:line:col: error/warning: message
    // TypeScript: file(line,col): error TSxxxx: message
    // Python:     File "file", line N

    static const std::regex gcc_pattern(
        R"(([^:\s]+):(\d+):(\d+):\s*(error|warning|note):\s*(.+))"
    );
    static const std::regex ts_pattern(
        R"(([^(]+)\((\d+),(\d+)\):\s*(error|warning)\s+(\w+):\s*(.+))"
    );
    static const std::regex py_pattern(
        R"re(File "([^"]+)", line (\d+))re"
    );

    std::istringstream stream{std::string(output)};
    std::string line;

    while (std::getline(stream, line)) {
        std::smatch match;

        if (std::regex_search(line, match, gcc_pattern)) {
            Diagnostic diag;
            diag.file   = match[1].str();
            diag.line   = static_cast<size_t>(std::stoi(match[2].str()));
            diag.column = static_cast<size_t>(std::stoi(match[3].str()));
            diag.message = match[5].str();

            const std::string level_str = match[4].str();
            if (level_str == "error")       diag.level = DiagnosticLevel::Error;
            else if (level_str == "warning") diag.level = DiagnosticLevel::Warning;
            else                             diag.level = DiagnosticLevel::Info;

            diagnostics.push_back(std::move(diag));
            continue;
        }

        if (std::regex_search(line, match, ts_pattern)) {
            Diagnostic diag;
            diag.file   = match[1].str();
            diag.line   = static_cast<size_t>(std::stoi(match[2].str()));
            diag.column = static_cast<size_t>(std::stoi(match[3].str()));
            diag.code   = match[5].str();
            diag.message = match[6].str();
            diag.source  = "typescript";

            const std::string level_str = match[4].str();
            diag.level = (level_str == "error")
                ? DiagnosticLevel::Error
                : DiagnosticLevel::Warning;

            diagnostics.push_back(std::move(diag));
            continue;
        }

        if (std::regex_search(line, match, py_pattern)) {
            Diagnostic diag;
            diag.file   = match[1].str();
            diag.line   = static_cast<size_t>(std::stoi(match[2].str()));
            diag.column = 0;
            diag.level  = DiagnosticLevel::Error;

            // Python tracebacks stack the message two lines after the
            // "File ... line N" header. Try to grab it.
            std::string next_line;
            if (std::getline(stream, next_line) && std::getline(stream, next_line))
                diag.message = next_line;

            diagnostics.push_back(std::move(diag));
        }
    }

    return diagnostics;
}

// =========================================================================
// Line-number adjustment — mirrors TS `adjustLineNumbers`.
//
// Transpilers that inject a preamble into user code emit line numbers
// relative to the *preambled* source. Subtract `preamble_offset` from
// every occurrence of /line N/ inside `message` so humans see the real
// user-code line. Any result that would fall below 1 is left alone.
// =========================================================================
inline auto adjust_line_numbers(std::string message, int preamble_offset)
    -> std::string
{
    if (preamble_offset == 0) return message;

    static const std::regex line_pattern(R"re(\bline\s+(\d+))re",
                                         std::regex_constants::icase);

    auto begin = std::sregex_iterator(message.begin(), message.end(), line_pattern);
    auto end   = std::sregex_iterator();

    // Build result in reverse so offsets from earlier replacements don't
    // invalidate the positions of later matches.
    std::vector<std::pair<size_t, std::string>> replacements;
    replacements.reserve(std::distance(begin, end));

    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        const int raw = std::stoi(m[1].str());
        const int adj = raw - preamble_offset;
        if (adj < 1) continue;  // TS behaviour: keep original if <1
        replacements.emplace_back(
            static_cast<size_t>(m.position(1)),
            std::to_string(adj)
        );
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& [pos, text] : replacements) {
        // Match group 1 starts at pos; find its length (contiguous digits)
        size_t len = 0;
        while (pos + len < message.size() &&
               std::isdigit(static_cast<unsigned char>(message[pos + len])))
            ++len;
        message.replace(pos, len, text);
    }

    return message;
}

// =========================================================================
// Level → display helpers
// =========================================================================

[[nodiscard]] inline constexpr std::string_view
level_tag(DiagnosticLevel lvl) noexcept
{
    switch (lvl) {
        case DiagnosticLevel::Error:   return "ERROR";
        case DiagnosticLevel::Warning: return "WARN ";
        case DiagnosticLevel::Info:    return "INFO ";
        case DiagnosticLevel::Hint:    return "HINT ";
    }
    return "INFO ";
}

[[nodiscard]] inline constexpr std::string_view
level_icon(DiagnosticLevel lvl) noexcept
{
    switch (lvl) {
        case DiagnosticLevel::Error:   return "✖";
        case DiagnosticLevel::Warning: return "⚠";
        case DiagnosticLevel::Info:    return "ℹ";
        case DiagnosticLevel::Hint:    return "✎";
    }
    return "ℹ";
}

[[nodiscard]] inline constexpr std::string_view
level_color(DiagnosticLevel lvl) noexcept
{
    switch (lvl) {
        case DiagnosticLevel::Error:   return ansi::red;
        case DiagnosticLevel::Warning: return ansi::yellow;
        case DiagnosticLevel::Info:    return ansi::blue;
        case DiagnosticLevel::Hint:    return ansi::gray;
    }
    return ansi::reset;
}

// =========================================================================
// format_diagnostics — the primary pretty-printer for a batch of
// diagnostics. Produces aligned, coloured output with optional source
// snippets and caret indicators.
//
// This replaces the two previously-split implementations in this module
// and in lsp_formatters.cppm::format_diagnostic_list, while adding:
//   * ANSI colouring (strippable)
//   * caret-under-range rendering when `line_text` + `length` available
//   * summary header with error/warning/info counts
//   * optional `max_display` truncation, same as the original
//   * `code` (e.g. TS2322) and `source` rendering
// =========================================================================

struct FormatDiagnosticOptions {
    /// Maximum number of diagnostics to print before the "... and N more"
    /// line. 0 or negative = unlimited.
    int max_display = 50;

    /// Render ANSI colour codes. Set false for plain-text / logging.
    bool use_colors = true;

    /// Include the source-line snippet with `^^^` caret when possible.
    bool show_snippets = true;

    /// Align the message body so the longest file:line:col prefix ends
    /// at the same column for every diagnostic. Looks much cleaner for
    /// multi-diagnostic blocks.
    bool align_messages = true;
};

inline auto format_diagnostics(
    std::span<const Diagnostic> diagnostics,
    const FormatDiagnosticOptions& opts = {}
) -> std::string
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());

    if (diagnostics.empty()) {
        oss << (opts.use_colors ? std::string(ansi::green) + "No diagnostics reported." + ansi::reset
                                : "No diagnostics reported.");
        return oss.str();
    }

    // ------------------------------------------------------------------
    // Summary header
    // ------------------------------------------------------------------
    size_t errors = 0, warnings = 0, infos = 0, hints = 0;
    for (const auto& d : diagnostics) {
        switch (d.level) {
            case DiagnosticLevel::Error:   ++errors;   break;
            case DiagnosticLevel::Warning: ++warnings; break;
            case DiagnosticLevel::Info:    ++infos;    break;
            case DiagnosticLevel::Hint:    ++hints;    break;
        }
    }

    if (opts.use_colors) oss << ansi::bold;
    oss << "Found " << diagnostics.size() << " diagnostic"
        << (diagnostics.size() == 1 ? "" : "s") << ": ";
    if (opts.use_colors) oss << ansi::reset;

    bool first = true;
    auto append = [&](size_t n, std::string_view label, std::string_view col) {
        if (n == 0) return;
        if (!first) oss << ", ";
        first = false;
        if (opts.use_colors) oss << col;
        oss << n << " " << label;
        if (opts.use_colors) oss << ansi::reset;
    };
    append(errors,   errors   == 1 ? "error"   : "errors",   ansi::red);
    append(warnings, warnings == 1 ? "warning" : "warnings", ansi::yellow);
    append(infos,    infos    == 1 ? "info"    : "infos",    ansi::blue);
    append(hints,    hints    == 1 ? "hint"    : "hints",    ansi::gray);
    oss << "\n\n";

    // ------------------------------------------------------------------
    // Compute alignment width (max file:line:col prefix length)
    // ------------------------------------------------------------------
    size_t prefix_w = 0;
    if (opts.align_messages) {
        const size_t cap = static_cast<size_t>(
            opts.max_display > 0
                ? std::min<size_t>(diagnostics.size(),
                                   static_cast<size_t>(opts.max_display))
                : diagnostics.size());
        for (size_t i = 0; i < cap; ++i) {
            const auto& d = diagnostics[i];
            size_t w = d.file.empty() ? 0 : d.file.filename().string().size();
            if (d.line)   w += 1 + std::to_string(d.line).size();
            if (d.column) w += 1 + std::to_string(d.column).size();
            if (w > prefix_w) prefix_w = w;
        }
        if (prefix_w) prefix_w += 2; // breathing room
    }

    // ------------------------------------------------------------------
    // Render each diagnostic
    // ------------------------------------------------------------------
    const size_t limit =
        opts.max_display > 0
            ? std::min<size_t>(diagnostics.size(),
                               static_cast<size_t>(opts.max_display))
            : diagnostics.size();

    for (size_t i = 0; i < limit; ++i) {
        const auto& d = diagnostics[i];

        // Icon + level label
        const auto col = opts.use_colors ? level_color(d.level) : std::string_view{};
        if (opts.use_colors) oss << col << ansi::bold;
        oss << level_icon(d.level) << " " << level_tag(d.level);
        if (opts.use_colors) oss << ansi::reset << col;

        // Location prefix
        std::ostringstream location;
        if (!d.file.empty()) {
            location << d.file.filename().string();
            if (d.line) {
                location << ":" << d.line;
                if (d.column) location << ":" << d.column;
            }
        }
        auto loc_str = location.str();
        if (!loc_str.empty()) oss << " " << loc_str;

        if (opts.align_messages && prefix_w > loc_str.size())
            oss << std::string(prefix_w - loc_str.size(), ' ');
        else if (!loc_str.empty())
            oss << " ";

        // Optional code tag — " [TS2322]"
        if (d.code && !d.code->empty()) {
            oss << "[";
            if (opts.use_colors) oss << ansi::cyan;
            oss << *d.code;
            if (opts.use_colors) oss << ansi::reset << col;
            oss << "] ";
        } else if (!d.source && !loc_str.empty()) {
            oss << "- ";
        } else if (!loc_str.empty()) {
            oss << "- ";
        }

        // Message (line-number adjusted for source when possible)
        oss << d.message;
        if (opts.use_colors) oss << ansi::reset;

        // Source origin (e.g. "  (gcc)" dimmed)
        if (d.source && !d.source->empty()) {
            if (opts.use_colors) oss << ansi::gray;
            oss << "  (" << *d.source << ")";
            if (opts.use_colors) oss << ansi::reset;
        }

        oss << "\n";

        // Source snippet + caret
        if (opts.show_snippets && d.line_text && !d.line_text->empty()) {
            const size_t caret_pad = (d.column >= 1) ? (d.column - 1) : 0;
            const size_t caret_len = std::max<size_t>(1, d.length ? d.length : 1);
            if (opts.use_colors) oss << ansi::gray;
            oss << "    " << *d.line_text << "\n";
            oss << "    "
                << std::string(caret_pad, ' ')
                << std::string(caret_len, '^');
            if (opts.use_colors) oss << ansi::reset;
            oss << "\n";
        }
    }

    if (opts.max_display > 0 &&
        static_cast<size_t>(opts.max_display) < diagnostics.size()) {
        const size_t rest = diagnostics.size()
                          - static_cast<size_t>(opts.max_display);
        if (opts.use_colors) oss << ansi::gray;
        oss << "... and " << rest << " more diagnostic"
            << (rest == 1 ? "" : "s") << ".";
        if (opts.use_colors) oss << ansi::reset;
        oss << "\n";
    }

    return oss.str();
}

// Backward-compatible overload — the previous signature took just
// `int max_display`. Keep it working for existing callers.
inline auto format_diagnostics(
    std::span<const Diagnostic> diagnostics,
    int max_display
) -> std::string
{
    FormatDiagnosticOptions opts;
    opts.max_display = max_display;
    return format_diagnostics(diagnostics, opts);
}

// =========================================================================
// format_syntax_error — specialised pretty-printer for "syntax errors"
// produced by a transpiler that embeds user code after a fixed preamble.
//
// Mirrors TS `formatSyntaxError(BuildMessageLike[], preambleLineCount)`.
//
// Each diagnostic's line number is offset-corrected before display.
// When `line_text` is present the diagnostic is rendered with a caret
// under the offending range.
// =========================================================================
struct SyntaxErrorInput {
    struct Msg {
        std::string message;
        size_t line   = 0;       // preamble-inclusive (1-based)
        size_t column = 0;       // 1-based
        size_t length = 0;       // chars of the offending range
        std::optional<std::string> line_text;
    };
    std::vector<Msg> messages;
    int preamble_line_count = 0;

    /// Optional filename shown in headers (transpiler diagnostics often
    /// don't have a real file, so this can stay empty).
    std::optional<std::string> filename;
};

inline auto format_syntax_error(const SyntaxErrorInput& input) -> std::string
{
    std::ostringstream oss;

    // Header
    if (input.messages.empty()) {
        return "Syntax error: (no detail available)";
    }

    const auto n = input.messages.size();
    if (n == 1) oss << "Syntax error (1 diagnostic):\n";
    else        oss << "Syntax errors (" << n << " diagnostics):\n";

    for (const auto& m : input.messages) {
        // Adjust line for preamble — guard against dropping below 1
        const size_t adj_line =
            (m.line > static_cast<size_t>(input.preamble_line_count))
                ? m.line - static_cast<size_t>(input.preamble_line_count)
                : 1;

        if (m.line || m.column) {
            oss << "  line " << adj_line;
            if (m.column) oss << ", col " << m.column;
            oss << ": ";
        } else {
            oss << "  ";
        }
        oss << adjust_line_numbers(m.message, input.preamble_line_count)
            << "\n";

        if (m.line_text && !m.line_text->empty()) {
            const size_t pad = (m.column >= 1) ? (m.column - 1) : 0;
            const size_t len = std::max<size_t>(1, m.length ? m.length : 1);
            oss << "    " << *m.line_text << "\n"
                << "    " << std::string(pad, ' ')
                << std::string(len, '^') << "\n";
        }
    }

    return oss.str();
}

// =========================================================================
// format_type_check_failure — specialised printer for type-checker
// reports (TypeScript / pyright / etc.).
//
// Mirrors TS `formatTypeCheckFailure`. Only `Error`-level diagnostics
// are rendered by default; callers can opt into showing warnings.
// =========================================================================
struct TypeCheckReport {
    std::vector<Diagnostic> diagnostics;
    size_t error_count   = 0;   // may be > diagnostics.size() on truncation
    size_t warning_count = 0;
    size_t total_diagnostic_count = 0;
    std::chrono::milliseconds duration{0};
    bool truncated = false;
    bool show_warnings = false;
};

inline auto format_type_check_failure(const TypeCheckReport& report)
    -> std::string
{
    std::ostringstream oss;
    oss << "Type check failed with " << report.error_count << " error(s)";
    if (report.warning_count > 0 && report.show_warnings)
        oss << ", " << report.warning_count << " warning(s)";
    oss << " in " << report.duration.count() << "ms.\n";

    size_t rendered = 0;
    for (const auto& d : report.diagnostics) {
        if (!report.show_warnings && d.level != DiagnosticLevel::Error)
            continue;

        // "line 3, col 7 TS2322: message" style — matches TS output
        oss << "line " << d.line << ", col " << d.column;
        if (d.code && !d.code->empty()) oss << " " << *d.code;
        oss << ": " << d.message << "\n";
        ++rendered;
    }

    if (report.truncated) {
        const size_t remaining =
            report.total_diagnostic_count > rendered
                ? report.total_diagnostic_count - rendered
                : 0;
        if (remaining > 0) {
            oss << "... and " << remaining << " more diagnostic(s).\n";
        }
    }

    return oss.str();
}

// =========================================================================
// group_by_file — preserved helper, unchanged API but now uses the
// extended Diagnostic struct.
// =========================================================================
inline auto group_by_file(std::span<const Diagnostic> diagnostics)
    -> std::map<std::filesystem::path, std::vector<Diagnostic>>
{
    std::map<std::filesystem::path, std::vector<Diagnostic>> grouped;
    for (const auto& diag : diagnostics)
        grouped[diag.file].push_back(diag);

    for (auto& [_, diags] : grouped) {
        std::sort(diags.begin(), diags.end(),
                  [](const Diagnostic& a, const Diagnostic& b) {
                      if (a.line != b.line) return a.line < b.line;
                      return a.column < b.column;
                  });
    }
    return grouped;
}

} // namespace cc::tools
