// C++23 Shell Tools Module
// Merges: shellToolUtils.ts, readOnlyCommandValidation.ts, outputLimits.ts,
//         prefix.ts, specPrefix.ts
module;

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

export module cc.utils.shell_tools;

export namespace cc::utils::shell_tools {

// ============================================================================
// Constants — Output limits
// ============================================================================

/// Maximum output length upper limit (hard cap)
inline constexpr size_t BASH_MAX_OUTPUT_UPPER_LIMIT = 150'000;

/// Default maximum output length
inline constexpr size_t BASH_MAX_OUTPUT_DEFAULT = 30'000;

/// Maximum number of lines to show in truncated output header
inline constexpr size_t TRUNCATION_HEADER_LINES = 5;

/// Maximum number of lines to show in truncated output footer
inline constexpr size_t TRUNCATION_FOOTER_LINES = 200;

// ============================================================================
// OutputLimiter — Controls shell output truncation
// ============================================================================

/// Manages output length limits for shell command results.
/// Respects BASH_MAX_OUTPUT_LENGTH env var bounded by the upper limit.
class OutputLimiter {
public:
    /// Get the effective maximum output length
    [[nodiscard]] static size_t get_max_output_length() {
        if (const char* env = std::getenv("BASH_MAX_OUTPUT_LENGTH")) {
            try {
                int value = std::stoi(env);
                if (value > 0) {
                    return std::min(static_cast<size_t>(value),
                                    BASH_MAX_OUTPUT_UPPER_LIMIT);
                }
            } catch (...) {
                // Invalid value, use default
            }
        }
        return BASH_MAX_OUTPUT_DEFAULT;
    }

    /// Truncate output if it exceeds the maximum length.
    /// Returns the truncated output with a message indicating truncation.
    [[nodiscard]] static std::string truncate_output(
        std::string_view output, size_t max_length = 0) {
        if (max_length == 0) max_length = get_max_output_length();
        if (output.size() <= max_length) return std::string(output);

        // Split into lines
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start < output.size()) {
            auto newline = output.find('\n', start);
            if (newline == std::string_view::npos) {
                lines.push_back(output.substr(start));
                break;
            }
            lines.push_back(output.substr(start, newline - start));
            start = newline + 1;
        }

        // Take header and footer lines
        std::string result;
        size_t header_end = std::min(TRUNCATION_HEADER_LINES, lines.size());
        size_t footer_start = (lines.size() > TRUNCATION_FOOTER_LINES)
            ? lines.size() - TRUNCATION_FOOTER_LINES
            : header_end;

        for (size_t i = 0; i < header_end; i++) {
            result += lines[i];
            result += '\n';
        }

        if (footer_start > header_end) {
            result += "\n... [" +
                      std::to_string(footer_start - header_end) +
                      " lines truncated] ...\n\n";
        }

        for (size_t i = footer_start; i < lines.size(); i++) {
            result += lines[i];
            if (i + 1 < lines.size()) result += '\n';
        }

        return result;
    }

    /// Check if output would be truncated
    [[nodiscard]] static bool would_truncate(std::string_view output) {
        return output.size() > get_max_output_length();
    }
};

// ============================================================================
// CommandValidator — Validates read-only commands for auto-approval
// ============================================================================

/// Flag argument type definitions
enum class FlagArgType {
    None,    // No argument
    Number,  // Integer argument
    String,  // Any string argument
    Char,    // Single character
    Braces,  // Literal "{}" only
    Eof,     // Literal "EOF" only
};

/// Configuration for a read-only external command
struct ExternalCommandConfig {
    std::map<std::string, FlagArgType> safe_flags;
    std::function<bool(std::string_view, const std::vector<std::string>&)>
        is_dangerous_callback;
    bool respects_double_dash = true;
};

/// Validates shell commands to determine if they're safe for auto-approval.
/// Port of readOnlyCommandValidation.ts logic.
class CommandValidator {
public:
    /// Register read-only command configurations
    void register_commands(
        const std::map<std::string, ExternalCommandConfig>& commands) {
        for (const auto& [name, config] : commands) {
            registered_commands_[name] = config;
        }
    }

    /// Register simple external read-only commands (no flag validation needed)
    void register_simple_commands(const std::vector<std::string>& commands) {
        for (const auto& cmd : commands) {
            simple_readonly_commands_.insert(cmd);
        }
    }

    /// Check if a tokenized command is read-only safe
    [[nodiscard]] bool is_read_only(
        std::string_view raw_command,
        const std::vector<std::string>& tokens) const {

        if (tokens.empty()) return false;

        // Check simple commands first
        std::string first_token = tokens[0];
        if (tokens.size() >= 2) {
            std::string two_tokens = tokens[0] + " " + tokens[1];
            if (simple_readonly_commands_.contains(two_tokens)) return true;
        }
        if (simple_readonly_commands_.contains(first_token)) return true;

        // Try matching against registered command configs (longest prefix first)
        for (const auto& [prefix, config] : registered_commands_) {
            if (!command_matches_prefix(tokens, prefix)) continue;

            // Extract args after prefix
            size_t prefix_word_count = count_words(prefix);
            std::vector<std::string> args(
                tokens.begin() + static_cast<ptrdiff_t>(prefix_word_count),
                tokens.end());

            // Check additional dangerous callback
            if (config.is_dangerous_callback &&
                config.is_dangerous_callback(raw_command, args)) {
                return false;
            }

            // Validate flags
            return validate_flags(args, config);
        }

        return false;
    }

    /// Validate flags of a command against its config
    [[nodiscard]] static bool validate_flags(
        const std::vector<std::string>& args,
        const ExternalCommandConfig& config) {

        size_t i = 0;
        while (i < args.size()) {
            const auto& token = args[i];
            if (token.empty()) { i++; continue; }

            // Double dash: end of options
            if (token == "--") {
                if (config.respects_double_dash) { i++; break; }
                i++;
                continue;
            }

            // Flag token
            if (token[0] == '-' && token.size() > 1) {
                bool has_equals = token.find('=') != std::string::npos;
                std::string flag;
                std::string inline_value;

                if (has_equals) {
                    auto eq_pos = token.find('=');
                    flag = token.substr(0, eq_pos);
                    inline_value = token.substr(eq_pos + 1);
                } else {
                    flag = token;
                }

                auto it = config.safe_flags.find(flag);
                if (it == config.safe_flags.end()) {
                    // Try combined short flags (e.g., -nr)
                    if (flag.size() > 2 && flag[1] != '-') {
                        bool all_valid = true;
                        for (size_t j = 1; j < flag.size(); j++) {
                            std::string single_flag = "-";
                            single_flag += flag[j];
                            auto sf_it = config.safe_flags.find(single_flag);
                            if (sf_it == config.safe_flags.end() ||
                                sf_it->second != FlagArgType::None) {
                                all_valid = false;
                                break;
                            }
                        }
                        if (all_valid) { i++; continue; }
                    }
                    return false; // Unknown flag
                }

                auto arg_type = it->second;
                if (arg_type == FlagArgType::None) {
                    if (has_equals) return false;
                    i++;
                } else {
                    if (has_equals) {
                        if (!validate_flag_argument(inline_value, arg_type))
                            return false;
                        i++;
                    } else {
                        if (i + 1 >= args.size()) return false;
                        if (!validate_flag_argument(args[i + 1], arg_type))
                            return false;
                        i += 2;
                    }
                }
            } else {
                // Positional argument — allowed
                i++;
            }
        }
        return true;
    }

    /// Validate a single flag argument value
    [[nodiscard]] static bool validate_flag_argument(
        const std::string& value, FlagArgType type) {
        switch (type) {
            case FlagArgType::None: return false;
            case FlagArgType::Number:
                return !value.empty() &&
                       std::all_of(value.begin(), value.end(), ::isdigit);
            case FlagArgType::String: return true;
            case FlagArgType::Char: return value.size() == 1;
            case FlagArgType::Braces: return value == "{}";
            case FlagArgType::Eof: return value == "EOF";
        }
        return false;
    }

    /// Check if a command contains vulnerable UNC paths (Windows only)
    [[nodiscard]] static bool contains_vulnerable_unc_path(
        [[maybe_unused]] std::string_view command) {
        #ifdef _WIN32
        // Detect \\server\share or //server/share patterns
        return command.find("\\\\") != std::string_view::npos ||
               (command.find("//") != std::string_view::npos &&
                command.find("://") == std::string_view::npos);
        #else
        return false;
        #endif
    }

private:
    [[nodiscard]] static bool command_matches_prefix(
        const std::vector<std::string>& tokens,
        const std::string& prefix) {
        // Split prefix into words
        std::vector<std::string> prefix_words;
        size_t start = 0;
        while (start < prefix.size()) {
            auto space = prefix.find(' ', start);
            if (space == std::string::npos) {
                prefix_words.push_back(prefix.substr(start));
                break;
            }
            prefix_words.push_back(prefix.substr(start, space - start));
            start = space + 1;
        }

        if (prefix_words.size() > tokens.size()) return false;
        for (size_t i = 0; i < prefix_words.size(); i++) {
            if (tokens[i] != prefix_words[i]) return false;
        }
        return true;
    }

    [[nodiscard]] static size_t count_words(const std::string& s) {
        size_t count = 1;
        for (char c : s) {
            if (c == ' ') count++;
        }
        return count;
    }

    std::map<std::string, ExternalCommandConfig> registered_commands_;
    std::unordered_set<std::string> simple_readonly_commands_;
};

// ============================================================================
// ShellPrefix — Shell command prefix handling
// ============================================================================

/// Manages shell command prefix configuration (CLAUDE_CODE_SHELL_PREFIX).
/// Used to wrap commands with custom shell initialization.
class ShellPrefix {
public:
    /// Check if a shell prefix is configured
    [[nodiscard]] static bool is_configured() {
        return std::getenv("CLAUDE_CODE_SHELL_PREFIX") != nullptr;
    }

    /// Get the configured shell prefix (if any)
    [[nodiscard]] static std::optional<std::string> get_prefix() {
        if (const char* val = std::getenv("CLAUDE_CODE_SHELL_PREFIX")) {
            std::string prefix(val);
            if (!prefix.empty()) return prefix;
        }
        return std::nullopt;
    }

    /// Format a command with the shell prefix.
    /// The prefix command receives the actual command as a single-quoted argument.
    [[nodiscard]] static std::string format_command(
        std::string_view prefix, std::string_view command) {
        return std::string(prefix) + " " + shell_quote(command);
    }

    /// Get the spec prefix for tool specifications.
    /// Used in tool descriptions to indicate the execution environment.
    [[nodiscard]] static std::string get_spec_prefix() {
        auto prefix = get_prefix();
        if (!prefix) return "";
        // Extract the meaningful part for display
        auto last_slash = prefix->rfind('/');
        if (last_slash != std::string::npos) {
            return prefix->substr(last_slash + 1);
        }
        return *prefix;
    }

private:
    /// Shell-quote a string value
    [[nodiscard]] static std::string shell_quote(std::string_view sv) {
        std::string result = "'";
        for (char c : sv) {
            if (c == '\'') result += "'\\''";
            else result += c;
        }
        result += '\'';
        return result;
    }
};

// ============================================================================
// Shell Tool Utilities
// ============================================================================

/// Utility functions for shell tool operations.
struct ShellToolUtils {
    /// Parse a raw command string into tokens (simplified shell tokenization)
    [[nodiscard]] static std::vector<std::string> tokenize(std::string_view command) {
        std::vector<std::string> tokens;
        std::string current;
        bool in_single_quote = false;
        bool in_double_quote = false;
        bool escaped = false;

        for (size_t i = 0; i < command.size(); i++) {
            char c = command[i];

            if (escaped) {
                current += c;
                escaped = false;
                continue;
            }

            if (c == '\\' && !in_single_quote) {
                escaped = true;
                continue;
            }

            if (c == '\'' && !in_double_quote) {
                in_single_quote = !in_single_quote;
                continue;
            }

            if (c == '"' && !in_single_quote) {
                in_double_quote = !in_double_quote;
                continue;
            }

            if ((c == ' ' || c == '\t') && !in_single_quote && !in_double_quote) {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                continue;
            }

            current += c;
        }

        if (!current.empty()) {
            tokens.push_back(std::move(current));
        }

        return tokens;
    }

    /// Check if a command is a simple single command (no pipes, redirects, etc.)
    [[nodiscard]] static bool is_simple_command(std::string_view command) {
        // Check for shell metacharacters outside quotes
        bool in_single = false, in_double = false;
        for (size_t i = 0; i < command.size(); i++) {
            char c = command[i];
            if (c == '\'' && !in_double) { in_single = !in_single; continue; }
            if (c == '"' && !in_single) { in_double = !in_double; continue; }
            if (in_single || in_double) continue;

            if (c == '|' || c == ';' || c == '&' || c == '`' ||
                c == '$' || c == '(' || c == ')') {
                return false;
            }
            // Check for redirects
            if (c == '>' || c == '<') return false;
        }
        return true;
    }

    /// Extract the base command name from a command string
    [[nodiscard]] static std::string extract_command_name(std::string_view command) {
        auto tokens = tokenize(command);
        if (tokens.empty()) return "";

        // Skip env var assignments (KEY=VALUE)
        for (const auto& token : tokens) {
            if (token.find('=') == std::string::npos || token[0] == '-') {
                // Get basename
                auto slash = token.rfind('/');
                return (slash != std::string::npos)
                    ? token.substr(slash + 1) : token;
            }
        }
        return "";
    }
};

} // namespace cc::utils::shell_tools
