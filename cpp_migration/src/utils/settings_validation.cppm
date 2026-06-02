module;

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.settings_validation;

export namespace cc::utils::settings_validation {

struct ValidationOutcome {
    bool valid = true;
    std::optional<std::string> error;
    std::optional<std::string> suggestion;
    std::vector<std::string> examples;
};

struct ValidationTip {
    std::optional<std::string> suggestion;
    std::optional<std::string> doc_link;
};

struct TipContext {
    std::string path;
    std::string code;
    std::optional<std::string> expected;
    std::optional<std::string> received;
    std::optional<std::vector<std::string>> enum_values;
    std::optional<std::string> message;
    std::optional<std::string> value;
};

namespace detail {
    inline constexpr std::string_view documentation_base = "https://code.claude.com/docs/en";

    [[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    }

    [[nodiscard]] inline bool starts_with(std::string_view value, std::string_view prefix) {
        return value.substr(0, prefix.size()) == prefix;
    }

    [[nodiscard]] inline bool ends_with(std::string_view value, std::string_view suffix) {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    [[nodiscard]] inline std::string capitalize(std::string value) {
        if (!value.empty()) value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
        return value;
    }

    [[nodiscard]] inline bool is_escaped(std::string_view str, std::size_t index) {
        std::size_t count = 0;
        while (index > 0 && str[index - 1] == '\\') {
            ++count;
            --index;
        }
        return count % 2 != 0;
    }

    [[nodiscard]] inline std::size_t count_unescaped_char(std::string_view str, char ch) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < str.size(); ++i) {
            if (str[i] == ch && !is_escaped(str, i)) ++count;
        }
        return count;
    }

    [[nodiscard]] inline bool has_unescaped_empty_parens(std::string_view str) {
        for (std::size_t i = 0; i + 1 < str.size(); ++i) {
            if (str[i] == '(' && str[i + 1] == ')' && !is_escaped(str, i)) return true;
        }
        return false;
    }

    struct ParsedRule {
        std::string tool_name;
        std::optional<std::string> rule_content;
    };

    [[nodiscard]] inline ParsedRule parse_permission_rule(std::string_view rule) {
        for (std::size_t i = 0; i < rule.size(); ++i) {
            if (rule[i] == '(' && !is_escaped(rule, i)) {
                std::size_t close = std::string_view::npos;
                for (std::size_t j = rule.size(); j > i + 1; --j) {
                    const std::size_t idx = j - 1;
                    if (rule[idx] == ')' && !is_escaped(rule, idx)) {
                        close = idx;
                        break;
                    }
                }
                if (close != std::string_view::npos && close > i) {
                    return {
                        .tool_name = std::string(rule.substr(0, i)),
                        .rule_content = std::string(rule.substr(i + 1, close - i - 1)),
                    };
                }
                return {.tool_name = std::string(rule.substr(0, i)), .rule_content = std::nullopt};
            }
        }
        return {.tool_name = std::string(rule), .rule_content = std::nullopt};
    }

    struct McpInfo {
        std::string server_name;
        std::optional<std::string> tool_name;
    };

    [[nodiscard]] inline std::optional<McpInfo> mcp_info_from_string(std::string_view tool_name) {
        if (!starts_with(tool_name, "mcp__")) return std::nullopt;
        const std::string rest(tool_name.substr(5));
        const auto sep = rest.find("__");
        if (sep == std::string::npos) return McpInfo{.server_name = rest, .tool_name = std::nullopt};
        return McpInfo{.server_name = rest.substr(0, sep), .tool_name = rest.substr(sep + 2)};
    }

    [[nodiscard]] inline std::string join_quoted(const std::vector<std::string>& values) {
        std::string result;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) result += ", ";
            result += '"';
            result += values[i];
            result += '"';
        }
        return result;
    }
} // namespace detail

[[nodiscard]] inline const std::vector<std::string>& file_pattern_tools() {
    static const std::vector<std::string> tools = {"Read", "Write", "Edit", "Glob", "NotebookRead", "NotebookEdit"};
    return tools;
}

[[nodiscard]] inline const std::vector<std::string>& bash_prefix_tools() {
    static const std::vector<std::string> tools = {"Bash"};
    return tools;
}

[[nodiscard]] inline bool is_file_pattern_tool(std::string_view tool_name) {
    const auto& tools = file_pattern_tools();
    return std::find(tools.begin(), tools.end(), tool_name) != tools.end();
}

[[nodiscard]] inline bool is_bash_prefix_tool(std::string_view tool_name) {
    const auto& tools = bash_prefix_tools();
    return std::find(tools.begin(), tools.end(), tool_name) != tools.end();
}

[[nodiscard]] inline ValidationOutcome validate_tool_content(std::string_view tool_name, std::string_view content) {
    if (tool_name == "WebSearch") {
        if (detail::contains(content, "*") || detail::contains(content, "?")) {
            return {
                .valid = false,
                .error = "WebSearch does not support wildcards",
                .suggestion = "Use exact search terms without * or ?",
                .examples = {"WebSearch(claude ai)", "WebSearch(typescript tutorial)"},
            };
        }
        return {.valid = true, .error = std::nullopt, .suggestion = std::nullopt, .examples = {}};
    }

    if (tool_name == "WebFetch") {
        if (detail::contains(content, "://") || detail::starts_with(content, "http")) {
            return {
                .valid = false,
                .error = "WebFetch permissions use domain format, not URLs",
                .suggestion = "Use \"domain:hostname\" format",
                .examples = {"WebFetch(domain:example.com)", "WebFetch(domain:github.com)"},
            };
        }
        if (!detail::starts_with(content, "domain:")) {
            return {
                .valid = false,
                .error = "WebFetch permissions must use \"domain:\" prefix",
                .suggestion = "Use \"domain:hostname\" format",
                .examples = {"WebFetch(domain:example.com)", "WebFetch(domain:*.google.com)"},
            };
        }
        return {.valid = true, .error = std::nullopt, .suggestion = std::nullopt, .examples = {}};
    }

    return {.valid = true, .error = std::nullopt, .suggestion = std::nullopt, .examples = {}};
}

[[nodiscard]] inline ValidationOutcome validate_permission_rule(std::string_view rule) {
    if (rule.empty() || std::all_of(rule.begin(), rule.end(), [](unsigned char c) { return std::isspace(c); })) {
        return {.valid = false, .error = "Permission rule cannot be empty", .suggestion = std::nullopt, .examples = {}};
    }

    const auto open_count = detail::count_unescaped_char(rule, '(');
    const auto close_count = detail::count_unescaped_char(rule, ')');
    if (open_count != close_count) {
        return {
            .valid = false,
            .error = "Mismatched parentheses",
            .suggestion = "Ensure all opening parentheses have matching closing parentheses",
            .examples = {},
        };
    }

    if (detail::has_unescaped_empty_parens(rule)) {
        const auto paren = rule.find('(');
        const std::string tool_name = paren == std::string_view::npos ? std::string{} : std::string(rule.substr(0, paren));
        if (tool_name.empty()) {
            return {.valid = false, .error = "Empty parentheses with no tool name", .suggestion = "Specify a tool name before the parentheses", .examples = {}};
        }
        return {
            .valid = false,
            .error = "Empty parentheses",
            .suggestion = "Either specify a pattern or use just \"" + tool_name + "\" without parentheses",
            .examples = {tool_name, tool_name + "(some-pattern)"},
        };
    }

    const auto parsed = detail::parse_permission_rule(rule);
    const auto mcp_info = detail::mcp_info_from_string(parsed.tool_name);
    if (mcp_info.has_value()) {
        if (parsed.rule_content.has_value() || open_count > 0) {
            std::vector<std::string> examples = {"mcp__" + mcp_info->server_name, "mcp__" + mcp_info->server_name + "__*"};
            if (mcp_info->tool_name.has_value() && *mcp_info->tool_name != "*") {
                examples.push_back("mcp__" + mcp_info->server_name + "__" + *mcp_info->tool_name);
            }
            return {
                .valid = false,
                .error = "MCP rules do not support patterns in parentheses",
                .suggestion = "Use \"" + parsed.tool_name + "\" without parentheses, or use \"mcp__" + mcp_info->server_name + "__*\" for all tools",
                .examples = examples,
            };
        }
        return {.valid = true, .error = std::nullopt, .suggestion = std::nullopt, .examples = {}};
    }

    if (parsed.tool_name.empty()) return {.valid = false, .error = "Tool name cannot be empty", .suggestion = std::nullopt, .examples = {}};
    if (!std::isupper(static_cast<unsigned char>(parsed.tool_name[0]))) {
        return {.valid = false, .error = "Tool names must start with uppercase", .suggestion = "Use \"" + detail::capitalize(parsed.tool_name) + "\"", .examples = {}};
    }

    if (parsed.rule_content.has_value()) {
        auto custom = validate_tool_content(parsed.tool_name, *parsed.rule_content);
        if (!custom.valid) return custom;
    }

    if (is_bash_prefix_tool(parsed.tool_name) && parsed.rule_content.has_value()) {
        const auto& content = *parsed.rule_content;
        if (detail::contains(content, ":*") && !detail::ends_with(content, ":*")) {
            return {
                .valid = false,
                .error = "The :* pattern must be at the end",
                .suggestion = "Move :* to the end for prefix matching, or use * for wildcard matching",
                .examples = {"Bash(npm run:*) - prefix matching (legacy)", "Bash(npm run *) - wildcard matching"},
            };
        }
        if (content == ":*") {
            return {.valid = false, .error = "Prefix cannot be empty before :*", .suggestion = "Specify a command prefix before :*", .examples = {"Bash(npm:*)", "Bash(git:*)"}};
        }
    }

    if (is_file_pattern_tool(parsed.tool_name) && parsed.rule_content.has_value()) {
        const auto& content = *parsed.rule_content;
        if (detail::contains(content, ":*")) {
            return {
                .valid = false,
                .error = "The \":*\" syntax is only for Bash prefix rules",
                .suggestion = "Use glob patterns like \"*\" or \"**\" for file matching",
                .examples = {parsed.tool_name + "(*.ts) - matches .ts files", parsed.tool_name + "(src/**) - matches all files in src", parsed.tool_name + "(**/*.test.ts) - matches test files"},
            };
        }
        const bool suspicious_wildcard = detail::contains(content, "*") &&
            !(detail::starts_with(content, "*") || detail::ends_with(content, "*") || detail::contains(content, "**") || detail::contains(content, "/*") || detail::contains(content, "*."));
        if (suspicious_wildcard) {
            return {
                .valid = false,
                .error = "Wildcard placement might be incorrect",
                .suggestion = "Wildcards are typically used at path boundaries",
                .examples = {parsed.tool_name + "(*.js) - all .js files", parsed.tool_name + "(src/*) - all files directly in src", parsed.tool_name + "(src/**) - all files recursively in src"},
            };
        }
    }

    return {.valid = true, .error = std::nullopt, .suggestion = std::nullopt, .examples = {}};
}

[[nodiscard]] inline std::optional<ValidationTip> get_validation_tip(const TipContext& context) {
    ValidationTip tip;
    bool matched = true;

    if (context.path == "permissions.defaultMode" && context.code == "invalid_value") {
        tip.suggestion = "Valid modes: \"acceptEdits\" (ask before file changes), \"plan\" (analysis only), \"bypassPermissions\" (auto-accept all), or \"default\" (standard behavior)";
        tip.doc_link = std::string(detail::documentation_base) + "/iam#permission-modes";
    } else if (context.path == "apiKeyHelper" && context.code == "invalid_type") {
        tip.suggestion = "Provide a shell command that outputs your API key to stdout. The script should output only the API key. Example: \"/bin/generate_temp_api_key.sh\"";
    } else if (context.path == "cleanupPeriodDays" && context.code == "too_small" && context.expected == "0") {
        tip.suggestion = "Must be 0 or greater. Set a positive number for days to retain transcripts (default is 30). Setting 0 disables session persistence entirely: no transcripts are written and existing transcripts are deleted at startup.";
    } else if (detail::starts_with(context.path, "env.") && context.code == "invalid_type") {
        tip.suggestion = "Environment variables must be strings. Wrap numbers and booleans in quotes. Example: \"DEBUG\": \"true\", \"PORT\": \"3000\"";
        tip.doc_link = std::string(detail::documentation_base) + "/settings#environment-variables";
    } else if ((context.path == "permissions.allow" || context.path == "permissions.deny") && context.code == "invalid_type" && context.expected == "array") {
        tip.suggestion = "Permission rules must be in an array. Format: [\"Tool(specifier)\"]. Examples: [\"Bash(npm run build)\", \"Edit(docs/**)\", \"Read(~/.zshrc)\"]. Use * for wildcards.";
    } else if (detail::contains(context.path, "hooks") && context.code == "invalid_type") {
        tip.suggestion = "Hooks use a matcher + hooks array. The matcher is a string: a tool name (\"Bash\"), pipe-separated list (\"Edit|Write\"), or empty to match all. Example: {\"PostToolUse\": [{\"matcher\": \"Edit|Write\", \"hooks\": [{\"type\": \"command\", \"command\": \"echo Done\"}]}]}";
    } else if (context.code == "invalid_type" && context.expected == "boolean") {
        tip.suggestion = "Use true or false without quotes. Example: \"includeCoAuthoredBy\": true";
    } else if (context.code == "unrecognized_keys") {
        tip.suggestion = "Check for typos or refer to the documentation for valid fields";
        tip.doc_link = std::string(detail::documentation_base) + "/settings";
    } else if (context.code == "invalid_value" && context.enum_values.has_value()) {
        tip.suggestion = "Valid values: " + detail::join_quoted(*context.enum_values);
    } else if (context.code == "invalid_type" && context.expected == "object" && context.received == "null" && context.path.empty()) {
        tip.suggestion = "Check for missing commas, unmatched brackets, or trailing commas. Use a JSON validator to identify the exact syntax error.";
    } else if (context.path == "permissions.additionalDirectories" && context.code == "invalid_type") {
        tip.suggestion = "Must be an array of directory paths. Example: [\"~/projects\", \"/tmp/workspace\"]. You can also use --add-dir flag or /add-dir command";
        tip.doc_link = std::string(detail::documentation_base) + "/iam#working-directories";
    } else {
        matched = false;
    }

    if (!matched) return std::nullopt;
    if (!tip.doc_link.has_value() && !context.path.empty()) {
        const auto dot = context.path.find('.');
        const auto prefix = context.path.substr(0, dot == std::string::npos ? context.path.size() : dot);
        if (prefix == "permissions") tip.doc_link = std::string(detail::documentation_base) + "/iam#configuring-permissions";
        else if (prefix == "env") tip.doc_link = std::string(detail::documentation_base) + "/settings#environment-variables";
        else if (prefix == "hooks") tip.doc_link = std::string(detail::documentation_base) + "/hooks";
    }
    return tip;
}

} // namespace cc::utils::settings_validation
