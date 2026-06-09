// ScriptTool - Sandboxed script execution with multi-language support
module;
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.script;

// migrated: integrate collapse decision + script primitives + typecheck
import cc.tools.script_primitives;
import cc.tools.script_typecheck;
import cc.tools.script_diagnostics;
import cc.tools.tool_display_names;

export namespace cc::tools {


enum class ScriptLanguage {
    Python,
    JavaScript,
    Shell,
};

constexpr auto language_name(ScriptLanguage lang) -> std::string_view {
    switch (lang) {
        case ScriptLanguage::Python:     return "python";
        case ScriptLanguage::JavaScript: return "javascript";
        case ScriptLanguage::Shell:      return "shell";
        default:                         return "unknown";
    }
}


enum class ScriptError {
    EmptyScript,
    UnsupportedLanguage,
    SandboxViolation,
    Timeout,
    ResourceLimitExceeded,
    SyntaxError,
    RuntimeError,
    TypeCheckFailed,
    InterpreterNotFound,
};

constexpr auto format_error(ScriptError err) -> std::string_view {
    switch (err) {
        case ScriptError::EmptyScript:          return "Script content is empty";
        case ScriptError::UnsupportedLanguage:  return "Unsupported script language";
        case ScriptError::SandboxViolation:     return "Script violated sandbox constraints";
        case ScriptError::Timeout:              return "Script execution timed out";
        case ScriptError::ResourceLimitExceeded: return "Script exceeded resource limits";
        case ScriptError::SyntaxError:          return "Script contains syntax errors";
        case ScriptError::RuntimeError:         return "Script runtime error";
        case ScriptError::TypeCheckFailed:      return "Type checking failed";
        case ScriptError::InterpreterNotFound:  return "Script interpreter not found";
        default:                                return "Unknown script error";
    }
}


struct SandboxLimits {
    size_t max_memory_mb{256};
    std::chrono::seconds timeout{30};
    size_t max_output_bytes{1024 * 512};
    bool allow_network{false};
    bool allow_filesystem{false};
    size_t max_processes{1};
};


struct Diagnostic {
    enum class Severity { Error, Warning, Info, Hint };

    Severity severity;
    size_t line;
    size_t column;
    std::string message;
    std::optional<std::string> source;
};


struct ScriptRequest {
    std::string code;
    ScriptLanguage language;
    SandboxLimits limits;
    bool enable_type_check{false};
    std::optional<std::string> stdin_data;
};


struct ScriptResult {
    int exit_code{0};
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration{0};
    std::vector<Diagnostic> diagnostics;
    bool timed_out{false};
    size_t memory_used_bytes{0};
};


auto resolve_interpreter(ScriptLanguage lang) -> std::expected<std::string, ScriptError> {
    switch (lang) {
        case ScriptLanguage::Python:     return std::string{"python3"};
        case ScriptLanguage::JavaScript: return std::string{"node"};
        case ScriptLanguage::Shell:      return std::string{"/bin/sh"};
        default: return std::unexpected(ScriptError::UnsupportedLanguage);
    }
}


constexpr auto script_extension(ScriptLanguage lang) -> std::string_view {
    switch (lang) {
        case ScriptLanguage::Python:     return ".py";
        case ScriptLanguage::JavaScript: return ".js";
        case ScriptLanguage::Shell:      return ".sh";
        default:                         return ".txt";
    }
}


class ScriptTool {
public:
    static constexpr std::string_view name = SCRIPT_TOOL_NAME;
    static constexpr std::string_view description = "Execute scripts in a sandboxed environment with resource limits";

    auto validate(const ScriptRequest& request) const -> std::expected<void, ScriptError> {
        if (request.code.empty()) {
            return std::unexpected(ScriptError::EmptyScript);
        }
        auto interp = resolve_interpreter(request.language);
        if (!interp) return std::unexpected(interp.error());
        return {};
    }

    auto execute(ScriptRequest request) -> std::expected<ScriptResult, ScriptError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto interpreter = resolve_interpreter(request.language);
        auto start_time = std::chrono::steady_clock::now();


        auto tmp_path = std::filesystem::temp_directory_path() /
            std::format("cc_script_{}{}", std::chrono::steady_clock::now().time_since_epoch().count(),
                        script_extension(request.language));

        {
            std::ofstream out(tmp_path);
            if (!out) return std::unexpected(ScriptError::RuntimeError);
            out << request.code;
        }


        auto cmd = build_sandboxed_command(*interpreter, tmp_path, request.limits);


        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) {
            std::filesystem::remove(tmp_path);
            return std::unexpected(ScriptError::InterpreterNotFound);
        }

        std::string output;
        std::array<char, 4096> buffer{};
        while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
            if (output.size() > request.limits.max_output_bytes) break;
        }
        int status = ::pclose(pipe);


        std::filesystem::remove(tmp_path);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        bool timed_out = elapsed > request.limits.timeout;

        ScriptResult result{
            .exit_code = status,
            .stdout_output = std::move(output),
            .duration = elapsed,
            .timed_out = timed_out,
        };


        if (request.enable_type_check) {
            result.diagnostics = run_type_check(request.language, request.code);

            // Bridge local Diagnostics (script_tool) into
            // script_diagnostics::Diagnostic, then pretty-print via the
            // unified format_diagnostics formatter.
            if (!result.diagnostics.empty()) {
                std::vector<::cc::tools::Diagnostic> formatted;
                formatted.reserve(result.diagnostics.size());
                for (const auto& d : result.diagnostics) {
                    ::cc::tools::Diagnostic out;
                    using Lvl = ::cc::tools::DiagnosticLevel;
                    switch (d.severity) {
                        case decltype(d)::Severity::Error:   out.level = Lvl::Error;   break;
                        case decltype(d)::Severity::Warning: out.level = Lvl::Warning; break;
                        case decltype(d)::Severity::Info:    out.level = Lvl::Info;    break;
                        case decltype(d)::Severity::Hint:    out.level = Lvl::Hint;    break;
                    }
                    out.line    = d.line;
                    out.column  = d.column;
                    out.message = d.message;
                    out.source  = d.source;
                    formatted.push_back(std::move(out));
                }
                ::cc::tools::FormatDiagnosticOptions fopts;
                fopts.max_display    = 200;
                fopts.use_colors     = true;
                fopts.align_messages = true;
                fopts.show_snippets  = false;
                if (!result.stderr_output.empty()) result.stderr_output += "\n";
                result.stderr_output += ::cc::tools::format_diagnostics(formatted, fopts);
            }
        }

        return result;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "code": {{ "type": "string", "description": "Script source code to execute" }},
      "language": {{ "type": "string", "enum": ["python", "javascript", "shell"], "description": "Script language" }},
      "timeout": {{ "type": "integer", "description": "Execution timeout in seconds (default 30)" }},
      "enable_type_check": {{ "type": "boolean", "description": "Enable static type checking" }}
    }},
    "required": ["code", "language"]
  }}
}})json", name, description);
    }

private:

    auto build_sandboxed_command(std::string_view interpreter,
                                 const std::filesystem::path& script_path,
                                 const SandboxLimits& limits) const -> std::string {

        return std::format(
            "ulimit -v {} -t {} 2>/dev/null; {} {} 2>&1",
            limits.max_memory_mb * 1024,       // KB for ulimit
            limits.timeout.count(),
            interpreter,
            script_path.string()
        );
    }


    auto run_type_check(ScriptLanguage lang, std::string_view code) const
        -> std::vector<Diagnostic>
    {
        std::vector<Diagnostic> diags;

        // migrated: integrate script typecheck via cc.tools.script_typecheck
        if (lang != ScriptLanguage::JavaScript) {
            // Only JS/TS runs type-checking; Python/Shell keep the empty list.
            // TypeScript subset is treated as JS for this placeholder mapping.
            // Add a guard so that non-TS code never reaches the heavy runner.
            return diags;
        }

        using namespace cc::tools::script_typecheck;
        TypecheckOptions opts;
        opts.code                = std::string(code);
        opts.preamble_line_count = 0;
        opts.max_diagnostics     = 100;
        opts.timeout             = std::chrono::seconds{30};

        TypecheckResult result = run_script_typecheck(opts);

        diags.reserve(result.diagnostics.size());
        for (const auto& d : result.diagnostics) {
            Diagnostic local_diag;
            switch (d.severity) {
                case Severity::Error:   local_diag.severity = Diagnostic::Severity::Error;   break;
                case Severity::Warning: local_diag.severity = Diagnostic::Severity::Warning; break;
                case Severity::Info:    local_diag.severity = Diagnostic::Severity::Info;    break;
            }
            local_diag.line    = static_cast<size_t>(std::max(1, d.line));
            local_diag.column  = static_cast<size_t>(std::max(1, d.column));
            local_diag.message = std::format("TS{}: {}", d.code, d.message);
            local_diag.source  = "script_typecheck";
            diags.push_back(std::move(local_diag));
        }

        return diags;
    }
};

} // namespace cc::tools
