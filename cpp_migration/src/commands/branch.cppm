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

export module cc.commands.branch;

import cc.types.types;
import cc.commands.command;

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
                           .choices = {{"list", "create", "switch", "delete"}}},
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

    [[nodiscard]] std::string format_list() const {
        if (branches_.empty()) return "No branches found (not in a git repository?).";

        std::string out = "Branches:\n";
        for (const auto& b : branches_) {
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
        if (std::ranges::any_of(branches_, [&](const auto& b) { return b.name == name; })) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Branch '{}' already exists.", name)));
        }
        // In production: execute git branch <name> via process spawning
        branches_.push_back(BranchInfo{.name = name, .is_current = true});
        return CommandResult::success(std::format("Created and switched to branch: {}", name));
    }

    [[nodiscard]] Result<CommandResult> switch_branch(const std::string& name) {
        auto it = std::ranges::find_if(branches_, [&](const auto& b) { return b.name == name; });
        if (it == branches_.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Branch '{}' not found.", name)));
        }
        // Mark current
        for (auto& b : branches_) b.is_current = false;
        it->is_current = true;
        return CommandResult::success(std::format("Switched to branch: {}", name));
    }

    [[nodiscard]] Result<CommandResult> delete_branch(const std::string& name) {
        auto it = std::ranges::find_if(branches_, [&](const auto& b) { return b.name == name; });
        if (it == branches_.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Branch '{}' not found.", name)));
        }
        if (it->is_current) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Cannot delete the currently checked-out branch."));
        }
        branches_.erase(it);
        return CommandResult::success(std::format("Deleted branch: {}", name));
    }
};

} // namespace cc::commands
