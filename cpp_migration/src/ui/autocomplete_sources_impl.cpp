/// @file autocomplete_sources_impl.cpp
/// @brief Heavy autocomplete source implementations kept out of app.cppm.
module;

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

module cc.ui.autocomplete_sources;

import cc.skills.skill;
import cc.skills.load_skills_dir;
import cc.skills.bundled;
import cc.tools.agent_runtime;
import cc.tools.mcp;
import cc.utils.json;

namespace cc::ui::autocomplete_sources {

namespace fs = std::filesystem;
namespace agent_runtime = cc::tools::agent_runtime;

inline void add_unique_skill(
    std::vector<SkillSuggestionData>& rows,
    std::unordered_set<std::string>& seen,
    const cc::skills::SkillDefinition& def,
    std::string source,
    std::string source_detail = {}) {
    if (def.name.empty() || seen.contains(def.name)) return;
    seen.insert(def.name);
    const auto token_estimate =
        static_cast<std::size_t>(std::max<std::size_t>(1, def.content.size() / 4));
    rows.push_back(SkillSuggestionData{
        .name = def.name,
        .description = def.description.empty()
            ? std::string("Skill: ") + def.name
            : def.description,
        .source = std::move(source),
        .source_detail = std::move(source_detail),
        .kind = def.kind.value_or(""),
        .token_estimate = token_estimate,
        .enabled = true,
        .user_invocable = def.user_invocable,
        .content = def.content,
    });
}

std::vector<SkillSuggestionData> collect_skill_suggestions(std::string_view cwd) {
    std::vector<SkillSuggestionData> rows;
    std::unordered_set<std::string> seen;

    // Use the unified SkillRegistry which merges bundled + statically-loaded
    // (managed/user/project/additional-dir/legacy-commands) + dynamically-
    // discovered skills from get_skill_dir_commands() + get_dynamic_skills().
    // TS REF: src/ui/components/Autocomplete.tsx uses getSkillDirCommands()
    //          + bundled skills for the unified skill picker.
    auto& registry = cc::skills::SkillRegistry::instance();
    auto all_skills = registry.all_skills(fs::path(std::string(cwd)));

    // Determine source label per skill
    for (const auto& def : all_skills) {
        std::string source = def.is_builtin ? "bundled" : "project";
        std::string detail;
        if (def.is_builtin) {
            detail = "bundled";
        } else {
            // Heuristic: if the skill has no author, it's likely from a
            // project/user directory.  The exact source is tracked in
            // SkillCommand.source but we lost that in the conversion.
            detail = "discovered";
        }
        add_unique_skill(rows, seen, def, source, detail);
    }

    // Also load plugin skills with prefix (SkillRegistry doesn't yet
    // handle plugin-prefixed skills, so we keep the direct loader path).
    cc::skills::SkillLoader loader;
    for (const auto& plugin : agent_runtime::discover_plugin_component_paths()) {
        for (const auto& path : plugin.skills_paths) {
            if (auto plugin_skills =
                    loader.load_from_directory_with_prefix(path, plugin.plugin_name);
                plugin_skills) {
                for (const auto& def : *plugin_skills) {
                    add_unique_skill(rows, seen, def, "plugin", plugin.plugin_name);
                }
            }
        }
    }

    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        auto order = [](std::string_view source) {
            if (source == "project") return 0;
            if (source == "user") return 1;
            if (source == "plugin") return 2;
            if (source == "bundled") return 3;
            return 4;
        };
        const int ao = order(a.source);
        const int bo = order(b.source);
        if (ao != bo) return ao < bo;
        return a.name < b.name;
    });
    return rows;
}

std::optional<SkillSuggestionData> find_skill_suggestion(
    std::string_view cwd,
    std::string_view name) {
    auto rows = collect_skill_suggestions(cwd);
    auto it = std::ranges::find_if(rows, [name](const auto& row) {
        return row.name == name;
    });
    if (it == rows.end()) return std::nullopt;
    return *it;
}

std::string skill_invocation_prompt(
    const SkillSuggestionData& skill,
    std::string_view user_text) {
    // SL-12: inline-substitute the $ARGUMENTS placeholder with the user's args,
    // faithful to TS skill invocation — skill content declares $ARGUMENTS where
    // the user's input should flow in. When present, the args are inlined and
    // no separate "User request" block is appended.
    std::string content = skill.content;
    bool has_placeholder = false;
    const std::string args(user_text);
    for (std::size_t pos = 0;
         (pos = content.find("$ARGUMENTS", pos)) != std::string::npos;) {
        has_placeholder = true;
        content.replace(pos, std::string_view("$ARGUMENTS").size(), args);
        pos += args.size();
    }

    std::string prompt = std::format(
        "<selected_skill name=\"{}\" source=\"{}\">\n{}\n</selected_skill>\n",
        skill.name,
        skill.source,
        content);
    if (has_placeholder) {
        if (args.empty()) prompt += "\nApply this skill to the current task.";
    } else if (!args.empty()) {
        prompt += "\nUser request:\n";
        prompt += args;
    } else {
        prompt += "\nApply this skill to the current task.";
    }
    return prompt;
}

std::vector<PluginCommandSuggestionData> collect_plugin_commands(std::string_view cwd) {
    std::vector<PluginCommandSuggestionData> out;
    std::unordered_set<std::string> seen;
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(fs::path(home) / ".claude" / "plugins");
    }
    if (!cwd.empty()) {
        roots.push_back(fs::path(std::string(cwd)) / ".claude" / "plugins");
    }

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            const auto manifest_path = entry.path() / "plugin.json";
            if (!fs::is_regular_file(manifest_path, ec)) continue;

            std::ifstream input(manifest_path);
            if (!input) continue;
            std::stringstream buffer;
            buffer << input.rdbuf();
            auto parsed = cc::utils::json::parse(buffer.str());
            if (!parsed) continue;

            auto root_node = parsed->root();
            auto name_node = root_node.get("name");
            const std::string plugin_name =
                name_node.is_str() ? std::string(name_node.as_str())
                                   : entry.path().filename().string();
            auto caps = root_node.get("capabilities");
            if (!caps.valid() || !caps.is_obj()) continue;
            auto commands = caps.get("commands");
            if (!commands.valid() || !commands.is_arr()) continue;
            commands.iter([&](cc::utils::json::JsonVal item) {
                if (!item.is_str()) return;
                std::string command(item.as_str());
                if (command.starts_with('/')) command.erase(command.begin());
                if (command.empty() || seen.contains(command)) return;
                seen.insert(command);
                out.push_back(PluginCommandSuggestionData{
                    .command = std::move(command),
                    .plugin_name = plugin_name,
                });
            });
        }
    }

    std::ranges::sort(out, {}, &PluginCommandSuggestionData::command);
    return out;
}

std::vector<McpResourceSuggestionData> collect_mcp_resource_suggestions() {
    // INF-04: TTL cache — list_native_mcp_resources crosses every connected MCP
    // server, so cache the result for 2s to avoid recomputing on every keystroke.
    // The FTXUI event loop is synchronous (a true async debounce needs reworking
    // it); a short TTL cuts the bulk of redundant cross-server work per typing
    // burst, which is the main perceivable latency source.
    static std::vector<McpResourceSuggestionData> cached;
    static std::chrono::steady_clock::time_point last{};
    static std::mutex mu;
    {
        std::lock_guard lk(mu);
        const auto now = std::chrono::steady_clock::now();
        if (!cached.empty() && now - last < std::chrono::seconds(2)) return cached;
    }

    std::vector<McpResourceSuggestionData> out;
    auto resources = cc::tools::list_native_mcp_resources(std::nullopt);
    if (!resources) return out;

    for (const auto& resource : *resources) {
        const bool channel_like =
            resource.name.starts_with("#") ||
            resource.uri.find("channel") != std::string::npos ||
            resource.uri.find("slack") != std::string::npos;
        out.push_back(McpResourceSuggestionData{
            .display = resource.name,
            .description = resource.description.value_or(
                channel_like ? "MCP channel" : "MCP resource"),
            .insert_text = std::format("{}:{}", resource.server_name, resource.uri),
            .server_name = resource.server_name,
            .channel_like = channel_like,
        });
    }
    {
        std::lock_guard lk(mu);
        cached = out;
        last = std::chrono::steady_clock::now();
    }
    return out;
}

} // namespace cc::ui::autocomplete_sources
