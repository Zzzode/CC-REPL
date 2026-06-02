/// @file command.cppm
/// @brief Command type definitions.
/// Migrated from src/types/command.ts
module;

#include <string>
#include <vector>
#include <optional>
#include <functional>

export module cc.types.command;

export namespace cc::types {

/// Slash command argument type
enum class ArgType : std::uint8_t {
    String,
    Number,
    Boolean,
    Choice,
};

/// Slash command argument definition
struct CommandArg {
    std::string name;
    ArgType type = ArgType::String;
    std::string description;
    bool required = false;
    std::optional<std::string> default_value;
    std::vector<std::string> choices;  // For ArgType::Choice
};

/// Parsed command invocation
struct ParsedCommand {
    std::string name;                               // Command name without '/'
    std::vector<std::string> positional_args;       // Positional arguments
    std::vector<std::pair<std::string, std::string>> named_args;  // --key=value pairs
    std::string raw_args;                           // Everything after the command name
};

/// Parse a raw command string
[[nodiscard]] inline ParsedCommand parse_command(std::string_view input) {
    ParsedCommand result;
    
    // Strip leading '/'
    if (!input.empty() && input[0] == '/') {
        input.remove_prefix(1);
    }
    
    // Extract command name
    auto space_pos = input.find(' ');
    if (space_pos == std::string_view::npos) {
        result.name = std::string(input);
        return result;
    }
    
    result.name = std::string(input.substr(0, space_pos));
    result.raw_args = std::string(input.substr(space_pos + 1));
    
    // Simple arg splitting (production version would handle quotes)
    auto remaining = input.substr(space_pos + 1);
    while (!remaining.empty()) {
        // Skip whitespace
        auto start = remaining.find_first_not_of(' ');
        if (start == std::string_view::npos) break;
        remaining.remove_prefix(start);
        
        // Find end of argument
        auto end = remaining.find(' ');
        auto arg = (end == std::string_view::npos) ? remaining : remaining.substr(0, end);
        
        if (arg.starts_with("--")) {
            auto eq = arg.find('=');
            if (eq != std::string_view::npos) {
                result.named_args.emplace_back(
                    std::string(arg.substr(2, eq - 2)),
                    std::string(arg.substr(eq + 1))
                );
            } else {
                result.named_args.emplace_back(std::string(arg.substr(2)), "true");
            }
        } else {
            result.positional_args.emplace_back(arg);
        }
        
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    
    return result;
}

} // namespace cc::types
