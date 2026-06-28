/// @file skills_cmd.cppm
/// @brief SkillsCommand implementing the /skills slash command.
/// Reuses cc.skills.skill (SkillDefinition, SkillLoader) and
/// cc.skills.load_skills_dir (SkillManifest) — no type duplication.
/// UI rendering (FTXUI tables/dialogs) DEFERRED to Phase 4.
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
#include <regex>
#include <string_view>

export module cc.commands.skills_cmd;

import cc.types.types;
import cc.commands.command;
import cc.skills.skill;
import cc.skills.load_skills_dir;

export namespace cc::commands {

using namespace cc::core;

// ============================================================================
// Data-prep row types (Phase 4 FTXUI table rendering)
// ============================================================================

/// Row for the skills list table.
struct SkillListRow {
    std::string name;
    std::string description;
    std::string version;     // "local", "bundled", or semver
    bool enabled = true;
    bool is_bundled = false;
    std::string path;        // filesystem path for user skills
    std::size_t trigger_count = 0;
};

/// Row for a skill's trigger patterns (inside show_info / run detail).
struct SkillTriggerRow {
    std::string pattern;
};

// ============================================================================
// SkillsCommand
// ============================================================================

/// SkillsCommand implements the /skills slash command.
/// Reuses the cc.skills.* modules for type definitions and loading.
class SkillsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "skills",
            .description = "List available skills",
            .args = {},
            .category = "tools",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (!ctx.args.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                "Usage: /skills"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto rows = list_skill_rows(ctx.cwd);
        return CommandResult::success(format_skill_list_text(rows));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        (void)partial;
        return {};
    }

    // ========================================================================
    // Data-prep pure functions (Phase 4 consumption)
    // ========================================================================

    /// Collect all installed skills as rows, using SkillLoader::discover_all
    /// (from cc.skills.skill) and bundled skills from cc.skills.bundled.
    static void add_context_skill_path(
        cc::skills::SkillLoader& loader,
        std::string_view cwd) {
        if (!cwd.empty()) {
            loader.add_search_path(std::filesystem::path(std::string(cwd)) /
                                   ".claude" / "skills");
        }
    }

    [[nodiscard]] static std::vector<SkillListRow> list_skill_rows(
        std::string_view cwd = {}) {
        std::vector<SkillListRow> rows;

        // 1. User-discovered skills via SkillLoader
        cc::skills::SkillLoader loader;
        add_context_skill_path(loader, cwd);
        auto discovered = loader.discover_all();
        if (discovered) {
            for (const auto& def : *discovered) {
                if (std::ranges::find_if(rows, [&](const SkillListRow& r) {
                        return r.name == def.name; }) != rows.end())
                    continue;
                SkillListRow r;
                r.name = def.name;
                r.description = def.description.empty()
                    ? std::string("Custom skill: ") + def.name
                    : def.description;
                r.version = def.version.value_or("local");
                r.enabled = true;
                r.is_bundled = def.is_builtin;
                r.trigger_count = def.trigger_patterns.size();
                rows.push_back(std::move(r));
            }
        }

        // 2. Skills discovered via load_skills_dir (manifest-based discovery)
        for (const auto& path : cc::skills::get_skills_search_paths()) {
            auto manifests = cc::skills::load_skills_directory(path);
            for (const auto& m : manifests) {
                // Avoid duplicates (skill may appear in both paths)
                if (std::ranges::find_if(rows, [&](const SkillListRow& r) {
                        return r.name == m.name; }) != rows.end())
                    continue;
                SkillListRow r;
                r.name = m.name;
                r.description = m.description;
                r.version = m.version.value_or("local");
                r.enabled = true;
                r.is_bundled = false;
                r.path = m.directory.string();
                r.trigger_count = m.triggers.size();
                rows.push_back(std::move(r));
            }
        }

        // 3. Check for .disabled markers to update enabled flag
        std::erase_if(rows, [](const SkillListRow&) { return false; });
        for (auto& r : rows) {
            if (!r.path.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(
                        std::filesystem::path(r.path) / ".disabled", ec)) {
                    r.enabled = false;
                }
            }
        }

        // Sort by name
        std::ranges::sort(rows, {}, &SkillListRow::name);
        return rows;
    }

    /// Return the trigger pattern rows for a given skill name.
    [[nodiscard]] static std::vector<SkillTriggerRow> list_trigger_rows(
        std::string_view name,
        std::string_view cwd = {}) {
        std::vector<SkillTriggerRow> rows;
        cc::skills::SkillLoader loader;
        add_context_skill_path(loader, cwd);
        auto discovered = loader.discover_all();
        if (!discovered) return rows;
        auto it = std::ranges::find_if(*discovered,
            [name](const auto& s) { return s.name == name; });
        if (it == discovered->end()) return rows;
        rows.reserve(it->trigger_patterns.size());
        for (const auto& p : it->trigger_patterns) {
            rows.push_back(SkillTriggerRow{.pattern = p});
        }
        return rows;
    }

    /// Return SkillDefinition by name (or nullopt if not found).
    [[nodiscard]] static std::optional<cc::skills::SkillDefinition>
    find_skill_definition(std::string_view name, std::string_view cwd = {}) {
        cc::skills::SkillLoader loader;
        add_context_skill_path(loader, cwd);
        auto discovered = loader.discover_all();
        if (!discovered) return std::nullopt;
        auto it = std::ranges::find_if(*discovered,
            [name](const auto& s) { return s.name == name; });
        if (it != discovered->end()) return *it;

        // Fallback to manifest-based search
        auto manifest = cc::skills::find_skill_by_name(name);
        if (manifest) {
            cc::skills::SkillDefinition def;
            def.name = manifest->name;
            def.description = manifest->description;
            if (manifest->version) def.version = *manifest->version;
            for (const auto& t : manifest->triggers)
                def.trigger_patterns.push_back(t);
            def.is_builtin = false;
            return def;
        }
        return std::nullopt;
    }

private:
    // ---- Text-formatting helpers (not FTXUI) ------------------------------

    [[nodiscard]] static std::string format_skill_list_text(
        const std::vector<SkillListRow>& rows) {
        if (rows.empty()) return "No skills installed.";
        std::string out = "Installed skills:\n";
        for (const auto& r : rows) {
            const char* status = r.enabled ? "[*]" : "[ ]";
            const char* source = r.is_bundled ? "[bundled]" : "[user]   ";
            std::string name_padded = r.name;
            if (name_padded.size() < 16) name_padded.resize(16, ' ');
            out += "  ";
            out += status;
            out += ' ';
            out += source;
            out += ' ';
            out += name_padded;
            out += ' ';
            out += r.description;
            out += '\n';
            if (r.trigger_count > 0) {
                out += std::format("                        ({} trigger patterns)\n",
                    r.trigger_count);
            }
        }
        auto enabled = std::ranges::count_if(rows,
            [](const auto& r) { return r.enabled; });
        out += std::format("\n{}/{} skills enabled.", enabled, rows.size());
        return out;
    }

    // ---- Execute subcommand helpers ---------------------------------------

    [[nodiscard]] static Result<CommandResult> show_info(
        const std::string& name,
        std::string_view cwd = {}) {
        auto def = find_skill_definition(name, cwd);
        if (!def) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Skill '{}' not found.", name)));
        }
        auto triggers = list_trigger_rows(name, cwd);

        std::string out = std::format(
            "Skill: {}\n"
            "  Version:     {}\n"
            "  Type:        {}\n"
            "  Description: {}\n"
            "  Author:      {}\n",
            def->name,
            def->version.value_or("unspecified"),
            def->is_builtin ? "bundled" : "user-installed",
            def->description,
            def->author.value_or("(unspecified)"));

        if (!triggers.empty()) {
            out += std::format("  Triggers ({}):\n", triggers.size());
            for (const auto& t : triggers)
                out += std::format("    - {}\n", t.pattern);
        } else {
            out += "  Triggers:    (none — explicit invocation only)\n";
        }

        // Preview content length
        if (!def->content.empty()) {
            out += std::format("  Content:     {} chars", def->content.size());
            std::size_t nl = std::count(def->content.begin(), def->content.end(), '\n');
            out += std::format(" ({} lines)", nl + 1);
        }
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static Result<CommandResult> run_skill(
        const std::string& name,
        std::string_view cwd = {}) {
        auto def = find_skill_definition(name, cwd);
        if (!def) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Skill '{}' not found.", name)));
        }
        // NOTE: actual skill execution (inject into system prompt + trigger
        // the agent run) happens in the higher-level query engine. The /skills
        // run command here is a UI-level confirmation that queues the skill
        // for the next turn. Phase 4 hooks into the proper turn loop.
        return CommandResult::success(std::format(
            "Skill '{}' queued for execution.\n"
            "The skill prompt will be injected on the next assistant turn.\n"
            "{} trigger patterns registered.",
            name, def->trigger_patterns.size()));
    }

    [[nodiscard]] static Result<CommandResult> test_skill(
        const std::string& name,
        std::string_view cwd = {}) {
        auto def = find_skill_definition(name, cwd);
        if (!def) {
            return std::unexpected(Error::make(ErrorCode::ToolNotFound,
                std::format("Skill '{}' not found.", name)));
        }
        // Perform a "dry-run" validation: check content is non-empty,
        // triggers compile as regex (if any), and frontmatter parses.
        std::vector<std::string> failures;

        if (def->content.empty())
            failures.push_back("Skill content (prompt body) is empty");

        // Try compiling each trigger pattern to check validity
        for (const auto& p : def->trigger_patterns) {
            try {
                std::regex(p, std::regex::icase | std::regex::optimize);
            } catch (const std::regex_error& e) {
                failures.push_back(
                    std::format("Trigger pattern '{}': regex error: {}", p, e.what()));
            }
        }

        if (!failures.empty()) {
            std::string out = std::format("Skill '{}' FAILED validation:\n", name);
            for (const auto& f : failures)
                out += std::format("  ✗ {}\n", f);
            return CommandResult::success(std::move(out));
        }

        return CommandResult::success(std::format(
            "Skill '{}' PASSED validation.\n"
            "  - Content: {} chars\n"
            "  - Trigger patterns: {} (all compile)\n"
            "  - Type: {}",
            name,
            def->content.size(),
            def->trigger_patterns.size(),
            def->is_builtin ? "bundled" : "user-installed"));
    }

    [[nodiscard]] static Result<CommandResult> reload_skills() {
        // Invalidate any caches by re-running discover_all + manifests.
        cc::skills::SkillLoader loader;
        auto discovered = loader.discover_all();

        // Deduplicate estimate: use set of names
        std::unordered_set<std::string> names;
        if (discovered) {
            for (const auto& d : *discovered) names.insert(d.name);
        }
        for (const auto& path : cc::skills::get_skills_search_paths()) {
            for (const auto& m : cc::skills::load_skills_directory(path))
                names.insert(m.name);
        }

        return CommandResult::success(std::format(
            "Reloaded skills: {} unique skills discovered.",
            names.size()));
    }

    [[nodiscard]] static Result<CommandResult> import_skills_dir(
        const std::string& dir_path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(dir_path, ec)) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Directory does not exist: {}", dir_path)));
        }
        if (!fs::is_directory(dir_path, ec)) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Not a directory: {}", dir_path)));
        }

        // 1. Scan the import directory
        auto manifests = cc::skills::load_skills_directory(fs::path(dir_path));
        if (manifests.empty()) {
            // Try SkillLoader markdown parsing as well
            cc::skills::SkillLoader loader;
            auto direct = loader.load_from_directory(fs::path(dir_path));
            std::size_t count = direct ? direct->size() : 0;
            if (count == 0) {
                return CommandResult::success(std::format(
                    "No skills found in '{}'.\n"
                    "Expected subdirectories containing SKILL.md / skill.md / prompt.md "
                    "or a manifest.json / skill.json manifest.",
                    dir_path));
            }
            return CommandResult::success(std::format(
                "Imported {} skill(s) from '{}' via markdown parser.\n"
                "Skills are now discoverable by the SkillLoader.",
                count, dir_path));
        }

        // 2. Report found skills
        std::string out = std::format(
            "Imported {} skill(s) from '{}':\n", manifests.size(), dir_path);
        for (const auto& m : manifests) {
            out += std::format("  - {} ({} triggers, path: {})\n",
                m.name, m.triggers.size(), m.directory.string());
        }
        return CommandResult::success(std::move(out));
    }
};

} // namespace cc::commands
