module;
#include <string>
#include <vector>
#include <functional>
#include <expected>
#include <unordered_map>
#include <sstream>

export module cc.vim.vim_commands;

export namespace cc::vim {

// An ex-mode command definition
struct VimCommand {
    std::string name;
    std::function<std::string(std::string_view args)> handler;
    std::string description;
};

namespace detail {
    // Global command registry
    inline std::vector<VimCommand> ex_commands;
    inline bool builtins_registered = false;
}

// Register an ex-mode command
inline auto register_ex_command(VimCommand cmd) -> void {
    detail::ex_commands.push_back(std::move(cmd));
}

// Get all registered ex-mode commands
inline auto get_ex_commands() -> std::vector<VimCommand> {
    return detail::ex_commands;
}

// Register built-in ex-mode commands
inline auto register_builtin_commands() -> void {
    if (detail::builtins_registered) return;
    detail::builtins_registered = true;

    // :w - write/save
    register_ex_command({
        "w",
        [](std::string_view args) -> std::string {
            if (args.empty()) {
                return "Buffer saved";
            }
            return "Saved to: " + std::string(args);
        },
        "Write buffer to file"
    });

    // :q - quit
    register_ex_command({
        "q",
        [](std::string_view) -> std::string {
            return "quit";
        },
        "Quit editor"
    });

    // :wq - write and quit
    register_ex_command({
        "wq",
        [](std::string_view) -> std::string {
            return "write_quit";
        },
        "Write and quit"
    });

    // :set - configure options
    register_ex_command({
        "set",
        [](std::string_view args) -> std::string {
            if (args.empty()) {
                return "Usage: :set <option>[=<value>]";
            }
            return "Set: " + std::string(args);
        },
        "Set editor option"
    });

    // :map - create key mapping
    register_ex_command({
        "map",
        [](std::string_view args) -> std::string {
            if (args.empty()) {
                return "No mappings defined";
            }
            return "Mapped: " + std::string(args);
        },
        "Create key mapping"
    });

    // :help - show help
    register_ex_command({
        "help",
        [](std::string_view args) -> std::string {
            if (args.empty()) {
                std::ostringstream out;
                out << "Available commands:\n";
                for (const auto& cmd : detail::ex_commands) {
                    out << "  :" << cmd.name << " - " << cmd.description << "\n";
                }
                return out.str();
            }
            // Search for specific command help
            for (const auto& cmd : detail::ex_commands) {
                if (cmd.name == args) {
                    return ":" + cmd.name + " - " + cmd.description;
                }
            }
            return "No help for: " + std::string(args);
        },
        "Show help information"
    });

    // :noh - clear search highlighting
    register_ex_command({
        "noh",
        [](std::string_view) -> std::string {
            return "clear_highlight";
        },
        "Clear search highlighting"
    });

    // :number / :nu - toggle line numbers
    register_ex_command({
        "number",
        [](std::string_view) -> std::string {
            return "toggle_line_numbers";
        },
        "Toggle line numbers"
    });
}

// Execute an ex-mode command string (e.g., "w filename.txt")
inline auto execute_ex_command(std::string_view command)
    -> std::expected<std::string, std::string> {
    // Ensure builtins are registered
    register_builtin_commands();

    if (command.empty()) {
        return std::unexpected("Empty command");
    }

    // Parse command name and arguments
    size_t space_pos = command.find(' ');
    std::string_view cmd_name;
    std::string_view cmd_args;

    if (space_pos == std::string_view::npos) {
        cmd_name = command;
    } else {
        cmd_name = command.substr(0, space_pos);
        cmd_args = command.substr(space_pos + 1);
    }

    // Handle force variants (e.g., :q!, :w!)
    bool force = false;
    if (!cmd_name.empty() && cmd_name.back() == '!') {
        force = true;
        cmd_name = cmd_name.substr(0, cmd_name.size() - 1);
    }

    // Look up and execute the command
    for (const auto& ex_cmd : detail::ex_commands) {
        if (ex_cmd.name == cmd_name) {
            std::string result = ex_cmd.handler(cmd_args);
            if (force) {
                result += " (forced)";
            }
            return result;
        }
    }

    return std::unexpected("Unknown command: " + std::string(cmd_name));
}

} // namespace cc::vim
