// C++23 Settings Rules Module
// Merges: permissionValidation.ts, pluginOnlyPolicy.ts, toolValidationConfig.ts,
//         validateEditTool.ts, validation.ts, validationTips.ts, allErrors.ts,
//         schemaOutput.ts, managedPath.ts, types.ts, constants.ts
module;

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module cc.utils.settings_rules;

export namespace cc::utils::settings_rules {

namespace fs = std::filesystem;

// ============================================================================
// Constants
// ============================================================================

/// All known setting sources in merge-priority order
inline constexpr std::array<std::string_view, 5> SETTING_SOURCES = {
    "userSettings",
    "projectSettings",
    "localSettings",
    "flagSettings",
    "policySettings",
};

/// Managed settings file name
inline constexpr std::string_view MANAGED_SETTINGS_FILENAME = "managed-settings.json";

/// Drop-in directory name for managed settings fragments
inline constexpr std::string_view MANAGED_SETTINGS_DROP_IN_DIR = "managed-settings.d";

/// Default permission mode
inline constexpr std::string_view DEFAULT_PERMISSION_MODE = "normal";

/// Hook event types
inline constexpr std::array<std::string_view, 12> HOOK_EVENTS = {
    "PreToolUse", "PostToolUse", "Notification", "UserPromptSubmit",
    "SessionStart", "SessionEnd", "Stop", "SubagentStop",
    "PreCompact", "PostCompact", "TeammateIdle", "TaskCreated",
};

// ============================================================================
// Types
// ============================================================================

/// Flag argument types for command validation
enum class FlagArgType {
    None,    // No argument (--color, -n)
    Number,  // Integer argument (--context=3)
    String,  // Any string argument (--relative=path)
    Char,    // Single character (delimiter)
    Braces,  // Literal "{}" only
    Eof,     // Literal "EOF" only
};

/// Result of permission rule validation
struct ValidationResult {
    bool valid = true;
    std::string error;
    std::string suggestion;
    std::vector<std::string> examples;
};

[[nodiscard]] inline ValidationResult valid_result() {
    return {};
}

[[nodiscard]] inline ValidationResult invalid_result(
    std::string error,
    std::string suggestion = {},
    std::vector<std::string> examples = {})
{
    ValidationResult result;
    result.valid = false;
    result.error = std::move(error);
    result.suggestion = std::move(suggestion);
    result.examples = std::move(examples);
    return result;
}

/// A single validation error with location context
struct SettingsError {
    std::string file;
    std::string path;
    std::string message;
    std::string source;

    [[nodiscard]] std::string key() const {
        return file + ":" + path + ":" + message;
    }
};

/// External command read-only configuration
struct ExternalCommandConfig {
    std::map<std::string, FlagArgType> safe_flags;
    std::function<bool(std::string_view, const std::vector<std::string>&)>
        additional_is_dangerous_callback;
    bool respects_double_dash = true;
};

// ============================================================================
// PermissionValidator — Validates permission rule format and content
// ============================================================================

/// Validates permission rules for correctness and provides helpful diagnostics.
/// Port of permissionValidation.ts.
class PermissionValidator {
public:
    /// Validate a single permission rule string
    [[nodiscard]] static ValidationResult validate(std::string_view rule) {
        // Empty rule check
        if (rule.empty() || is_only_whitespace(rule)) {
            return invalid_result("Permission rule cannot be empty");
        }

        std::string rule_str(rule);

        // Check parentheses matching (only unescaped)
        int open_count = count_unescaped_char(rule_str, '(');
        int close_count = count_unescaped_char(rule_str, ')');
        if (open_count != close_count) {
            return invalid_result(
                "Mismatched parentheses",
                "Ensure all opening parentheses have matching closing parentheses");
        }

        // Check for empty parentheses
        if (has_unescaped_empty_parens(rule_str)) {
            auto paren_pos = rule_str.find('(');
            std::string tool_name = (paren_pos != std::string::npos)
                ? rule_str.substr(0, paren_pos) : "";
            if (tool_name.empty()) {
                return invalid_result(
                    "Empty parentheses with no tool name",
                    "Specify a tool name before the parentheses");
            }
            return invalid_result(
                "Empty parentheses",
                "Either specify a pattern or use just \"" +
                    tool_name + "\" without parentheses",
                {tool_name, tool_name + "(some-pattern)"});
        }

        // Parse the rule into tool name and content
        auto [tool_name, rule_content] = parse_rule(rule_str);

        // MCP validation
        if (is_mcp_rule(tool_name)) {
            if (rule_content.has_value() || open_count > 0) {
                return invalid_result(
                    "MCP rules do not support patterns in parentheses",
                    "Use \"" + tool_name + "\" without parentheses");
            }
            return valid_result();
        }

        // Tool name validation
        if (tool_name.empty()) {
            return invalid_result("Tool name cannot be empty");
        }

        // Tool name must start with uppercase
        if (!std::isupper(static_cast<unsigned char>(tool_name[0]))) {
            std::string capitalized = tool_name;
            capitalized[0] = static_cast<char>(
                std::toupper(static_cast<unsigned char>(capitalized[0])));
            return invalid_result(
                "Tool names must start with uppercase",
                "Use \"" + capitalized + "\"");
        }

        // Bash-specific validation
        if (is_bash_prefix_tool(tool_name) && rule_content.has_value()) {
            auto bash_result = validate_bash_content(*rule_content, tool_name);
            if (!bash_result.valid) return bash_result;
        }

        // File tool validation
        if (is_file_pattern_tool(tool_name) && rule_content.has_value()) {
            auto file_result = validate_file_content(*rule_content, tool_name);
            if (!file_result.valid) return file_result;
        }

        return valid_result();
    }

private:
    [[nodiscard]] static bool is_only_whitespace(std::string_view sv) {
        return std::all_of(sv.begin(), sv.end(), [](char c) {
            return std::isspace(static_cast<unsigned char>(c));
        });
    }

    [[nodiscard]] static bool is_escaped(const std::string& str, size_t index) {
        int backslash_count = 0;
        int j = static_cast<int>(index) - 1;
        while (j >= 0 && str[static_cast<size_t>(j)] == '\\') {
            backslash_count++;
            j--;
        }
        return backslash_count % 2 != 0;
    }

    [[nodiscard]] static int count_unescaped_char(const std::string& str, char ch) {
        int count = 0;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == ch && !is_escaped(str, i)) count++;
        }
        return count;
    }

    [[nodiscard]] static bool has_unescaped_empty_parens(const std::string& str) {
        for (size_t i = 0; i + 1 < str.size(); i++) {
            if (str[i] == '(' && str[i + 1] == ')' && !is_escaped(str, i)) {
                return true;
            }
        }
        return false;
    }

    struct ParsedRule {
        std::string tool_name;
        std::optional<std::string> rule_content;
    };

    [[nodiscard]] static ParsedRule parse_rule(const std::string& rule) {
        auto paren_pos = rule.find('(');
        if (paren_pos == std::string::npos) {
            return {.tool_name = rule, .rule_content = std::nullopt};
        }
        std::string tool = rule.substr(0, paren_pos);
        // Extract content between parens (may be nested)
        std::string content = rule.substr(paren_pos + 1);
        if (!content.empty() && content.back() == ')') {
            content.pop_back();
        }
        return {.tool_name = tool, .rule_content = content};
    }

    [[nodiscard]] static bool is_mcp_rule(const std::string& tool_name) {
        return tool_name.starts_with("mcp__");
    }

    [[nodiscard]] static bool is_bash_prefix_tool(const std::string& tool_name) {
        static const std::unordered_set<std::string> tools = {
            "Bash", "PowerShell", "Shell"
        };
        return tools.contains(tool_name);
    }

    [[nodiscard]] static bool is_file_pattern_tool(const std::string& tool_name) {
        static const std::unordered_set<std::string> tools = {
            "Read", "Write", "Edit", "MultiEdit", "NotebookEdit"
        };
        return tools.contains(tool_name);
    }

    [[nodiscard]] static ValidationResult validate_bash_content(
        const std::string& content, const std::string& tool_name) {
        // Check for :* mistakes
        auto colon_star_pos = content.find(":*");
        if (colon_star_pos != std::string::npos &&
            colon_star_pos + 2 != content.size()) {
            return invalid_result(
                "The :* pattern must be at the end",
                "Move :* to the end for prefix matching",
                {
                    tool_name + "(npm run:*) - prefix matching (legacy)",
                    tool_name + "(npm run *) - wildcard matching",
                });
        }
        if (content == ":*") {
            return invalid_result(
                "Prefix cannot be empty before :*",
                "Specify a command prefix before :*",
                {tool_name + "(npm:*)", tool_name + "(git:*)"});
        }
        return valid_result();
    }

    [[nodiscard]] static ValidationResult validate_file_content(
        const std::string& content, const std::string& tool_name) {
        if (content.find(":*") != std::string::npos) {
            return invalid_result(
                "The \":*\" syntax is only for Bash prefix rules",
                "Use glob patterns like \"*\" or \"**\" for file matching",
                {
                    tool_name + "(*.ts) - matches .ts files",
                    tool_name + "(src/**) - matches all files in src",
                });
        }
        return valid_result();
    }
};

// ============================================================================
// PolicyChecker — Plugin-only policy enforcement
// ============================================================================

/// Enforces plugin-only policies: certain settings keys can only be set
/// by plugins or managed policy, not by user/project settings.
class PolicyChecker {
public:
    /// Check if a key is plugin-only (cannot be set in user/project settings)
    [[nodiscard]] static bool is_plugin_only_key(std::string_view key) {
        static const std::unordered_set<std::string_view> plugin_only_keys = {
            "agent",
            "agentPermissions",
            "pluginCommands",
        };
        return plugin_only_keys.contains(key);
    }

    /// Check if a settings source is allowed to set a specific key
    [[nodiscard]] static bool is_key_allowed_for_source(
        std::string_view key, std::string_view source) {
        if (!is_plugin_only_key(key)) return true;
        // Plugin-only keys can only come from plugins or policy
        return source == "policySettings" || source == "flagSettings";
    }

    /// Filter out plugin-only keys from a settings map
    [[nodiscard]] static std::vector<std::string> get_violations(
        const std::map<std::string, std::string>& settings,
        std::string_view source) {
        std::vector<std::string> violations;
        for (const auto& [key, _] : settings) {
            if (!is_key_allowed_for_source(key, source)) {
                violations.push_back(key);
            }
        }
        return violations;
    }
};

// ============================================================================
// ValidationRule — Tool validation configuration
// ============================================================================

/// Defines validation rules for specific tools (edit tool validation, etc.)
struct ValidationRule {
    std::string tool_name;
    std::string description;
    std::function<ValidationResult(std::string_view)> validate;
};

/// Registry of tool-specific validation rules
class ValidationRuleRegistry {
public:
    /// Register a custom validation rule for a tool
    void register_rule(ValidationRule rule) {
        rules_[rule.tool_name] = std::move(rule);
    }

    /// Get custom validation for a tool (if any)
    [[nodiscard]] std::optional<std::reference_wrapper<const ValidationRule>>
    get_rule(const std::string& tool_name) const {
        auto it = rules_.find(tool_name);
        if (it == rules_.end()) return std::nullopt;
        return std::cref(it->second);
    }

    /// Get all registered rules
    [[nodiscard]] const std::map<std::string, ValidationRule>& rules() const {
        return rules_;
    }

private:
    std::map<std::string, ValidationRule> rules_;
};

// ============================================================================
// CommandValidator — Read-only command validation (from readOnlyCommandValidation.ts)
// ============================================================================

/// Validates shell commands against read-only command configurations.
/// Determines if a command is safe to auto-approve without user permission.
class CommandValidator {
public:
    /// Register a set of read-only command configs
    void register_commands(std::map<std::string, ExternalCommandConfig> commands) {
        for (auto& [name, config] : commands) {
            commands_[name] = std::move(config);
        }
    }

    /// Check if a command matches any registered read-only command
    [[nodiscard]] bool is_read_only_command(
        std::string_view command,
        const std::vector<std::string>& tokens) const {
        for (const auto& [prefix, config] : commands_) {
            if (matches_prefix(command, prefix)) {
                // Get args after prefix
                auto args = extract_args_after_prefix(tokens, prefix);
                // Check additional callback
                if (config.additional_is_dangerous_callback &&
                    config.additional_is_dangerous_callback(command, args)) {
                    return false;
                }
                // Validate flags
                return validate_flags(args, config);
            }
        }
        return false;
    }

    /// Validate flags against a command configuration
    [[nodiscard]] static bool validate_flags(
        const std::vector<std::string>& args,
        const ExternalCommandConfig& config) {
        size_t i = 0;
        while (i < args.size()) {
            const auto& token = args[i];
            if (token == "--") {
                if (config.respects_double_dash) break;
                i++;
                continue;
            }

            if (token.starts_with("-") && token.size() > 1) {
                // Handle --flag=value
                auto eq_pos = token.find('=');
                std::string flag = (eq_pos != std::string::npos)
                    ? token.substr(0, eq_pos) : token;
                std::string inline_value = (eq_pos != std::string::npos)
                    ? token.substr(eq_pos + 1) : "";
                bool has_equals = (eq_pos != std::string::npos);

                auto it = config.safe_flags.find(flag);
                if (it == config.safe_flags.end()) return false;

                if (it->second == FlagArgType::None) {
                    if (has_equals) return false;
                    i++;
                } else {
                    if (has_equals) {
                        if (!validate_flag_argument(inline_value, it->second))
                            return false;
                        i++;
                    } else {
                        if (i + 1 >= args.size()) return false;
                        if (!validate_flag_argument(args[i + 1], it->second))
                            return false;
                        i += 2;
                    }
                }
            } else {
                // Non-flag argument (positional) — allowed
                i++;
            }
        }
        return true;
    }

    /// Validate a flag argument against its expected type
    [[nodiscard]] static bool validate_flag_argument(
        const std::string& value, FlagArgType arg_type) {
        switch (arg_type) {
            case FlagArgType::None: return false;
            case FlagArgType::Number:
                return std::all_of(value.begin(), value.end(), ::isdigit) &&
                       !value.empty();
            case FlagArgType::String: return true;
            case FlagArgType::Char: return value.size() == 1;
            case FlagArgType::Braces: return value == "{}";
            case FlagArgType::Eof: return value == "EOF";
        }
        return false;
    }

    /// Check if command contains vulnerable UNC paths (Windows only)
    [[nodiscard]] static bool contains_vulnerable_unc_path(
        [[maybe_unused]] std::string_view command) {
        // Only relevant on Windows — on other platforms always returns false
        #ifdef _WIN32
        // Basic UNC path detection: \\server\share or //server/share
        static const std::regex unc_pattern(R"(\\\\[^\s\\/]+[\\/])");
        static const std::regex fwd_unc_pattern(R"((?<!:)//[^\s\\/]+[\\/])");
        std::string cmd(command);
        return std::regex_search(cmd, unc_pattern) ||
               std::regex_search(cmd, fwd_unc_pattern);
        #else
        return false;
        #endif
    }

private:
    [[nodiscard]] static bool matches_prefix(
        std::string_view command, const std::string& prefix) {
        if (command.size() < prefix.size()) return false;
        if (command.substr(0, prefix.size()) != prefix) return false;
        // Must be followed by space, end, or pipe
        if (command.size() == prefix.size()) return true;
        char next = command[prefix.size()];
        return next == ' ' || next == '\t' || next == '|' || next == ';';
    }

    [[nodiscard]] static std::vector<std::string> extract_args_after_prefix(
        const std::vector<std::string>& tokens, const std::string& prefix) {
        // Count words in prefix
        size_t prefix_words = 1;
        for (char c : prefix) {
            if (c == ' ') prefix_words++;
        }
        if (prefix_words >= tokens.size()) return {};
        return {tokens.begin() + static_cast<ptrdiff_t>(prefix_words),
                tokens.end()};
    }

    std::map<std::string, ExternalCommandConfig> commands_;
};

// ============================================================================
// ManagedPath — Platform-specific managed settings path resolution
// ============================================================================

/// Resolves platform-specific paths for managed settings files.
struct ManagedPath {
    /// Get the managed settings base directory for the current platform
    [[nodiscard]] static fs::path get_managed_file_path() {
        #if defined(__APPLE__)
        return "/Library/Application Support/ClaudeCode";
        #elif defined(_WIN32)
        // PROGRAMDATA or fallback
        if (const char* pd = std::getenv("PROGRAMDATA")) {
            return fs::path(pd) / "ClaudeCode";
        }
        return "C:\\ProgramData\\ClaudeCode";
        #else
        return "/etc/claude-code";
        #endif
    }

    /// Get the managed settings drop-in directory
    [[nodiscard]] static fs::path get_drop_in_dir() {
        return get_managed_file_path() / std::string(MANAGED_SETTINGS_DROP_IN_DIR);
    }

    /// Get the full path to managed-settings.json
    [[nodiscard]] static fs::path get_managed_settings_file() {
        return get_managed_file_path() / std::string(MANAGED_SETTINGS_FILENAME);
    }
};

// ============================================================================
// SchemaOutput — Settings schema documentation output
// ============================================================================

/// Generates human-readable schema documentation for settings.
struct SchemaOutput {
    /// Generate a list of all valid top-level setting keys
    [[nodiscard]] static std::vector<std::string> get_valid_keys() {
        return {
            "permissions", "sandbox", "hooks",
            "agent", "agentPermissions",
            "enabledPlugins", "extraKnownMarketplaces",
            "cleanupPeriodDays", "model",
            "skipDangerousModePermissionPrompt",
            "skipAutoPermissionPrompt",
            "useAutoModeDuringPlan",
        };
    }

    /// Get keys that should be expanded one level deep in logging
    [[nodiscard]] static std::vector<std::string> get_expandable_keys() {
        return {"permissions", "sandbox", "hooks"};
    }
};

// ============================================================================
// AllErrors — Error aggregation utilities
// ============================================================================

/// Collects and deduplicates settings errors from all sources.
class ErrorCollector {
public:
    /// Add an error (deduplicates by file:path:message)
    void add(SettingsError error) {
        auto key = error.key();
        if (seen_keys_.insert(key).second) {
            errors_.push_back(std::move(error));
        }
    }

    /// Add multiple errors
    void add_all(const std::vector<SettingsError>& errors) {
        for (const auto& e : errors) add(e);
    }

    /// Get all collected errors
    [[nodiscard]] const std::vector<SettingsError>& errors() const {
        return errors_;
    }

    /// Check if there are any errors
    [[nodiscard]] bool has_errors() const { return !errors_.empty(); }

    /// Clear all errors
    void clear() {
        errors_.clear();
        seen_keys_.clear();
    }

private:
    std::vector<SettingsError> errors_;
    std::unordered_set<std::string> seen_keys_;
};

} // namespace cc::utils::settings_rules
