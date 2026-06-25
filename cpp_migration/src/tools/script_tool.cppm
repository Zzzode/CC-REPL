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
import cc.tools.script_types;
import cc.tools.tool_display_names;
import cc.utils.bash_execution;

export namespace cc::tools {


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


struct ScriptRequest {
    std::string code;
    ScriptLanguage language;
    SandboxLimits limits;
    bool enable_type_check{false};
    std::optional<std::string> stdin_data;
};


auto resolve_interpreter(ScriptLanguage lang) -> std::expected<std::string, ScriptError> {
    auto runner = get_script_runner(lang);
    if (runner) return runner->string();
    return std::unexpected(ScriptError::InterpreterNotFound);
}

constexpr auto script_extension(ScriptLanguage lang) -> std::string_view {
    switch (lang) {
        case ScriptLanguage::TypeScript: return ".ts";
        case ScriptLanguage::JavaScript: return ".js";
        case ScriptLanguage::Python:     return ".py";
        case ScriptLanguage::Shell:      return ".sh";
        case ScriptLanguage::Unknown:    return ".txt";
    }
    return ".txt";
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


        FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
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
        int status = cc::utils::bash::pclose_spawn(pipe);


        std::filesystem::remove(tmp_path);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);

        bool timed_out = elapsed > request.limits.timeout;

        ScriptResult result{
            .exit_code = status,
            .output = std::move(output),
            .errors = timed_out ? "Script exceeded configured timeout" : std::string{},
            .diagnostics = {},
            .duration = elapsed,
        };


        if (request.enable_type_check) {
            result.diagnostics = run_type_check(request.language, request.code);

            // Pretty-print diagnostics via unified format_diagnostics formatter.
            if (!result.diagnostics.empty()) {
                ::cc::tools::FormatDiagnosticOptions fopts;
                fopts.max_display    = 200;
                fopts.use_colors     = true;
                fopts.align_messages = true;
                fopts.show_snippets  = false;
                if (!result.errors.empty()) result.errors += "\n";
                result.errors += ::cc::tools::format_diagnostics(result.diagnostics, fopts);
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
        if (lang != ScriptLanguage::JavaScript && lang != ScriptLanguage::TypeScript) {
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
                case script_typecheck::Severity::Error:   local_diag.level = DiagnosticLevel::Error;   break;
                case script_typecheck::Severity::Warning: local_diag.level = DiagnosticLevel::Warning; break;
                case script_typecheck::Severity::Info:    local_diag.level = DiagnosticLevel::Info;    break;
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
