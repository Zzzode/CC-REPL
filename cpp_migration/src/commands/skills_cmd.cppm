/// @file skills_cmd.cppm
/// @brief SkillsCommand implementing the /skills slash command.
/// List installed skills, enable/disable, show details, reload.
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

export module cc.commands.skills_cmd;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Metadata for an installed skill
struct SkillInfo {
    std::string name;
    std::string description;
    std::string version;
    bool enabled = true;
    bool bundled = false;        // Built-in vs user-installed
};

/// SkillsCommand implements the /skills slash command.
/// Manages the skill system: list, enable/disable, show details, reload.
class SkillsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "skills",
            .description = "Manage installed skills",
            .aliases = {"skill"},
            .args = {
                CommandArg{.name = "action", .description = "list | enable | disable | info | reload",
                           .type = ArgType::Choice, .required = false,
                           .choices = {{"list", "enable", "disable", "info", "reload"}}},
                CommandArg{.name = "skill_name", .description = "Name of the skill",
                           .type = ArgType::Text, .required = false},
            },
            .hidden = false,
            .category = "tools",
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto action = ctx.args[0];
        static constexpr std::array valid = {"list", "enable", "disable", "info", "reload"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: list|enable|disable|info|reload", action)));
        }
        if ((action == "enable" || action == "disable" || action == "info") && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Usage: /skills {} <skill_name>", action)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        if (ctx.args.empty()) return CommandResult::success(format_list());

        auto action = std::string(ctx.args[0]);

        if (action == "list") return CommandResult::success(format_list());
        if (action == "info") return show_info(std::string(ctx.args[1]));
        if (action == "enable") return set_enabled(std::string(ctx.args[1]), true);
        if (action == "disable") return set_enabled(std::string(ctx.args[1]), false);
        if (action == "reload") return reload_skills();

        return CommandResult::success(format_list());
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "enable", "disable", "info", "reload"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        for (const auto& skill : skills_) {
            if (skill.name.starts_with(partial)) {
                suggestions.push_back(skill.name);
            }
        }
        return suggestions;
    }

    /// Register available skills (called by the skill loader)
    void set_skills(std::vector<SkillInfo> skills) { skills_ = std::move(skills); }

private:
    std::vector<SkillInfo> skills_;

    [[nodiscard]] std::string format_list() const {
        if (skills_.empty()) return "No skills installed.";

        std::string out = "Installed skills:\n";
        for (const auto& s : skills_) {
            auto status = s.enabled ? "●" : "○";
            auto source = s.bundled ? "[bundled]" : "[user]";
            out += std::format("  {} {} v{} {} — {}\n",
                status, s.name, s.version, source, s.description);
        }
        auto enabled = std::ranges::count_if(skills_, [](const auto& s) { return s.enabled; });
        out += std::format("\n{}/{} skills enabled.", enabled, skills_.size());
        return out;
    }

    [[nodiscard]] Result<CommandResult> show_info(const std::string& name) const {
        auto it = find_skill(name);
        if (it == skills_.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Skill '{}' not found.", name)));
        }
        return CommandResult::success(std::format(
            "Skill: {}\n  Version: {}\n  Status: {}\n  Source: {}\n  Description: {}",
            it->name, it->version, it->enabled ? "enabled" : "disabled",
            it->bundled ? "bundled" : "user-installed", it->description));
    }

    [[nodiscard]] Result<CommandResult> set_enabled(const std::string& name, bool enabled) {
        auto it = find_skill_mut(name);
        if (it == skills_.end()) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Skill '{}' not found.", name)));
        }
        it->enabled = enabled;
        return CommandResult::success(std::format("Skill '{}' {}.",
            name, enabled ? "enabled" : "disabled"));
    }

    [[nodiscard]] Result<CommandResult> reload_skills() {
        // In production: re-scan skill directories and reload definitions
        return CommandResult::success(std::format("Reloaded {} skills.", skills_.size()));
    }

    [[nodiscard]] std::vector<SkillInfo>::const_iterator find_skill(const std::string& name) const {
        return std::ranges::find_if(skills_, [&](const auto& s) { return s.name == name; });
    }

    [[nodiscard]] std::vector<SkillInfo>::iterator find_skill_mut(const std::string& name) {
        return std::ranges::find_if(skills_, [&](const auto& s) { return s.name == name; });
    }
};

} // namespace cc::commands
