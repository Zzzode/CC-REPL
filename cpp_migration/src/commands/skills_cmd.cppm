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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

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
    std::string path;
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
                           .choices = {"list", "enable", "disable", "info", "reload"}},
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
        ensure_loaded();
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
        ensure_loaded();
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

    void ensure_loaded() {
        if (skills_.empty()) {
            skills_ = load_installed_skills();
        }
    }

    [[nodiscard]] static std::vector<std::filesystem::path> skill_roots() {
        std::vector<std::filesystem::path> roots;
        if (const char* codex_home = std::getenv("CODEX_HOME"); codex_home && *codex_home) {
            roots.emplace_back(std::filesystem::path{codex_home} / "skills");
        }
        if (const char* home = std::getenv("HOME"); home && *home) {
            roots.emplace_back(std::filesystem::path{home} / ".codex" / "skills");
        }
        roots.emplace_back(std::filesystem::current_path() / ".codex" / "skills");
        return roots;
    }

    [[nodiscard]] static std::string trim(std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        const auto last = value.find_last_not_of(" \t\r\n");
        return std::string(value.substr(first, last - first + 1));
    }

    [[nodiscard]] static std::string read_description(const std::filesystem::path& skill_md) {
        std::ifstream in(skill_md);
        if (!in) return {};

        std::string line;
        while (std::getline(in, line)) {
            auto trimmed = trim(line);
            if (trimmed.empty() || trimmed.starts_with("#")) continue;
            return trimmed;
        }
        return {};
    }

    [[nodiscard]] static std::vector<SkillInfo> load_installed_skills() {
        std::vector<SkillInfo> loaded;
        std::unordered_set<std::string> seen;

        for (const auto& root : skill_roots()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) continue;

            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (ec) break;
                if (!entry.is_directory(ec)) continue;

                const auto skill_path = entry.path();
                const auto skill_md = skill_path / "SKILL.md";
                if (!std::filesystem::is_regular_file(skill_md, ec)) continue;

                const auto name = skill_path.filename().string();
                if (!seen.insert(name).second) continue;

                auto description = read_description(skill_md);
                if (description.empty()) description = "Local skill";

                loaded.push_back(SkillInfo{
                    .name = name,
                    .description = description,
                    .version = "local",
                    .enabled = !std::filesystem::exists(skill_path / ".disabled", ec),
                    .bundled = false,
                    .path = skill_path.string(),
                });
            }
        }

        std::ranges::sort(loaded, {}, &SkillInfo::name);
        return loaded;
    }

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
        if (!it->path.empty()) {
            const auto marker = std::filesystem::path{it->path} / ".disabled";
            std::error_code ec;
            if (enabled) {
                std::filesystem::remove(marker, ec);
            } else {
                std::ofstream out(marker);
                if (!out) {
                    return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                        std::format("Failed to write disable marker for skill '{}'.", name)));
                }
            }
            if (ec) {
                return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                    std::format("Failed to update skill '{}': {}", name, ec.message())));
            }
        }
        it->enabled = enabled;
        return CommandResult::success(std::format("Skill '{}' {}.",
            name, enabled ? "enabled" : "disabled"));
    }

    [[nodiscard]] Result<CommandResult> reload_skills() {
        skills_ = load_installed_skills();
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
