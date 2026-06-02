// C++23 PowerShell Parser Module
// Provides PowerShell command parsing, dangerous cmdlet detection,
// and static prefix extraction for permission management.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <chrono>
#include <cstdint>

export module cc.utils.powershell_parser;

export namespace cc::utils::powershell_parser {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// The PowerShell AST element type for pipeline elements.
/// Maps to CommandBaseAst derivatives in System.Management.Automation.Language.
enum class PipelineElementType {
    CommandAst,
    CommandExpressionAst,
    ParenExpressionAst
};

/// The AST node type for individual command elements (arguments, expressions).
/// Used to classify each element during the AST walk.
enum class CommandElementType {
    ScriptBlock,
    SubExpression,
    ExpandableString,
    MemberInvocation,
    Variable,
    StringConstant,
    Parameter,
    Other
};

/// The PowerShell AST statement type.
/// Maps to StatementAst derivatives in System.Management.Automation.Language.
enum class StatementType {
    PipelineAst,
    PipelineChainAst,
    AssignmentStatementAst,
    IfStatementAst,
    ForStatementAst,
    ForEachStatementAst,
    WhileStatementAst,
    DoWhileStatementAst,
    DoUntilStatementAst,
    SwitchStatementAst,
    TryStatementAst,
    TrapStatementAst,
    FunctionDefinitionAst,
    DataStatementAst,
    UnknownStatementAst
};

/// Classification of a PowerShell command name.
enum class CommandNameType {
    Cmdlet,       // Verb-Noun pattern
    Application,  // File path (contains ./ or \)
    Unknown       // Bare name
};

/// Redirection operator type.
enum class RedirectionOperator {
    Redirect,         // >
    AppendRedirect,   // >>
    ErrorRedirect,    // 2>
    ErrorAppend,      // 2>>
    AllRedirect,      // *>
    AllAppend,        // *>>
    MergeError        // 2>&1
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

/// A child node of a command element (one level deep).
/// Populated for CommandParameterAst -> .Argument (colon-bound parameters).
struct CommandElementChild {
    CommandElementType type;
    std::string text;
};

/// A redirection found in the command.
struct ParsedRedirection {
    RedirectionOperator op;
    std::string target;
    bool is_merging = false;
};

/// A command invocation within a pipeline segment.
struct ParsedCommandElement {
    std::string name;
    CommandNameType name_type = CommandNameType::Unknown;
    PipelineElementType element_type = PipelineElementType::CommandAst;
    std::vector<std::string> args;
    std::string text;
    std::vector<CommandElementType> element_types;
    std::vector<std::optional<std::vector<CommandElementChild>>> children;
    std::vector<ParsedRedirection> redirections;
};

/// Security-relevant AST patterns found via FindAll on a statement.
struct SecurityPatterns {
    bool has_member_invocations = false;
    bool has_sub_expressions = false;
    bool has_expandable_strings = false;
    bool has_script_blocks = false;
};

/// A parsed statement from PowerShell.
struct ParsedStatement {
    StatementType statement_type = StatementType::UnknownStatementAst;
    std::vector<ParsedCommandElement> commands;
    std::vector<ParsedRedirection> redirections;
    std::string text;
    std::vector<ParsedCommandElement> nested_commands;
    std::optional<SecurityPatterns> security_patterns;
};

/// A variable reference found in the command.
struct ParsedVariable {
    std::string path;
    bool is_splatted = false;
};

/// A parse error from PowerShell's parser.
struct ParseError {
    std::string message;
    std::string error_id;
};

/// The complete parsed result from the PowerShell AST parser.
struct ParsedPowerShellCommand {
    bool valid = false;
    std::vector<ParseError> errors;
    std::vector<ParsedStatement> statements;
    std::vector<ParsedVariable> variables;
    bool has_stop_parsing = false;
    std::string original_command;
    std::vector<std::string> type_literals;
    bool has_using_statements = false;
    bool has_script_requirements = false;
};

/// Security-relevant flags derived from the parsed command structure.
struct SecurityFlags {
    bool has_sub_expressions = false;
    bool has_script_blocks = false;
    bool has_splatting = false;
    bool has_expandable_strings = false;
    bool has_member_invocations = false;
    bool has_assignments = false;
    bool has_stop_parsing = false;
};

/// Result of a prefix extraction attempt.
struct PrefixResult {
    std::optional<std::string> command_prefix;
};

// ---------------------------------------------------------------------------
// Constants — Dangerous Cmdlet Sets
// ---------------------------------------------------------------------------

/// Cmdlets that accept a -FilePath (or positional path) and execute file contents.
[[nodiscard]] inline const std::unordered_set<std::string>& filepath_execution_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "invoke-command",
        "start-job",
        "start-threadjob",
        "register-scheduledjob",
    };
    return s;
}

/// Cmdlets where a scriptblock argument executes arbitrary code.
[[nodiscard]] inline const std::unordered_set<std::string>& dangerous_script_block_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "invoke-command",
        "invoke-expression",
        "start-job",
        "start-threadjob",
        "register-scheduledjob",
        "register-engineevent",
        "register-objectevent",
        "register-wmievent",
        "new-pssession",
        "enter-pssession",
    };
    return s;
}

/// Cmdlets that load and execute module/script code.
[[nodiscard]] inline const std::unordered_set<std::string>& module_loading_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "import-module",
        "ipmo",
        "install-module",
        "save-module",
        "update-module",
        "install-script",
        "save-script",
    };
    return s;
}

/// Network cmdlets that enable exfiltration/download.
[[nodiscard]] inline const std::unordered_set<std::string>& network_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "invoke-webrequest",
        "invoke-restmethod",
    };
    return s;
}

/// Alias/variable mutation cmdlets.
[[nodiscard]] inline const std::unordered_set<std::string>& alias_hijack_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "set-alias",
        "sal",
        "new-alias",
        "nal",
        "set-variable",
        "sv",
        "new-variable",
        "nv",
    };
    return s;
}

/// WMI/CIM process spawn cmdlets.
[[nodiscard]] inline const std::unordered_set<std::string>& wmi_cim_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "invoke-wmimethod",
        "iwmi",
        "invoke-cimmethod",
    };
    return s;
}

/// Cmdlets with additionalCommandIsDangerousCallback in the allowlist.
[[nodiscard]] inline const std::unordered_set<std::string>& arg_gated_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "select-object",
        "sort-object",
        "group-object",
        "where-object",
        "measure-object",
        "write-output",
        "write-host",
        "start-sleep",
        "format-table",
        "format-list",
        "format-wide",
        "format-custom",
        "out-string",
        "out-host",
        "ipconfig",
        "hostname",
        "route",
    };
    return s;
}

/// Shells and process spawners.
[[nodiscard]] inline const std::vector<std::string>& shells_and_spawners() {
    static const std::vector<std::string> v = {
        "pwsh",
        "powershell",
        "cmd",
        "bash",
        "wsl",
        "sh",
        "start-process",
        "start",
        "add-type",
        "new-object",
    };
    return v;
}

/// Cross-platform code execution interpreters.
[[nodiscard]] inline const std::vector<std::string>& cross_platform_code_exec() {
    static const std::vector<std::string> v = {
        "node",
        "python",
        "python3",
        "ruby",
        "perl",
        "php",
        "lua",
        "java",
        "javac",
        "deno",
        "bun",
    };
    return v;
}

/// Commands to never suggest as a wildcard prefix in the permission dialog.
/// Derived from all the validator lists above plus shells and code executors.
[[nodiscard]] inline const std::unordered_set<std::string>& never_suggest() {
    static const std::unordered_set<std::string> s = [] {
        std::unordered_set<std::string> result;
        // Shells and spawners
        for (const auto& cmd : shells_and_spawners()) {
            result.insert(cmd);
        }
        // Filepath execution
        for (const auto& cmd : filepath_execution_cmdlets()) {
            result.insert(cmd);
        }
        // Dangerous script blocks
        for (const auto& cmd : dangerous_script_block_cmdlets()) {
            result.insert(cmd);
        }
        // Module loading
        for (const auto& cmd : module_loading_cmdlets()) {
            result.insert(cmd);
        }
        // Network cmdlets
        for (const auto& cmd : network_cmdlets()) {
            result.insert(cmd);
        }
        // Alias hijack
        for (const auto& cmd : alias_hijack_cmdlets()) {
            result.insert(cmd);
        }
        // WMI/CIM
        for (const auto& cmd : wmi_cim_cmdlets()) {
            result.insert(cmd);
        }
        // Arg-gated
        for (const auto& cmd : arg_gated_cmdlets()) {
            result.insert(cmd);
        }
        // ForEach-Object (-MemberName resolves against pipeline objects)
        result.insert("foreach-object");
        // Cross-platform code executors (single-word only)
        for (const auto& cmd : cross_platform_code_exec()) {
            if (cmd.find(' ') == std::string::npos) {
                result.insert(cmd);
            }
        }
        // Include aliases of all the above
        // (aliases that resolve to any cmdlet in the core set)
        return result;
    }();
    return s;
}

// ---------------------------------------------------------------------------
// Constants — Common Aliases
// ---------------------------------------------------------------------------

/// Common PowerShell aliases mapped to their canonical cmdlet names.
[[nodiscard]] inline const std::unordered_map<std::string, std::string>& common_aliases() {
    static const std::unordered_map<std::string, std::string> m = {
        // Directory listing
        {"ls", "Get-ChildItem"},
        {"dir", "Get-ChildItem"},
        {"gci", "Get-ChildItem"},
        // Content
        {"cat", "Get-Content"},
        {"type", "Get-Content"},
        {"gc", "Get-Content"},
        // Navigation
        {"cd", "Set-Location"},
        {"sl", "Set-Location"},
        {"chdir", "Set-Location"},
        {"pushd", "Push-Location"},
        {"popd", "Pop-Location"},
        {"pwd", "Get-Location"},
        {"gl", "Get-Location"},
        // Items
        {"gi", "Get-Item"},
        {"gp", "Get-ItemProperty"},
        {"ni", "New-Item"},
        {"mkdir", "New-Item"},
        {"md", "New-Item"},
        {"ri", "Remove-Item"},
        {"del", "Remove-Item"},
        {"rd", "Remove-Item"},
        {"rmdir", "Remove-Item"},
        {"rm", "Remove-Item"},
        {"erase", "Remove-Item"},
        {"mi", "Move-Item"},
        {"mv", "Move-Item"},
        {"move", "Move-Item"},
        {"ci", "Copy-Item"},
        {"cp", "Copy-Item"},
        {"copy", "Copy-Item"},
        {"cpi", "Copy-Item"},
        {"si", "Set-Item"},
        {"rni", "Rename-Item"},
        {"ren", "Rename-Item"},
        // Process
        {"ps", "Get-Process"},
        {"gps", "Get-Process"},
        {"kill", "Stop-Process"},
        {"spps", "Stop-Process"},
        {"start", "Start-Process"},
        {"saps", "Start-Process"},
        {"sajb", "Start-Job"},
        {"ipmo", "Import-Module"},
        // Output
        {"echo", "Write-Output"},
        {"write", "Write-Output"},
        {"sleep", "Start-Sleep"},
        // Help
        {"help", "Get-Help"},
        {"man", "Get-Help"},
        {"gcm", "Get-Command"},
        // Service
        {"gsv", "Get-Service"},
        // Variables
        {"gv", "Get-Variable"},
        {"sv", "Set-Variable"},
        // History
        {"h", "Get-History"},
        {"history", "Get-History"},
        // Invoke
        {"iex", "Invoke-Expression"},
        {"iwr", "Invoke-WebRequest"},
        {"irm", "Invoke-RestMethod"},
        {"icm", "Invoke-Command"},
        {"ii", "Invoke-Item"},
        // PSSession
        {"nsn", "New-PSSession"},
        {"etsn", "Enter-PSSession"},
        {"exsn", "Exit-PSSession"},
        {"gsn", "Get-PSSession"},
        {"rsn", "Remove-PSSession"},
        // Misc
        {"cls", "Clear-Host"},
        {"clear", "Clear-Host"},
        {"select", "Select-Object"},
        {"where", "Where-Object"},
        {"foreach", "ForEach-Object"},
        {"%", "ForEach-Object"},
        {"?", "Where-Object"},
        {"measure", "Measure-Object"},
        {"ft", "Format-Table"},
        {"fl", "Format-List"},
        {"fw", "Format-Wide"},
        {"oh", "Out-Host"},
        {"ogv", "Out-GridView"},
        {"ac", "Add-Content"},
        {"clc", "Clear-Content"},
        {"tee", "Tee-Object"},
        {"epcsv", "Export-Csv"},
        {"sp", "Set-ItemProperty"},
        {"rp", "Remove-ItemProperty"},
        {"cli", "Clear-Item"},
        {"epal", "Export-Alias"},
        {"sls", "Select-String"},
    };
    return m;
}

/// PowerShell tokenizer-level dash characters accepted as parameter prefixes.
[[nodiscard]] inline const std::unordered_set<char32_t>& ps_tokenizer_dash_chars() {
    static const std::unordered_set<char32_t> s = {
        U'-',      // U+002D hyphen-minus (ASCII)
        U'\u2013', // en-dash
        U'\u2014', // em-dash
        U'\u2015', // horizontal bar
    };
    return s;
}

/// Directory-changing cmdlets.
[[nodiscard]] inline const std::unordered_set<std::string>& directory_change_cmdlets() {
    static const std::unordered_set<std::string> s = {
        "set-location",
        "push-location",
        "pop-location",
    };
    return s;
}

/// Directory-changing aliases.
[[nodiscard]] inline const std::unordered_set<std::string>& directory_change_aliases() {
    static const std::unordered_set<std::string> s = {
        "cd", "sl", "chdir", "pushd", "popd",
    };
    return s;
}

// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

/// Convert a string to lowercase (ASCII).
[[nodiscard]] inline std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

/// Map raw .NET AST type name to StatementType.
[[nodiscard]] inline StatementType map_statement_type(std::string_view raw_type) {
    if (raw_type == "PipelineAst") return StatementType::PipelineAst;
    if (raw_type == "PipelineChainAst") return StatementType::PipelineChainAst;
    if (raw_type == "AssignmentStatementAst") return StatementType::AssignmentStatementAst;
    if (raw_type == "IfStatementAst") return StatementType::IfStatementAst;
    if (raw_type == "ForStatementAst") return StatementType::ForStatementAst;
    if (raw_type == "ForEachStatementAst") return StatementType::ForEachStatementAst;
    if (raw_type == "WhileStatementAst") return StatementType::WhileStatementAst;
    if (raw_type == "DoWhileStatementAst") return StatementType::DoWhileStatementAst;
    if (raw_type == "DoUntilStatementAst") return StatementType::DoUntilStatementAst;
    if (raw_type == "SwitchStatementAst") return StatementType::SwitchStatementAst;
    if (raw_type == "TryStatementAst") return StatementType::TryStatementAst;
    if (raw_type == "TrapStatementAst") return StatementType::TrapStatementAst;
    if (raw_type == "FunctionDefinitionAst") return StatementType::FunctionDefinitionAst;
    if (raw_type == "DataStatementAst") return StatementType::DataStatementAst;
    return StatementType::UnknownStatementAst;
}

/// Map raw .NET AST type name to CommandElementType.
[[nodiscard]] inline CommandElementType map_element_type(
    std::string_view raw_type,
    std::string_view expression_type = ""
) {
    if (raw_type == "ScriptBlockExpressionAst") return CommandElementType::ScriptBlock;
    if (raw_type == "SubExpressionAst" || raw_type == "ArrayExpressionAst") {
        return CommandElementType::SubExpression;
    }
    if (raw_type == "ExpandableStringExpressionAst") return CommandElementType::ExpandableString;
    if (raw_type == "InvokeMemberExpressionAst" || raw_type == "MemberExpressionAst") {
        return CommandElementType::MemberInvocation;
    }
    if (raw_type == "VariableExpressionAst") return CommandElementType::Variable;
    if (raw_type == "StringConstantExpressionAst" || raw_type == "ConstantExpressionAst") {
        return CommandElementType::StringConstant;
    }
    if (raw_type == "CommandParameterAst") return CommandElementType::Parameter;
    if (raw_type == "ParenExpressionAst") return CommandElementType::SubExpression;
    if (raw_type == "CommandExpressionAst" && !expression_type.empty()) {
        return map_element_type(expression_type);
    }
    return CommandElementType::Other;
}

/// Classify command name as cmdlet, application, or unknown.
[[nodiscard]] inline CommandNameType classify_command_name(std::string_view name) {
    // Cmdlet pattern: Verb-Noun (e.g. Get-ChildItem)
    bool found_dash = false;
    bool valid_cmdlet = !name.empty();
    for (std::size_t i = 0; i < name.size() && valid_cmdlet; ++i) {
        char c = name[i];
        if (c == '-') {
            if (found_dash || i == 0) { valid_cmdlet = false; }
            else { found_dash = true; }
        } else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            valid_cmdlet = false;
        }
    }
    if (valid_cmdlet && found_dash && name.back() != '-') {
        // Ensure there's at least one char after the dash
        auto dash_pos = name.find('-');
        if (dash_pos != std::string_view::npos && dash_pos + 1 < name.size() &&
            std::isalpha(static_cast<unsigned char>(name[dash_pos + 1]))) {
            return CommandNameType::Cmdlet;
        }
    }

    // Application: contains path separators
    if (name.find('.') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        name.find('/') != std::string_view::npos) {
        return CommandNameType::Application;
    }

    return CommandNameType::Unknown;
}

/// Strip module prefix from command name.
/// e.g. "Microsoft.PowerShell.Utility\\Invoke-Expression" -> "Invoke-Expression"
[[nodiscard]] inline std::string strip_module_prefix(std::string_view name) {
    auto idx = name.rfind('\\');
    if (idx == std::string_view::npos) return std::string(name);
    // Don't strip file paths
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
        return std::string(name); // Drive letter path
    }
    if (name.starts_with("\\\\") || name.starts_with(".\\") || name.starts_with("..\\")) {
        return std::string(name); // UNC or relative path
    }
    return std::string(name.substr(idx + 1));
}

// ---------------------------------------------------------------------------
// Parse function (async — spawns pwsh)
// ---------------------------------------------------------------------------

/// Parse a PowerShell command using the native AST parser.
/// Spawns pwsh to parse the command and returns structured results.
/// Returns error string if PowerShell is unavailable or spawn fails.
[[nodiscard]] inline std::expected<ParsedPowerShellCommand, std::string>
parse_powershell_command(std::string_view command) {
    // Placeholder: actual implementation spawns pwsh and parses JSON output.
    // In C++ this would use process spawning + JSON parsing.
    ParsedPowerShellCommand result;
    result.valid = false;
    result.original_command = std::string(command);
    result.errors.push_back({"Not implemented - requires pwsh spawn", "NotImplemented"});
    return result;
}

// ---------------------------------------------------------------------------
// Analysis helpers — operate on ParsedPowerShellCommand
// ---------------------------------------------------------------------------

/// Get all command names across all statements (lowercased).
[[nodiscard]] inline std::vector<std::string>
get_all_command_names(const ParsedPowerShellCommand& parsed) {
    std::vector<std::string> names;
    for (const auto& stmt : parsed.statements) {
        for (const auto& cmd : stmt.commands) {
            names.push_back(to_lower(cmd.name));
        }
        for (const auto& cmd : stmt.nested_commands) {
            names.push_back(to_lower(cmd.name));
        }
    }
    return names;
}

/// Get all pipeline segments as a flat list of commands.
[[nodiscard]] inline std::vector<ParsedCommandElement>
get_all_commands(const ParsedPowerShellCommand& parsed) {
    std::vector<ParsedCommandElement> commands;
    for (const auto& stmt : parsed.statements) {
        for (const auto& cmd : stmt.commands) {
            commands.push_back(cmd);
        }
        for (const auto& cmd : stmt.nested_commands) {
            commands.push_back(cmd);
        }
    }
    return commands;
}

/// Get all redirections across all statements.
[[nodiscard]] inline std::vector<ParsedRedirection>
get_all_redirections(const ParsedPowerShellCommand& parsed) {
    std::vector<ParsedRedirection> redirections;
    for (const auto& stmt : parsed.statements) {
        for (const auto& redir : stmt.redirections) {
            redirections.push_back(redir);
        }
        for (const auto& cmd : stmt.nested_commands) {
            for (const auto& redir : cmd.redirections) {
                redirections.push_back(redir);
            }
        }
    }
    return redirections;
}

/// Get variables filtered by scope (e.g., "env").
[[nodiscard]] inline std::vector<ParsedVariable>
get_variables_by_scope(const ParsedPowerShellCommand& parsed, std::string_view scope) {
    std::string prefix = to_lower(scope) + ":";
    std::vector<ParsedVariable> result;
    for (const auto& v : parsed.variables) {
        std::string lower_path = to_lower(v.path);
        if (lower_path.starts_with(prefix)) {
            result.push_back(v);
        }
    }
    return result;
}

/// Check if any command in the parsed result matches a given name (case-insensitive).
/// Handles common aliases too.
[[nodiscard]] inline bool has_command_named(
    const ParsedPowerShellCommand& parsed,
    std::string_view name
) {
    std::string lower_name = to_lower(name);
    const auto& aliases = common_aliases();

    std::string canonical_from_alias;
    auto it = aliases.find(lower_name);
    if (it != aliases.end()) {
        canonical_from_alias = to_lower(it->second);
    }

    for (const auto& cmd_name : get_all_command_names(parsed)) {
        if (cmd_name == lower_name) return true;

        auto alias_it = aliases.find(cmd_name);
        std::string canonical;
        if (alias_it != aliases.end()) {
            canonical = to_lower(alias_it->second);
        }

        if (canonical == lower_name) return true;
        if (!canonical_from_alias.empty() && cmd_name == canonical_from_alias) return true;
        if (!canonical.empty() && !canonical_from_alias.empty() &&
            canonical == canonical_from_alias) return true;
    }
    return false;
}

/// Check if the command contains any directory-changing commands.
[[nodiscard]] inline bool has_directory_change(const ParsedPowerShellCommand& parsed) {
    for (const auto& cmd_name : get_all_command_names(parsed)) {
        if (directory_change_cmdlets().contains(cmd_name) ||
            directory_change_aliases().contains(cmd_name)) {
            return true;
        }
    }
    return false;
}

/// Check if the command is a single simple command (no pipes, semicolons, operators).
[[nodiscard]] inline bool is_single_command(const ParsedPowerShellCommand& parsed) {
    if (parsed.statements.size() != 1) return false;
    const auto& stmt = parsed.statements[0];
    return stmt.commands.size() == 1 && stmt.nested_commands.empty();
}

/// Check if a specific command has a given argument/flag (case-insensitive).
[[nodiscard]] inline bool command_has_arg(
    const ParsedCommandElement& command,
    std::string_view arg
) {
    std::string lower_arg = to_lower(arg);
    for (const auto& a : command.args) {
        if (to_lower(a) == lower_arg) return true;
    }
    return false;
}

/// Check if any argument is an unambiguous abbreviation of a PowerShell parameter.
[[nodiscard]] inline bool command_has_arg_abbreviation(
    const ParsedCommandElement& command,
    std::string_view full_param,
    std::string_view min_prefix
) {
    std::string lower_full = to_lower(full_param);
    std::string lower_min = to_lower(min_prefix);
    for (const auto& a : command.args) {
        // Strip colon-bound value
        std::string param_part;
        auto colon_idx = a.find(':', 1);
        if (colon_idx != std::string::npos) {
            param_part = a.substr(0, colon_idx);
        } else {
            param_part = a;
        }
        // Strip backtick escapes
        std::string clean;
        clean.reserve(param_part.size());
        for (char c : param_part) {
            if (c != '`') clean += c;
        }
        std::string lower = to_lower(clean);
        if (lower.starts_with(lower_min) &&
            lower_full.starts_with(lower) &&
            lower.size() <= lower_full.size()) {
            return true;
        }
    }
    return false;
}

/// Determines if an argument is a PowerShell parameter.
[[nodiscard]] inline bool is_powershell_parameter(
    std::string_view arg,
    std::optional<CommandElementType> element_type = std::nullopt
) {
    if (element_type.has_value()) {
        return element_type.value() == CommandElementType::Parameter;
    }
    if (arg.empty()) return false;
    // Check first character against dash chars (ASCII dash only in simplified version)
    return arg[0] == '-';
}

/// Split a parsed command into its pipeline segments.
[[nodiscard]] inline const std::vector<ParsedStatement>&
get_pipeline_segments(const ParsedPowerShellCommand& parsed) {
    return parsed.statements;
}

/// True if a redirection target is PowerShell's $null automatic variable.
[[nodiscard]] inline bool is_null_redirection_target(std::string_view target) {
    // Trim whitespace
    std::string trimmed;
    for (char c : target) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            trimmed += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return trimmed == "$null" || trimmed == "${null}";
}

/// Get output redirections (file redirections, not merging).
[[nodiscard]] inline std::vector<ParsedRedirection>
get_file_redirections(const ParsedPowerShellCommand& parsed) {
    std::vector<ParsedRedirection> result;
    for (const auto& redir : get_all_redirections(parsed)) {
        if (!redir.is_merging && !is_null_redirection_target(redir.target)) {
            result.push_back(redir);
        }
    }
    return result;
}

/// Derive security-relevant flags from the parsed command structure.
[[nodiscard]] inline SecurityFlags derive_security_flags(const ParsedPowerShellCommand& parsed) {
    SecurityFlags flags{};
    flags.has_stop_parsing = parsed.has_stop_parsing;

    auto check_elements = [&](const ParsedCommandElement& cmd) {
        for (const auto& et : cmd.element_types) {
            switch (et) {
                case CommandElementType::ScriptBlock:
                    flags.has_script_blocks = true;
                    break;
                case CommandElementType::SubExpression:
                    flags.has_sub_expressions = true;
                    break;
                case CommandElementType::ExpandableString:
                    flags.has_expandable_strings = true;
                    break;
                case CommandElementType::MemberInvocation:
                    flags.has_member_invocations = true;
                    break;
                default:
                    break;
            }
        }
    };

    for (const auto& stmt : parsed.statements) {
        if (stmt.statement_type == StatementType::AssignmentStatementAst) {
            flags.has_assignments = true;
        }
        for (const auto& cmd : stmt.commands) {
            check_elements(cmd);
        }
        for (const auto& cmd : stmt.nested_commands) {
            check_elements(cmd);
        }
        if (stmt.security_patterns.has_value()) {
            const auto& sp = stmt.security_patterns.value();
            if (sp.has_member_invocations) flags.has_member_invocations = true;
            if (sp.has_sub_expressions) flags.has_sub_expressions = true;
            if (sp.has_expandable_strings) flags.has_expandable_strings = true;
            if (sp.has_script_blocks) flags.has_script_blocks = true;
        }
    }

    for (const auto& v : parsed.variables) {
        if (v.is_splatted) {
            flags.has_splatting = true;
            break;
        }
    }

    return flags;
}

/// Check if a cmdlet name appears in the NEVER_SUGGEST set (case-insensitive).
[[nodiscard]] inline bool is_never_suggest(std::string_view name) {
    return never_suggest().contains(to_lower(name));
}

/// Resolve an alias to its canonical cmdlet name.
/// Returns the canonical name if found, or the original name otherwise.
[[nodiscard]] inline std::string resolve_alias(std::string_view name) {
    std::string lower = to_lower(name);
    auto it = common_aliases().find(lower);
    if (it != common_aliases().end()) {
        return it->second;
    }
    return std::string(name);
}

// ---------------------------------------------------------------------------
// Static Prefix Extraction
// ---------------------------------------------------------------------------

/// Extract a static prefix from a single parsed command element.
/// Returns nullopt for commands we won't suggest.
[[nodiscard]] inline std::optional<std::string>
extract_prefix_from_element(const ParsedCommandElement& cmd) {
    // Application name type means a file path — don't suggest
    if (cmd.name_type == CommandNameType::Application) return std::nullopt;
    if (cmd.name.empty()) return std::nullopt;

    std::string lower_name = to_lower(cmd.name);
    if (never_suggest().contains(lower_name)) return std::nullopt;

    // Cmdlets: the name alone is the right prefix granularity
    if (cmd.name_type == CommandNameType::Cmdlet) {
        return cmd.name;
    }

    // External command: for now return the bare name
    // (full implementation would consult fig specs for subcommand depth)
    if (!cmd.element_types.empty() && cmd.element_types[0] != CommandElementType::StringConstant) {
        return std::nullopt;
    }

    // Check all args are StringConstant or Parameter
    for (std::size_t i = 0; i < cmd.args.size(); ++i) {
        if (i + 1 < cmd.element_types.size()) {
            auto t = cmd.element_types[i + 1];
            if (t != CommandElementType::StringConstant && t != CommandElementType::Parameter) {
                return std::nullopt;
            }
        }
    }

    // For now, return the command name as prefix
    // (full implementation would do spec-based subcommand walking)
    return cmd.name;
}

/// Extract a prefix suggestion for a PowerShell command.
/// Returns nullopt on parse failure; PrefixResult with optional prefix on success.
[[nodiscard]] inline std::optional<PrefixResult>
get_command_prefix_static(const ParsedPowerShellCommand& parsed) {
    if (!parsed.valid) return std::nullopt;

    auto commands = get_all_commands(parsed);
    // Find first actual CommandAst
    for (const auto& cmd : commands) {
        if (cmd.element_type == PipelineElementType::CommandAst) {
            auto prefix = extract_prefix_from_element(cmd);
            return PrefixResult{prefix};
        }
    }
    return PrefixResult{std::nullopt};
}

/// Word-aligned longest common prefix (case-insensitive).
/// Emits first string's casing.
[[nodiscard]] inline std::string word_aligned_lcp(const std::vector<std::string>& strings) {
    if (strings.empty()) return "";
    if (strings.size() == 1) return strings[0];

    // Split first string into words
    std::vector<std::string> first_words;
    {
        std::string word;
        for (char c : strings[0]) {
            if (c == ' ') {
                if (!word.empty()) { first_words.push_back(word); word.clear(); }
            } else {
                word += c;
            }
        }
        if (!word.empty()) first_words.push_back(word);
    }

    std::size_t common_count = first_words.size();

    for (std::size_t i = 1; i < strings.size(); ++i) {
        std::vector<std::string> words;
        std::string word;
        for (char c : strings[i]) {
            if (c == ' ') {
                if (!word.empty()) { words.push_back(word); word.clear(); }
            } else {
                word += c;
            }
        }
        if (!word.empty()) words.push_back(word);

        std::size_t match_count = 0;
        while (match_count < common_count && match_count < words.size() &&
               to_lower(words[match_count]) == to_lower(first_words[match_count])) {
            ++match_count;
        }
        common_count = match_count;
        if (common_count == 0) break;
    }

    std::string result;
    for (std::size_t i = 0; i < common_count; ++i) {
        if (i > 0) result += ' ';
        result += first_words[i];
    }
    return result;
}

/// Extract prefixes for all subcommands in a compound PowerShell command.
/// Returns per-subcommand prefixes, collapsed by word-aligned LCP for same-root groups.
[[nodiscard]] inline std::vector<std::string> get_compound_command_prefixes_static(
    const ParsedPowerShellCommand& parsed,
    std::function<bool(const ParsedCommandElement&)> exclude_subcommand = nullptr
) {
    if (!parsed.valid) return {};

    auto commands = get_all_commands(parsed);
    // Filter to CommandAst only
    std::vector<const ParsedCommandElement*> cmd_asts;
    for (const auto& cmd : commands) {
        if (cmd.element_type == PipelineElementType::CommandAst) {
            cmd_asts.push_back(&cmd);
        }
    }

    if (cmd_asts.size() <= 1) {
        if (cmd_asts.empty()) return {};
        auto prefix = extract_prefix_from_element(*cmd_asts[0]);
        return prefix ? std::vector<std::string>{*prefix} : std::vector<std::string>{};
    }

    std::vector<std::string> prefixes;
    for (const auto* cmd : cmd_asts) {
        if (exclude_subcommand && exclude_subcommand(*cmd)) continue;
        auto prefix = extract_prefix_from_element(*cmd);
        if (prefix) prefixes.push_back(*prefix);
    }

    if (prefixes.empty()) return {};

    // Group by root command (first word, lowercased) and collapse via LCP
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const auto& prefix : prefixes) {
        auto space_pos = prefix.find(' ');
        std::string root = (space_pos != std::string::npos)
            ? prefix.substr(0, space_pos) : prefix;
        std::string key = to_lower(root);
        groups[key].push_back(prefix);
    }

    std::vector<std::string> collapsed;
    for (const auto& [root_lower, group] : groups) {
        std::string lcp = word_aligned_lcp(group);
        // Count words in LCP
        std::size_t word_count = lcp.empty() ? 0 : 1;
        for (char c : lcp) { if (c == ' ') ++word_count; }

        if (word_count <= 1) {
            // Too broad — skip if the command is subcommand-aware
            // (simplified: always include for non-cmdlet externals)
            continue;
        }
        collapsed.push_back(lcp);
    }
    return collapsed;
}

} // namespace cc::utils::powershell_parser
