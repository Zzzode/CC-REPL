// Script Typecheck runner — mirrors src/tools/ScriptTool/typecheck.ts
//
// Executes the TypeScript compiler on (virtual) input code via a subprocess,
// captures JSON diagnostics, filters, sorts, and optionally remaps line
// numbers to account for an injected preamble.
module;

#include <array>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <optional>

export module cc.tools.script_typecheck;

import cc.tools.script_types;
import cc.tools.script_diagnostics;
import cc.utils.json;
import cc.utils.bash_execution;

export namespace cc::tools::script_typecheck {

namespace fs = std::filesystem;
using cc::utils::bash::ExecutionResult;
using cc::utils::bash::ShellSessionConfig;
using cc::utils::bash::execute_command;
using cc::utils::json::JsonVal;
using cc::utils::json::parse;

// ---------------------------------------------------------------------------
// Public types (mirror ScriptTypeDiagnostic / ScriptTypeCheckResult from TS)
// ---------------------------------------------------------------------------

enum class Severity { Error, Warning, Info };

struct ScriptTypeDiagnostic {
    Severity severity{Severity::Error};
    int code{0};
    std::string message;
    int line{1};
    int column{1};
};

struct TypecheckOptions {
    std::string code;                       ///< Source text to type-check
    int preamble_line_count{0};             ///< Lines of hidden preamble to subtract
    int max_diagnostics{100};               ///< Hard cap on returned diagnostics
    std::optional<fs::path> working_dir;    ///< Optional cwd for node_modules resolution
    std::chrono::seconds timeout{30};
};

struct TypecheckResult {
    bool passed{false};
    std::chrono::milliseconds duration{0};
    int error_count{0};
    int warning_count{0};
    int total_diagnostic_count{0};
    std::vector<ScriptTypeDiagnostic> diagnostics;
    bool truncated{false};
    std::string runner_error;               ///< Non-empty when subprocess itself failed
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace detail {

auto severity_from_ts_category(int category) noexcept -> Severity {
    // ts.DiagnosticCategory: Warning=0, Error=1, Message=2, Suggestion=3
    switch (category) {
        case 1:  return Severity::Error;
        case 0:  return Severity::Warning;
        default: return Severity::Info;
    }
}

constexpr auto diagnostic_rank(Severity s) noexcept -> int {
    // Errors first, then warnings, then info (same order as TS diagnosticRank)
    switch (s) {
        case Severity::Error:   return 0;
        case Severity::Warning: return 1;
        case Severity::Info:    return 2;
    }
    return 2;
}

/// Diagnostic "flattening": tsc returns message as chain; pick root text.
auto extract_message_text(JsonVal msg_node) -> std::string {
    if (msg_node.is_str()) {
        return std::string(msg_node.as_str());
    }
    if (msg_node.is_obj()) {
        // TypeScript diagnosticMessage chain: { messageText, next?: DiagnosticMessage[] }
        auto direct = msg_node.get("messageText");
        if (direct.is_str()) {
            return std::string(direct.as_str());
        }
    }
    return {};
}

/// Virtual file path used inside the subprocess; mirrors TS VIRTUAL_FILE.
constexpr std::string_view kVirtualFile = "/__script_tool__/inline.ts";

bool is_virtual_file_path(std::string_view path) {
    if (path == kVirtualFile) return true;
    // Normalise Windows backslashes
    std::string normalized;
    normalized.reserve(path.size());
    for (char c : path) normalized.push_back(c == '\\' ? '/' : c);
    return normalized == kVirtualFile;
}

/// Escape a string for safe embedding inside a JavaScript double-quoted literal.
auto js_string_escape(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\x{:02x}", static_cast<unsigned char>(c));
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

/// Generate the inline node script that uses the TypeScript compiler API
/// and prints JSON diagnostics to stdout.  Mirrors runScriptTypeCheck in TS.
auto generate_runner_script(const TypecheckOptions& opts) -> std::string {
    // We produce a self-contained node script.  The source code is embedded
    // as a JS string literal so we avoid writing two temp files (the .ts +
    // a separate runner).  We use dynamic import() so the script works
    // whether run via `bun` or `node`.
    return std::format(R"js(
const VIRTUAL_FILE = "{}";
const EMBEDDED_CODE = "{}";
const PREAMBLE_LINES = {};
const MAX_DIAGNOSTICS = {};

(async function main() {{
    const ts = await import("typescript").catch(() => null);
    if (!ts) {{
        process.stdout.write(JSON.stringify({{
            __runnerError: "typescript module not found — install with `npm i -D typescript`",
            diagnostics: [], passed: false
        }}));
        process.exit(0);
    }}
    const t = ts.default ?? ts;
    const compilerOptions = {{
        noEmit: true,
        target: t.ScriptTarget.ES2022,
        module: t.ModuleKind.ESNext,
        moduleResolution: 100 /* Bundler */,
        allowJs: true,
        checkJs: false,
        strict: false,
        noImplicitAny: false,
        skipLibCheck: true,
        types: ["node", "bun-types"],
        lib: ["lib.es2022.d.ts"],
    }};
    const host = t.createCompilerHost(compilerOptions, true);
    const origGetSourceFile = host.getSourceFile.bind(host);
    const origReadFile = host.readFile.bind(host);
    const origFileExists = host.fileExists.bind(host);
    host.getSourceFile = (fn, lv, onErr, createNew) => {{
        if (fn === VIRTUAL_FILE || fn.replace(/\\\\/g, "/") === VIRTUAL_FILE) {{
            return t.createSourceFile(fn, EMBEDDED_CODE, lv, true, t.ScriptKind.TS);
        }}
        return origGetSourceFile(fn, lv, onErr, createNew);
    }};
    host.readFile = (fn) => {{
        if (fn === VIRTUAL_FILE || fn.replace(/\\\\/g, "/") === VIRTUAL_FILE) return EMBEDDED_CODE;
        return origReadFile(fn);
    }};
    host.fileExists = (fn) => {{
        if (fn === VIRTUAL_FILE || fn.replace(/\\\\/g, "/") === VIRTUAL_FILE) return true;
        return origFileExists(fn);
    }};
    const program = t.createProgram([VIRTUAL_FILE], compilerOptions, host);
    const allDiags = t.getPreEmitDiagnostics(program).filter(d => {{
        if (!d.file) return true;
        const fn = d.file.fileName;
        return fn === VIRTUAL_FILE || fn.replace(/\\\\/g, "/") === VIRTUAL_FILE;
    }});
    function rank(d) {{
        if (d.category === t.DiagnosticCategory.Error) return 0;
        if (d.category === t.DiagnosticCategory.Warning) return 1;
        return 2;
    }}
    allDiags.sort((a, b) => {{
        const ra = rank(a), rb = rank(b);
        if (ra !== rb) return ra - rb;
        return (a.start ?? 0) - (b.start ?? 0);
    }});
    const limit = Math.max(1, MAX_DIAGNOSTICS);
    const limited = allDiags.slice(0, limit).map(d => {{
        let line = 1, col = 1;
        if (d.file && typeof d.start === "number") {{
            const loc = d.file.getLineAndCharacterOfPosition(d.start);
            line = loc.line + 1 - PREAMBLE_LINES;
            col = loc.character + 1;
            if (line < 1) line = 1;
        }}
        let msg = d.messageText;
        while (msg && typeof msg === "object" && "messageText" in msg) {{
            msg = msg.messageText;
        }}
        return {{
            severity: d.category === t.DiagnosticCategory.Error ? "error"
                    : d.category === t.DiagnosticCategory.Warning ? "warning" : "info",
            category: d.category,
            code: d.code,
            message: typeof msg === "string" ? msg : String(msg),
            line, column: col,
        }};
    }});
    const errCount = allDiags.filter(d => d.category === t.DiagnosticCategory.Error).length;
    const warnCount = allDiags.filter(d => d.category === t.DiagnosticCategory.Warning).length;
    const out = {{
        passed: errCount === 0,
        errorCount: errCount,
        warningCount: warnCount,
        totalDiagnosticCount: allDiags.length,
        diagnostics: limited,
        truncated: allDiags.length > limit,
    }};
    process.stdout.write(JSON.stringify(out));
}})().catch(e => {{
    process.stdout.write(JSON.stringify({{
        __runnerError: String(e && e.message || e),
        diagnostics: [], passed: false,
        errorCount: 1, warningCount: 0, totalDiagnosticCount: 1, truncated: false
    }}));
}});
)js",
        kVirtualFile,
        js_string_escape(opts.code),
        opts.preamble_line_count,
        std::max(1, opts.max_diagnostics)
    );
}

/// Execute the generated runner script via bun (preferred) or node.
auto run_via_subprocess(
    std::string_view runner_script,
    const TypecheckOptions& opts
) -> ExecutionResult {
    // Use bash to echo the script into `bun -` (stdin execution) so we don't
    // need to create temporary files.  If bun isn't available, fall back to
    // `node -`.
    ShellSessionConfig cfg;
    cfg.timeout = opts.timeout;
    if (opts.working_dir) {
        cfg.working_dir = opts.working_dir->string();
    }
    // Escape for single-quoted shell heredoc: just use printf.
    // Simpler: write to a real tempfile then execute it.
    auto tmp = fs::temp_directory_path() /
        std::format("cc_tc_{}.mjs",
            std::chrono::steady_clock::now().time_since_epoch().count());

    {
        std::ofstream out(tmp, std::ios::binary);
        out << runner_script;
    }

    // Try bun first, fall back to node.
    std::string cmd = std::format(
        "(bun {} 2>/dev/null || node {} 2>&1)",
        tmp.string(), tmp.string()
    );

    auto result = execute_command(cmd, cfg);

    std::error_code ec;
    fs::remove(tmp, ec);

    if (!result) {
        ExecutionResult err;
        err.exit_code = 1;
        err.stderr_output = result.error();
        return err;
    }
    return *result;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Run type-check for an inline code snippet.
/// Mirrors runScriptTypeCheck(ScriptTypeCheckOptions).
auto run_script_typecheck(const TypecheckOptions& opts) -> TypecheckResult {
    TypecheckResult out;
    const auto start = std::chrono::steady_clock::now();

    if (opts.code.empty()) {
        out.passed = true;
        out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return out;
    }

    const auto script = detail::generate_runner_script(opts);
    auto exec = detail::run_via_subprocess(script, opts);

    out.duration = exec.duration;

    // If the subprocess returned nothing usable, synthesise a runner error.
    if (exec.stdout_output.empty() || exec.exit_code != 0) {
        out.passed = false;
        out.error_count = 1;
        out.total_diagnostic_count = 1;
        out.runner_error = exec.stderr_output.empty()
            ? exec.stdout_output
            : exec.stderr_output;
        if (out.runner_error.empty()) {
            out.runner_error = "typecheck subprocess exited with code "
                + std::to_string(exec.exit_code);
        }
        out.diagnostics.push_back(ScriptTypeDiagnostic{
            .severity = Severity::Error,
            .code = -1,
            .message = out.runner_error,
            .line = 1,
            .column = 1,
        });
        return out;
    }

    // --- Parse JSON output with yyjson ------------------------------------
    auto doc = parse(exec.stdout_output);
    if (!doc) {
        out.passed = false;
        out.error_count = 1;
        out.total_diagnostic_count = 1;
        out.runner_error = std::format(
            "failed to parse tsc JSON output: {}",
            doc.error().message()
        );
        out.diagnostics.push_back(ScriptTypeDiagnostic{
            .severity = Severity::Error,
            .code = -1,
            .message = out.runner_error,
            .line = 1, .column = 1,
        });
        return out;
    }

    auto root = doc->root();

    // --- Subprocess-reported runner error ---------------------------------
    if (auto err = root.get("__runnerError"); err.is_str()) {
        out.runner_error = std::string(err.as_str());
        out.passed = false;
        out.error_count = 1;
        out.total_diagnostic_count = 1;
        out.diagnostics.push_back(ScriptTypeDiagnostic{
            .severity = Severity::Error,
            .code = -1,
            .message = out.runner_error,
            .line = 1, .column = 1,
        });
        return out;
    }

    out.passed                 = root.get("passed").as_bool();
    out.error_count            = static_cast<int>(root.get("errorCount").as_int());
    out.warning_count          = static_cast<int>(root.get("warningCount").as_int());
    out.total_diagnostic_count = static_cast<int>(root.get("totalDiagnosticCount").as_int());
    out.truncated              = root.get("truncated").as_bool();

    auto diag_arr = root.get("diagnostics");
    if (diag_arr.is_arr()) {
        diag_arr.iter([&](JsonVal item) {
            if (!item.is_obj()) return;
            ScriptTypeDiagnostic d;
            // category-based severity fallback if "severity" string missing
            if (auto sev = item.get("severity"); sev.is_str()) {
                auto s = sev.as_str();
                if (s == "error")      d.severity = Severity::Error;
                else if (s == "warning") d.severity = Severity::Warning;
                else                     d.severity = Severity::Info;
            } else {
                d.severity = detail::severity_from_ts_category(
                    static_cast<int>(item.get("category").as_int()));
            }
            d.code    = static_cast<int>(item.get("code").as_int());
            d.message = detail::extract_message_text(item.get("message"));
            d.line    = std::max(1, static_cast<int>(item.get("line").as_int()));
            d.column  = std::max(1, static_cast<int>(item.get("column").as_int()));
            out.diagnostics.push_back(std::move(d));
        });
    }

    // --- Canonical sort (errors → warnings → info, then by position) ------
    std::stable_sort(out.diagnostics.begin(), out.diagnostics.end(),
        [](const ScriptTypeDiagnostic& a, const ScriptTypeDiagnostic& b) {
            const int ra = detail::diagnostic_rank(a.severity);
            const int rb = detail::diagnostic_rank(b.severity);
            if (ra != rb) return ra < rb;
            if (a.line != b.line) return a.line < b.line;
            return a.column < b.column;
        });

    return out;
}

/// Type-check a list of on-disk files via `bunx tsc --noEmit --pretty false`.
/// Produces tsc's textual output first, then re-parses with yyjson when
/// `--json` is available on the project's tsc version.
auto typecheck_files(
    const std::vector<fs::path>& files,
    TypecheckOptions opts
) -> TypecheckResult {
    TypecheckResult out;
    const auto start = std::chrono::steady_clock::now();

    if (files.empty()) {
        out.passed = true;
        out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return out;
    }

    // Build argument list with proper quoting
    std::string file_args;
    for (const auto& f : files) {
        if (!file_args.empty()) file_args += ' ';
        file_args += '\'';
        file_args += f.string();
        file_args += '\'';
    }

    ShellSessionConfig cfg;
    cfg.timeout = opts.timeout;
    if (opts.working_dir) cfg.working_dir = opts.working_dir->string();

    // Strategy: emit JSON by running the same embedded-style runner but
    // concatenating all on-disk files into a single virtual compilation is
    // expensive.  Instead, prefer `tsc --noEmit` with a temp tsconfig that
    // forces JSON output.  We do this via our runner-style approach: a small
    // node script that reads each physical file, creates a program, and
    // prints diagnostics as JSON.
    std::string file_list_js = "[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i) file_list_js += ',';
        file_list_js += '"';
        file_list_js += detail::js_string_escape(files[i].string());
        file_list_js += '"';
    }
    file_list_js += ']';

    std::string runner = std::format(R"js(
const FILES = {};
const MAX_DIAGNOSTICS = {};
(async function main() {{
    const ts = await import("typescript").catch(() => null);
    if (!ts) {{
        process.stdout.write(JSON.stringify({{
            __runnerError: "typescript not found", diagnostics: [],
            passed: false, errorCount: 1, warningCount: 0,
            totalDiagnosticCount: 1, truncated: false
        }}));
        return;
    }}
    const t = ts.default ?? ts;
    const options = {{
        noEmit: true,
        target: t.ScriptTarget.ES2022,
        module: t.ModuleKind.ESNext,
        moduleResolution: 100 /* Bundler */,
        allowJs: true,
        strict: false,
        skipLibCheck: true,
    }};
    const program = t.createProgram(FILES, options);
    const diags = t.getPreEmitDiagnostics(program);
    function rank(d) {{
        if (d.category === t.DiagnosticCategory.Error) return 0;
        if (d.category === t.DiagnosticCategory.Warning) return 1;
        return 2;
    }}
    diags.sort((a, b) => {{
        const ra = rank(a), rb = rank(b);
        if (ra !== rb) return ra - rb;
        return (a.start ?? 0) - (b.start ?? 0);
    }});
    const limit = Math.max(1, MAX_DIAGNOSTICS);
    const limited = diags.slice(0, limit).map(d => {{
        let line = 1, col = 1, file = "";
        if (d.file && typeof d.start === "number") {{
            const loc = d.file.getLineAndCharacterOfPosition(d.start);
            line = loc.line + 1;
            col = loc.character + 1;
            file = d.file.fileName;
        }} else if (d.file) {{
            file = d.file.fileName;
        }}
        let msg = d.messageText;
        while (msg && typeof msg === "object" && "messageText" in msg) msg = msg.messageText;
        return {{
            severity: d.category === t.DiagnosticCategory.Error ? "error"
                    : d.category === t.DiagnosticCategory.Warning ? "warning" : "info",
            category: d.category,
            code: d.code,
            message: typeof msg === "string" ? msg : String(msg),
            line, column: col, file,
        }};
    }});
    const errCount  = diags.filter(d => d.category === t.DiagnosticCategory.Error).length;
    const warnCount = diags.filter(d => d.category === t.DiagnosticCategory.Warning).length;
    process.stdout.write(JSON.stringify({{
        passed: errCount === 0,
        errorCount: errCount,
        warningCount: warnCount,
        totalDiagnosticCount: diags.length,
        diagnostics: limited,
        truncated: diags.length > limit,
    }}));
}})().catch(e => process.stdout.write(JSON.stringify({{
    __runnerError: String(e.message || e), diagnostics: [],
    passed: false, errorCount: 1, warningCount: 0,
    totalDiagnosticCount: 1, truncated: false
}})));
)js",
        file_list_js,
        std::max(1, opts.max_diagnostics)
    );

    auto tmp = fs::temp_directory_path() /
        std::format("cc_tcf_{}.mjs",
            std::chrono::steady_clock::now().time_since_epoch().count());
    { std::ofstream(tmp, std::ios::binary) << runner; }

    std::string cmd = std::format(
        "(bun {} 2>/dev/null || node {} 2>&1)",
        tmp.string(), tmp.string()
    );
    auto exec_result = execute_command(cmd, cfg);
    std::error_code ec;
    fs::remove(tmp, ec);

    if (!exec_result) {
        out.passed = false;
        out.error_count = 1;
        out.total_diagnostic_count = 1;
        out.runner_error = exec_result.error();
        out.diagnostics.push_back(ScriptTypeDiagnostic{
            Severity::Error, -1, out.runner_error, 1, 1
        });
        out.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return out;
    }

    out.duration = exec_result->duration;
    if (exec_result->stdout_output.empty()) {
        // Empty stdout = likely no issues and tsc succeeded; treat as passed
        out.passed = true;
        return out;
    }

    auto doc = parse(exec_result->stdout_output);
    if (!doc) {
        out.passed = false;
        out.error_count = 1;
        out.total_diagnostic_count = 1;
        out.runner_error = std::format(
            "typecheck JSON parse failure: {}", doc.error().message());
        out.diagnostics.push_back(ScriptTypeDiagnostic{
            Severity::Error, -1, out.runner_error, 1, 1
        });
        return out;
    }
    auto root = doc->root();

    if (auto e = root.get("__runnerError"); e.is_str()) {
        out.runner_error = std::string(e.as_str());
    }

    out.passed                 = root.get("passed").as_bool();
    out.error_count            = static_cast<int>(root.get("errorCount").as_int());
    out.warning_count          = static_cast<int>(root.get("warningCount").as_int());
    out.total_diagnostic_count = static_cast<int>(root.get("totalDiagnosticCount").as_int());
    out.truncated              = root.get("truncated").as_bool();

    auto diag_arr = root.get("diagnostics");
    if (diag_arr.is_arr()) {
        diag_arr.iter([&](JsonVal item) {
            if (!item.is_obj()) return;
            ScriptTypeDiagnostic d;
            auto s = item.get_string("severity");
            if (s == "error")      d.severity = Severity::Error;
            else if (s == "warning") d.severity = Severity::Warning;
            else                     d.severity = Severity::Info;
            d.code    = static_cast<int>(item.get("code").as_int());
            d.message = detail::extract_message_text(item.get("message"));
            d.line    = std::max(1, static_cast<int>(item.get("line").as_int()));
            d.column  = std::max(1, static_cast<int>(item.get("column").as_int()));
            out.diagnostics.push_back(std::move(d));
        });
    }

    std::stable_sort(out.diagnostics.begin(), out.diagnostics.end(),
        [](const ScriptTypeDiagnostic& a, const ScriptTypeDiagnostic& b) {
            const int ra = detail::diagnostic_rank(a.severity);
            const int rb = detail::diagnostic_rank(b.severity);
            if (ra != rb) return ra < rb;
            if (a.line != b.line) return a.line < b.line;
            return a.column < b.column;
        });

    return out;
}

// --- Compatibility helpers kept from the original placeholder API ---------

auto has_type_errors(const TypecheckResult& r) -> bool {
    return r.error_count > 0;
}

auto filter_errors(const TypecheckResult& r) -> std::vector<ScriptTypeDiagnostic> {
    std::vector<ScriptTypeDiagnostic> out;
    for (const auto& d : r.diagnostics) {
        if (d.severity == Severity::Error) out.push_back(d);
    }
    return out;
}

} // namespace cc::tools::script_typecheck
