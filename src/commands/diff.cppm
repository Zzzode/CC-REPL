/// @file diff.cppm
/// @brief DiffCommand implementing the /diff slash command.
/// Show all file changes in current session, colored diff output, summary mode.
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

export module cc.commands.diff;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Represents a single file change in the session
struct FileChange {
    std::string path;
    std::uint32_t additions = 0;
    std::uint32_t deletions = 0;
    std::string diff_content;   // Unified diff text
    bool is_new = false;
    bool is_deleted = false;
};

/// DiffCommand implements the /diff slash command.
/// Shows file changes made during the current session.
class DiffCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "diff",
            .description = "Show file changes made in the current session",
            .args = {
                CommandArg{.name = "mode", .description = "full | summary | stat",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"full", "summary", "stat"}},
                CommandArg{.name = "file", .description = "Specific file to show diff for",
                           .type = ArgType::FilePath, .required = false},
            },
            .category = "git",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto mode = ctx.args[0];
        if (mode != "full" && mode != "summary" && mode != "stat") {
            // Might be a file path instead
            if (!mode.starts_with("-")) return {};
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid mode: '{}'. Use: full|summary|stat", mode)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (changes_.empty()) {
            return CommandResult::success("No file changes in this session.");
        }

        auto mode = std::string("full");
        std::optional<std::string> file_filter;

        if (!ctx.args.empty()) {
            if (ctx.args[0] == "full" || ctx.args[0] == "summary" || ctx.args[0] == "stat") {
                mode = std::string(ctx.args[0]);
                if (ctx.args.size() > 1) file_filter = std::string(ctx.args[1]);
            } else {
                file_filter = std::string(ctx.args[0]);
            }
        }

        if (mode == "stat") return CommandResult::success(format_stat(file_filter));
        if (mode == "summary") return CommandResult::success(format_summary(file_filter));
        return CommandResult::success(format_full(file_filter));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"full", "summary", "stat"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        // Suggest changed file paths
        for (const auto& c : changes_) {
            if (c.path.starts_with(partial)) {
                suggestions.push_back(c.path);
            }
        }
        return suggestions;
    }

    /// Record a file change (called by the tool system after file writes)
    void record_change(FileChange change) {
        // Update existing entry or add new
        auto it = std::ranges::find_if(changes_,
            [&](const auto& c) { return c.path == change.path; });
        if (it != changes_.end()) {
            *it = std::move(change);
        } else {
            changes_.push_back(std::move(change));
        }
    }

private:
    std::vector<FileChange> changes_;

    [[nodiscard]] std::string format_stat(const std::optional<std::string>& filter) const {
        std::string out = std::format("{} file(s) changed:\n", changes_.size());
        std::uint32_t total_add = 0, total_del = 0;
        for (const auto& c : changes_) {
            if (filter && c.path != *filter) continue;
            out += std::format("  {} | +{} -{}\n", c.path, c.additions, c.deletions);
            total_add += c.additions;
            total_del += c.deletions;
        }
        out += std::format("  Total: +{} -{}", total_add, total_del);
        return out;
    }

    [[nodiscard]] std::string format_summary(const std::optional<std::string>& filter) const {
        std::string out = "Changes summary:\n";
        for (const auto& c : changes_) {
            if (filter && c.path != *filter) continue;
            auto status = c.is_new ? "[new]" : c.is_deleted ? "[deleted]" : "[modified]";
            out += std::format("  {} {} (+{} -{})\n", status, c.path, c.additions, c.deletions);
        }
        return out;
    }

    [[nodiscard]] std::string format_full(const std::optional<std::string>& filter) const {
        std::string out;
        for (const auto& c : changes_) {
            if (filter && c.path != *filter) continue;
            out += std::format("--- a/{}\n+++ b/{}\n", c.path, c.path);
            out += c.diff_content;
            out += "\n";
        }
        if (out.empty()) {
            out = filter ? std::format("No changes for file: {}", *filter) : "No changes.";
        }
        return out;
    }
};

} // namespace cc::commands
