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
import cc.ui.prompt.fuzzy_rank_nucleo;
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

// ============================================================
// Agent / teammate autocomplete source
// ============================================================
// TS REF: src/hooks/unifiedSuggestions.ts:77-108 — generateAgentSuggestions()
//   builds AgentSuggestionSource[] from AgentDefinition[] with color +
//   truncated whenToUse description.  Filtered by case-insensitive substring
//   match on agentType or displayText.
// TS REF: src/hooks/useTypeahead.tsx:604-625 — DM teammate suggestions from
//   state.teamContext.teammates + state.agentNameRegistry, prefix-matched on
//   lowercased name, with status appended to description.
std::vector<AgentSuggestionData> collect_agent_suggestions(std::string_view cwd) {
    std::vector<AgentSuggestionData> result;
    std::unordered_set<std::string> seen_names;

    std::optional<fs::path> cwd_path;
    if (!cwd.empty()) cwd_path = fs::path(std::string(cwd));

    // (1) Agent definitions (built-in, plugin, user, project, local, flag, policy)
    for (const auto& agent : agent_runtime::get_all_agent_definitions(cwd_path)) {
        if (agent.agent_type.empty()) continue;
        if (!seen_names.insert(agent.agent_type).second) continue;

        // Truncate description to ~60 chars, matching TS truncateDescription.
        std::string desc = agent.when_to_use;
        if (desc.size() > 60) {
            desc = desc.substr(0, 57) + "...";
        }

        result.push_back(AgentSuggestionData{
            .name        = agent.agent_type,
            .description = std::move(desc),
            .source      = "agent",
            .color       = agent.color,
            .status      = std::nullopt,
            .is_subagent = false,
        });
    }

    // (2) Native agent records (live teammates / named sub-agents)
    // TS REF: useTypeahead.tsx:616-625 — named agents from agentNameRegistry
    //   show "send message · <status>" description.
    for (const auto& record : agent_runtime::load_all_native_agent_records()) {
        auto name = record.name.value_or(record.agent_id);
        if (name.empty()) continue;
        if (!seen_names.insert(name).second) continue;

        const std::string status =
            std::string(agent_runtime::native_agent_status_name(record.status));
        std::string desc = "send message";
        if (!status.empty() && status != "unknown") {
            desc += " · " + status;
        }

        result.push_back(AgentSuggestionData{
            .name        = std::move(name),
            .description = std::move(desc),
            .source      = "teammate",
            .color       = record.teammate_color,
            .status      = status,
            .is_subagent = true,
        });
    }

    // Sort: teammates first (they're the DM priority per TS useTypeahead.tsx
    // AT-05 bare-@ teammate exclusivity), then alphabetically by name.
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        if (a.is_subagent != b.is_subagent) return a.is_subagent > b.is_subagent;
        return a.name < b.name;
    });

    return result;
}

// ============================================================
// Prompt history persistence (for @history / Ctrl+R autocomplete)
// ============================================================
namespace {

// TS REF: src/history.ts:115 — history stored at {getClaudeConfigHomeDir()}/history.jsonl
//   We use ~/.cc-repl/history.jsonl to match the CPP migration's data layout
//   (sessions/, dump-prompts/ etc. already live under ~/.cc-repl/).
[[nodiscard]] fs::path prompt_history_file_path() {
    if (const char* env = std::getenv("CC_REPL_HISTORY_FILE"); env && *env) {
        return fs::path{env};
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path{home} / ".cc-repl" / "history.jsonl";
    }
    return fs::path{".cc-repl"} / "history.jsonl";
}

// TS REF: src/history.ts:219-225 — LogEntry { display, pastedContents, timestamp,
//   project, sessionId }.  We keep a minimal subset for the autocomplete source.
struct PromptHistoryEntry {
    std::string display;       // first line of prompt (for display)
    std::string full_text;     // complete prompt text
    std::int64_t timestamp_ms = 0;
    std::string session_id;
    std::string project;
};

// Parse a single JSONL line into a PromptHistoryEntry.
// Returns nullopt on parse failure.
[[nodiscard]] std::optional<PromptHistoryEntry> parse_history_line(std::string_view line) {
    auto parsed = cc::utils::json::parse(line);
    if (!parsed) return std::nullopt;
    auto root = parsed->root();
    if (!root.is_obj()) return std::nullopt;

    PromptHistoryEntry entry;
    auto display_node = root.get("display");
    if (display_node.is_str()) {
        entry.display = std::string(display_node.as_str());
    } else {
        // Fallback: use "text" field if present
        auto text_node = root.get("text");
        if (text_node.is_str()) entry.display = std::string(text_node.as_str());
    }
    if (entry.display.empty()) return std::nullopt;

    auto full_node = root.get("full_text");
    if (full_node.is_str()) {
        entry.full_text = std::string(full_node.as_str());
    } else {
        entry.full_text = entry.display;
    }

    auto ts_node = root.get("timestamp");
    if (ts_node.is_num()) {
        entry.timestamp_ms = static_cast<std::int64_t>(ts_node.as_int());
    }

    auto sess_node = root.get("sessionId");
    if (sess_node.is_str()) entry.session_id = std::string(sess_node.as_str());

    auto proj_node = root.get("project");
    if (proj_node.is_str()) entry.project = std::string(proj_node.as_str());

    return entry;
}

// Read the history JSONL file in reverse (newest-first), yielding parsed
// entries.  Faithful to TS readLinesReverse() pattern.
[[nodiscard]] std::vector<PromptHistoryEntry> read_history_recent(std::size_t max_entries) {
    const auto path = prompt_history_file_path();
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) return {};

    // TS REF: src/utils/fsOperations.ts:722 — readLinesReverse reads in 4KB
    // chunks from the end.  For simplicity we read the whole file and reverse
    // iterate; prompt history is capped at a few KB so this is fine.
    std::ifstream ifs(path);
    if (!ifs) return {};

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) lines.push_back(std::move(line));
    }

    std::vector<PromptHistoryEntry> entries;
    entries.reserve(std::min(max_entries, lines.size()));

    // Iterate from newest (last line) to oldest.
    for (auto it = lines.rbegin(); it != lines.rend() && entries.size() < max_entries; ++it) {
        if (auto entry = parse_history_line(*it)) {
            entries.push_back(std::move(*entry));
        }
    }
    return entries;
}

} // anonymous namespace

void append_prompt_history(
    std::string_view prompt_text,
    std::string_view session_id,
    std::string_view project_path)
{
    if (prompt_text.empty()) return;

    const auto path = prompt_history_file_path();
    std::error_code ec;
    auto dir = path.parent_path();
    if (!dir.empty()) {
        fs::create_directories(dir, ec);
    }

    // Build first-line display (truncate at first newline or 200 chars).
    std::string display;
    const auto nl = prompt_text.find('\n');
    if (nl != std::string_view::npos) {
        display = std::string(prompt_text.substr(0, nl));
    } else {
        display = std::string(prompt_text);
    }
    if (display.size() > 200) {
        display = display.substr(0, 197) + "...";
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Build JSON line manually to avoid heavy JSON doc construction.
    // TS REF: src/history.ts — JSONL format with escaped display text.
    auto escape_json = [](std::string_view s) -> std::string {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;      break;
            }
        }
        return out;
    };

    std::string json_line = std::format(
        R"({{"display":"{}","full_text":"{}","timestamp":{},"sessionId":"{}","project":"{}"}})",
        escape_json(display),
        escape_json(std::string(prompt_text)),
        now_ms,
        escape_json(std::string(session_id)),
        escape_json(std::string(project_path)));

    // TS REF: src/history.ts:308-314 — file lock protects concurrent writes.
    // We use a simple static mutex for in-process serialization; cross-process
    // locking is out of scope for the migration (matches the CPP "best-effort"
    // approach used elsewhere for file writes).
    static std::mutex write_mu;
    std::lock_guard lk(write_mu);

    std::ofstream ofs(path, std::ios::app | std::ios::binary);
    if (!ofs) return;
    ofs << json_line << '\n';
}

// TS REF: src/history.ts:190-228 — getHistory() yields entries for the current
//   project only, current session first, then other sessions, newest-first,
//   deduped by display, capped at MAX_HISTORY_ITEMS (100).
// TS REF: src/hooks/useHistorySearch.ts:73-117 — search does a case-sensitive
//   substring match via lastIndexOf.  We use case-insensitive to match the
//   user expectation of a search box (TS's case-sensitivity is arguably a bug
//   — the HistorySearchDialog uses case-insensitive matching).
std::vector<HistorySuggestionData> collect_history_suggestions(
    std::string_view query,
    std::size_t max_entries)
{
    // Read a generous batch from disk so we can filter + dedup.
    constexpr std::size_t kReadBatch = 200;
    auto entries = read_history_recent(kReadBatch);

    // Build lowercase query once.
    std::string query_lower;
    query_lower.reserve(query.size());
    for (char c : query) {
        query_lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    std::vector<HistorySuggestionData> result;
    result.reserve(std::min(max_entries, entries.size()));
    std::unordered_set<std::string> seen_display;

    // Determine current project for filtering (best-effort).
    std::error_code ec;
    std::string current_project = fs::current_path(ec).string();

    for (const auto& entry : entries) {
        // Substring match (case-insensitive) on display + full_text.
        if (!query_lower.empty()) {
            std::string display_lower;
            display_lower.reserve(entry.display.size());
            for (char c : entry.display) {
                display_lower.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            std::string full_lower;
            full_lower.reserve(entry.full_text.size());
            for (char c : entry.full_text) {
                full_lower.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            if (display_lower.find(query_lower) == std::string::npos &&
                full_lower.find(query_lower) == std::string::npos) {
                continue;
            }
        }

        // Dedup by display text (TS getHistory dedup semantics).
        if (!seen_display.insert(entry.display).second) continue;

        result.push_back(HistorySuggestionData{
            .prompt_text   = entry.display,
            .full_text     = entry.full_text,
            .timestamp_ms  = entry.timestamp_ms,
            .session_id    = entry.session_id,
            .message_count = 0,
        });

        if (result.size() >= max_entries) break;
    }

    return result;
}

// ============================================================
// Pre-formatted suggestions (extracted from app.cppm to stay
// under clang's 2GB source-location budget).
// ============================================================

namespace {

// Format a relative time string like "5m ago" or "2d ago".
[[nodiscard]] std::string format_relative_time(std::int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return {};
    using namespace std::chrono;
    auto now = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    auto ds = (now - timestamp_ms) / 1000;
    if (ds < 60) return "just now";
    if (ds < 3600) return std::format("{}m ago", ds / 60);
    if (ds < 86400) return std::format("{}h ago", ds / 3600);
    return std::format("{}d ago", ds / 86400);
}

} // anonymous namespace

std::vector<FormattedSuggestion> build_history_suggestions(
    std::string_view query,
    std::size_t replacement_start,
    std::size_t replacement_end,
    std::size_t max_entries)
{
    auto entries = collect_history_suggestions(query, max_entries);
    std::vector<FormattedSuggestion> result;
    result.reserve(entries.size());

    for (const auto& entry : entries) {
        std::string display = entry.prompt_text;
        if (display.size() > 80) display = display.substr(0, 77) + "...";

        std::string desc = "History";
        auto rel = format_relative_time(entry.timestamp_ms);
        if (!rel.empty()) desc += " · " + rel;
        if (!entry.session_id.empty())
            desc += std::format(" · {}", entry.session_id.substr(0, 8));

        result.push_back(FormattedSuggestion{
            .display_text = std::move(display),
            .description = std::move(desc),
            .insert_text = entry.full_text,
            .replacement_start = replacement_start,
            .replacement_end = replacement_end,
            .submit_on_return = false,
            .id = "history:" + std::to_string(entry.timestamp_ms),
            .icon = {},
            .color_name = {},
        });
    }
    return result;
}

std::vector<FormattedSuggestion> build_agent_suggestions(
    std::string_view cwd,
    std::string_view query,
    std::size_t token_start,
    std::size_t token_end)
{
    namespace frn = cc::ui::prompt::fuzzy_rank_nucleo;

    auto agents = collect_agent_suggestions(cwd);
    std::vector<FormattedSuggestion> result;
    result.reserve(agents.size());

    for (const auto& ag : agents) {
        if (!frn::fuzzy_match_nucleo(ag.name, query)) continue;

        std::string display = "@" + ag.name;
        std::string desc_prefix = ag.is_subagent ? "Teammate" : "Agent";
        std::string desc = desc_prefix + " · " + ag.description;

        result.push_back(FormattedSuggestion{
            .display_text = std::move(display),
            .description = std::move(desc),
            .insert_text = "@" + ag.name + " ",
            .replacement_start = token_start,
            .replacement_end = token_end,
            .submit_on_return = false,
            .id = (ag.is_subagent ? "teammate:" : "agent:") + ag.name,
            .icon = ag.is_subagent ? "👤" : "🤖",
            .color_name = ag.color.value_or(""),
        });
    }
    return result;
}

} // namespace cc::ui::autocomplete_sources
