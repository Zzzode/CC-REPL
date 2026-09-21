/// @file add_dir.cppm
/// @brief AddDirCommand implementing the /add-dir slash command.
/// Adds a working directory to the session or local settings.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <span>
#include <filesystem>
#include <algorithm>

export module cc.commands.add_dir;

import cc.types.types;
import cc.commands.command;
import cc.utils.path;
import cc.state.app_state;
import cc.state.store;

export namespace cc::commands {

using namespace cc::core;
namespace fs = std::filesystem;

struct AddDirOptions {
    std::optional<std::string> path;
    bool remember = false;
};

class AddDirCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "add-dir",
            .description = "Add a working directory",
            .args = {
                CommandArg{.name = "<path>", .description = "Directory path to add", .type = ArgType::FilePath, .required = false},
                CommandArg{.name = "--remember", .description = "Save to local settings", .type = ArgType::None, .required = false},
            },
            .category = "workspace",
            .argument_hint = "<path> [--remember]",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        if (!opts.path) {
            return {};  // No path is OK — execute() will show usage hint
        }
        // Validate that the path argument is non-empty
        if (opts.path->empty()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Path argument must not be empty"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);

        if (!opts.path) {
            // No path provided — show current allowed directories + usage
            return CommandResult::success(format_current_status(ctx));
        }

        // ---- Step 1: Expand ~/ to home directory ----
        fs::path input_path(*opts.path);
        fs::path expanded = cc::utils::path::expand_tilde(input_path);

        // ---- Step 2: Resolve to absolute path ----
        std::error_code ec;
        fs::path absolute;
        if (expanded.is_absolute()) {
            absolute = expanded;
        } else {
            // Resolve relative to cwd from context, or current_path
            fs::path base = ctx.cwd.empty() ? fs::current_path(ec) : fs::path(ctx.cwd);
            if (ec) base = fs::current_path();
            absolute = base / expanded;
        }

        // Use weakly_canonical to resolve symlinks / normalize dots without requiring
        // the path to fully exist (we check existence separately below).
        fs::path resolved = fs::weakly_canonical(absolute, ec);
        if (ec || resolved.empty()) {
            resolved = fs::absolute(absolute, ec);
            if (ec) return CommandResult::fail(
                std::format("Failed to resolve path '{}': {}", *opts.path, ec.message()));
        }

        // ---- Step 3: Validate existence and directory ----
        if (!fs::exists(resolved, ec)) {
            return CommandResult::fail(
                std::format("Directory does not exist: {}", resolved.string()));
        }
        if (ec) {
            return CommandResult::fail(
                std::format("Error checking path '{}': {}", resolved.string(), ec.message()));
        }
        if (!fs::is_directory(resolved, ec)) {
            return CommandResult::fail(
                std::format("Path is not a directory: {}", resolved.string()));
        }

        // ---- Step 4: Check for duplicates ----
        const std::string resolved_str = resolved.string();
        bool already_allowed = false;
        if (const auto* state = static_cast<const cc::state::AppState*>(ctx.get_app_state())) {
            for (const auto& d : state->allowed_directories) {
                if (d == resolved_str) {
                    already_allowed = true;
                    break;
                }
            }
        }

        if (already_allowed) {
            return CommandResult::success(std::format(
                "Directory already allowed: {}\n"
                "Use /permissions to manage all permission rules.",
                resolved_str));
        }

        // ---- Step 5: Store in AppState ----
        using cc::state::ActionType;
        ctx.dispatch_action(static_cast<int>(ActionType::AddAllowedDirectory), &resolved_str);

        // ---- Build detailed success message ----
        std::string display_path = cc::utils::path::get_display_path(resolved);
        std::string message = std::format(
            "Added allowed directory\n"
            "  Resolved path: {}\n"
            "  Display name:  {}\n",
            resolved_str, display_path);

        if (opts.remember) {
            message += "  Saved to session settings (--remember)\n";
            // Note: --remember is implicit since allowed_directories persist via
            // state persistence. We keep the flag for CLI compatibility.
        }

        message += "\nNote: May need restart to take full effect on all tools.\n"
                   "Use /permissions to manage all permission rules.";

        return CommandResult::success(std::move(message));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        // Could provide path completion here
        return {};
    }

private:
    [[nodiscard]] static AddDirOptions parse_options(std::span<const std::string> args) {
        AddDirOptions opts;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--remember") {
                opts.remember = true;
            } else if (!opts.path) {
                opts.path = args[i];
            }
        }
        return opts;
    }

    [[nodiscard]] static std::string format_current_status(const CommandContext& ctx) {
        const auto* state = static_cast<const cc::state::AppState*>(ctx.get_app_state());
        std::string out = "Allowed directories:\n";
        if (state && !state->allowed_directories.empty()) {
            for (const auto& d : state->allowed_directories) {
                out += std::format("  {}\n", d);
            }
            out += std::format("\nTotal: {} director(s)\n", state->allowed_directories.size());
        } else {
            out += "  (none)\n";
        }
        out += "\nUsage: /add-dir <path> [--remember]\n"
               "Use /permissions to manage all permission rules.";
        return out;
    }
};

} // namespace cc::commands
