/// @file branch.cppm
/// @brief BranchCommand implementing the /branch slash command.
/// Create, switch, list, and delete git branches.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <sstream>

export module cc.commands.branch;

import cc.types.types;
import cc.commands.command;
import cc.utils.bash_execution;

export namespace cc::commands {

using namespace cc::core;

/// Information about a git branch
struct BranchInfo {
    std::string name;
    bool is_current = false;
    std::optional<std::string> upstream;
    std::optional<std::string> last_commit_summary;
};

/// BranchCommand implements the /branch slash command.
/// Manages git branches: create, switch, list, delete.
class BranchCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "branch",
            .description = "Manage git branches",
            .aliases = {"br"},
            .args = {
                CommandArg{.name = "action", .description = "list | create | switch | delete",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"list", "create", "switch", "delete"}},
                CommandArg{.name = "branch_name", .description = "Branch name",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "git",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "create", "switch", "delete"};
        if (std::ranges::find(valid, action) == valid.end()) {
            // Could be a branch name for quick switch
            return {};
        }
        if ((action == "create" || action == "switch" || action == "delete") && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /branch {} <branch_name>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_list());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_list());
        if (action == "create") return create_branch(std::string(ctx.args[1]));
        if (action == "switch") return switch_branch(std::string(ctx.args[1]));
        if (action == "delete") return delete_branch(std::string(ctx.args[1]));

        // Quick switch: /branch feature-xyz
        return switch_branch(action);
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "create", "switch", "delete"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& b : branches_) {
            if (b.name.starts_with(partial)) {
                suggestions.push_back(b.name);
            }
        }
        return suggestions;
    }

    /// Set branch list (populated via git operations)
    void set_branches(std::vector<BranchInfo> branches) { branches_ = std::move(branches); }

private:
    std::vector<BranchInfo> branches_;

    [[nodiscard]] static Result<CommandResult> git_command(std::string_view command, std::string_view success) {
        auto result = cc::utils::bash::execute_command(command);
        if (!result) {
            return std::unexpected(Error::make(ErrorCode::InternalError, result.error()));
        }
        if (result->exit_code != 0) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("git command failed: {}", result->stdout_output)));
        }
        return CommandResult::success(std::string(success));
    }

    [[nodiscard]] static std::vector<BranchInfo> read_git_branches() {
        std::vector<BranchInfo> branches;
        auto result = cc::utils::bash::execute_command(
            "git branch --format='%(HEAD)%09%(refname:short)%09%(upstream:short)%09%(subject)'");
        if (!result || result->exit_code != 0) return branches;

        std::istringstream lines(result->stdout_output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            std::vector<std::string> parts;
            std::string part;
            std::istringstream fields(line);
            while (std::getline(fields, part, '\t')) parts.push_back(part);
            if (parts.size() < 2 || parts[1].empty()) continue;
            BranchInfo info;
            info.is_current = parts[0] == "*";
            info.name = parts[1];
            if (parts.size() > 2 && !parts[2].empty()) info.upstream = parts[2];
            if (parts.size() > 3 && !parts[3].empty()) info.last_commit_summary = parts[3];
            branches.push_back(std::move(info));
        }
        return branches;
    }

    [[nodiscard]] std::string format_list() const {
        auto live_branches = read_git_branches();
        const auto& branches = live_branches.empty() ? branches_ : live_branches;
        if (branches.empty()) return "No branches found (not in a git repository?).";

        std::string out = "Branches:\n";
        for (const auto& b : branches) {
            auto marker = b.is_current ? "* " : "  ";
            out += std::format("{}{}", marker, b.name);
            if (b.upstream) out += std::format(" → {}", *b.upstream);
            if (b.last_commit_summary) out += std::format(" ({})", *b.last_commit_summary);
            out += "\n";
        }
        return out;
    }

    [[nodiscard]] Result<CommandResult> create_branch(const std::string& name) {
        // Validate branch name
        if (name.empty() || name.find(' ') != std::string::npos) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid branch name: '{}'", name)));
        }
        auto live_branches = read_git_branches();
        if (std::ranges::any_of(live_branches, [&](const auto& b) { return b.name == name; }) ||
            std::ranges::any_of(branches_, [&](const auto& b) { return b.name == name; })) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Branch '{}' already exists.", name)));
        }
        auto quoted = cc::utils::bash::escape_shell_arg(name);
        auto command = std::format("git checkout -b {}", quoted);
        auto result = git_command(command, std::format("Created and switched to branch: {}", name));
        if (result) branches_ = read_git_branches();
        return result;
    }

    [[nodiscard]] Result<CommandResult> switch_branch(const std::string& name) {
        auto quoted = cc::utils::bash::escape_shell_arg(name);
        auto result = git_command(
            std::format("git checkout {}", quoted),
            std::format("Switched to branch: {}", name));
        if (result) branches_ = read_git_branches();
        return result;
    }

    [[nodiscard]] Result<CommandResult> delete_branch(const std::string& name) {
        auto quoted = cc::utils::bash::escape_shell_arg(name);
        auto result = git_command(
            std::format("git branch -d {}", quoted),
            std::format("Deleted branch: {}", name));
        if (result) branches_ = read_git_branches();
        return result;
    }
};

} // namespace cc::commands
