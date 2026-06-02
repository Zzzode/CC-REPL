module;
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.immediate_command;

export namespace cc::utils {

// List of commands that can be handled immediately without LLM round-trip
bool is_immediate_command(std::string_view input) {
    // Trim leading whitespace
    auto start = input.find_first_not_of(" \t");
    if (start == std::string_view::npos) return false;
    input = input.substr(start);

    // Must start with /
    if (!input.starts_with("/")) return false;

    // Extract command name
    auto space = input.find(' ', 1);
    std::string_view cmd = (space != std::string_view::npos) ? input.substr(0, space) : input;

    return cmd == "/exit" || cmd == "/quit" || cmd == "/q" ||
           cmd == "/clear" || cmd == "/help" || cmd == "/?" ||
           cmd == "/version" || cmd == "/compact" ||
           cmd == "/model" || cmd == "/cost" || cmd == "/status";
}

// Execute an immediate command and return the result
std::optional<std::string> execute_immediate(std::string_view command) {
    // Trim and extract command
    auto start = command.find_first_not_of(" \t");
    if (start == std::string_view::npos) return std::nullopt;
    command = command.substr(start);

    if (!command.starts_with("/")) return std::nullopt;

    auto space = command.find(' ', 1);
    std::string_view cmd = (space != std::string_view::npos) ? command.substr(0, space) : command;

    if (cmd == "/exit" || cmd == "/quit" || cmd == "/q") {
        return "exit";
    }

    if (cmd == "/clear") {
        return "\033[2J\033[H"; // ANSI clear screen + cursor home
    }

    if (cmd == "/help" || cmd == "/?") {
        return "Available commands:\n"
               "  /help, /?       - Show this help\n"
               "  /exit, /quit    - Exit the session\n"
               "  /clear          - Clear the terminal\n"
               "  /compact        - Compact conversation context\n"
               "  /model          - Show/change current model\n"
               "  /cost           - Show session cost\n"
               "  /status         - Show session status\n"
               "  /version        - Show version info\n";
    }

    if (cmd == "/version") {
        return "CC-REPL v0.1.0 (C++23)";
    }

    if (cmd == "/status") {
        return "Session: active\nModel: claude-sonnet-4-20250514\nStatus: ready";
    }

    if (cmd == "/cost") {
        return "Session cost: $0.00 (0 requests)";
    }

    if (cmd == "/compact") {
        return "compact"; // Signal to compact context
    }

    if (cmd == "/model") {
        // If just /model, show current
        if (space == std::string_view::npos) {
            return "Current model: claude-sonnet-4-20250514";
        }
        // Otherwise, it's a model switch request
        return std::nullopt; // Let the LLM handle model switching logic
    }

    return std::nullopt;
}

} // namespace cc::utils
