// app_autocomplete.cpp — impl unit for AppAdapter methods kept OUT of app.cppm
// to stay under clang's 2GB source-location budget.
// Contains: RefreshAutocompleteSuggestions, destructor, Render, OnEvent,
//           ActiveChild.
//
// Other methods were split into separate impl units:
//   app_constructor.cpp   — constructor
//   app_handle_submit.cpp — HandleSubmit, HandleCommand
//   app_agent_menu.cpp    — FormatAgentsMenuOutput, LoadAgentCardsForMenu,
//                           SyncState, ConsumePendingResult,
//                           WaitForInFlightPastes, get_permission_callback,
//                           trigger_orphan_cleanup_for_testing
//   app_extra_methods.cpp — RunLocalBashCommand, ProjectRuntimeMetadataToScreenState,
//                           ApplyMessageCollapsePipeline, SpawnPasteWorker,
//                           ProcessCompletedPastes, project_agent_definition_card
module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

module cc.ui.app;

import cc.commands.registry;
import cc.tools.agent_runtime;
import cc.ui.autocomplete_sources;
import cc.ui.common.declared_cursor;
import cc.ui.prompt.file_index;
import cc.ui.prompt.fuzzy_rank_nucleo;
import cc.ui.repl_screen;
import cc.utils.debug;
import cc.utils.hyperlink;
import cc.utils.parse_references;
import cc.utils.path;
import cc.utils.skill_usage;
import cc.utils.session_storage;

namespace cc::ui {

namespace repl = cc::ui::repl_screen;
namespace agent_runtime = cc::tools::agent_runtime;
namespace acsrc = cc::ui::autocomplete_sources;
namespace frn = cc::ui::prompt::fuzzy_rank_nucleo;
namespace fidx = cc::ui::prompt::file_index;

void AppAdapter::RefreshAutocompleteSuggestions() {
    const auto previous_suggestions = screen_state_->autocomplete_suggestions;
    const int previous_index = screen_state_->autocomplete_index;
    screen_state_->autocomplete_suggestions.clear();
    screen_state_->autocomplete_index = -1;
    screen_state_->autocomplete_stable_name_width = 0;  // INF-03: slash branch sets it

    const std::string& input = screen_state_->input_text;
    const std::size_t cursor =
        screen_state_->input_cursor == std::string::npos ||
            screen_state_->input_cursor > input.size()
        ? input.size()
        : screen_state_->input_cursor;
    const auto token = token_around_cursor(input, cursor);

    auto add_suggestion = [&](std::string display,
                              std::string description,
                              std::string insert,
                              std::size_t start,
                              std::size_t end,
                              bool submit_on_return = false,
                              std::string id = "",
                              std::string icon = "",
                              std::string color_name = "") {
        if (id.empty()) id = display;  // INF-02: stable-id fallback
        screen_state_->autocomplete_suggestions.push_back(
            repl::ReplScreenState::AutocompleteSuggestion{
                .display_text = std::move(display),
                .description = std::move(description),
                .insert_text = std::move(insert),
                .replacement_start = start,
                .replacement_end = end,
                .submit_on_return = submit_on_return,
                .icon = std::move(icon),
                .id = std::move(id),
                .color_name = std::move(color_name),
            });
    };

    auto restore_index = [&] {
        if (screen_state_->autocomplete_suggestions.empty()) return;
        int preserved_index = 0;
        if (previous_index >= 0 &&
            previous_index < static_cast<int>(previous_suggestions.size())) {
            const auto& previous =
                previous_suggestions[static_cast<std::size_t>(previous_index)];
            // INF-02: match by stable id (falls back to display_text via
            // add_suggestion's default), so same-display items from
            // different sources don't collide.
            auto it = std::ranges::find(
                screen_state_->autocomplete_suggestions,
                previous.id,
                &repl::ReplScreenState::AutocompleteSuggestion::id);
            if (it != screen_state_->autocomplete_suggestions.end()) {
                preserved_index = static_cast<int>(
                    std::distance(
                        screen_state_->autocomplete_suggestions.begin(),
                        it));
            }
        }
        screen_state_->autocomplete_index = preserved_index;
    };

    if (input.empty()) {
        // SL-11: empty-prompt deterministic next-action suggestion.
        // Surface the QueryEnd-generated suggestion as the lone popup
        // entry while idle (not responding). A non-empty input falls
        // through and clears next_action_suggestion below.
        if (!query_running_.load() && screen_state_->next_action_suggestion &&
            screen_state_->next_action_suggestion->front() != '/') {
            const auto& text = *screen_state_->next_action_suggestion;
            add_suggestion(text, "Suggested next action", text,
                           0, 0, /*submit_on_return=*/false,
                           /*id=*/"__next_action_suggestion__");
            restore_index();
        }
        return;
    }
    // SL-11: user typed something — retire the next-action suggestion.
    screen_state_->next_action_suggestion.reset();

    // INF-05: honor a previous Esc dismissal — if the user closed the
    // popup for this exact input, don't reopen until the input changes.
    // (Suggestions were already cleared at the top of this function.)
    if (input == screen_state_->dismissed_autocomplete_for_input) {
        return;
    }
    screen_state_->dismissed_autocomplete_for_input.clear();

    // SL-03: derive inline argument hint for "/cmd ..." inputs (shown by
    // TextInputImpl after the prompt). Faithful to TS useTypeahead's
    // commandArgumentHint (src/hooks/useTypeahead.tsx:729-770).
    screen_state_->pending_argument_hint.clear();
    if (input.starts_with('/') && cmd_registry_) {
        const auto sp = input.find(' ');
        if (sp != std::string::npos && sp > 1) {
            const std::string cmd_name = input.substr(1, sp - 1);
            if (const auto* def = cmd_registry_->find_definition(cmd_name)) {
                if (!def->argument_hint.empty()) {
                    screen_state_->pending_argument_hint = def->argument_hint;
                }
            }
        }
    }

    // SL-05: mid-input slash ghost text — a "/prefix" token appearing
    // mid-input (input doesn't start with '/') completes inline to the
    // shortest matching command name. Faithful to TS findMidInputSlashCommand
    // + getBestCommandMatch (src/utils/commandSuggestions.ts:114-195).
    screen_state_->pending_ghost_text.clear();
    if (!input.starts_with('/') && token.text.starts_with('/') &&
        cmd_registry_) {
        const std::string partial = token.text.substr(1);
        if (!(partial.empty() || partial.find(' ') != std::string::npos)) {
            const CommandDefinition* best = nullptr;
            for (const auto* def : cmd_registry_->visible_commands()) {
                if (def->name.size() >= partial.size() &&
                    def->name.compare(0, partial.size(), partial) == 0) {
                    if (!best || def->name.size() < best->name.size()) best = def;
                }
            }
            if (best && best->name.size() > partial.size()) {
                screen_state_->pending_ghost_text = best->name.substr(partial.size());
            }
        }
    }

    auto add_directory_suggestions = [&](std::string_view partial) {
        std::filesystem::path base = screen_state_->cwd.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(screen_state_->cwd);
        std::filesystem::path raw{std::string(partial)};
        std::filesystem::path parent = raw.has_parent_path()
            ? base / raw.parent_path()
            : base;
        const auto prefix = raw.has_parent_path()
            ? raw.parent_path().string() + "/"
            : std::string{};
        const auto leaf = raw.filename().string();

        std::error_code ec;
        if (!std::filesystem::is_directory(parent, ec)) return;

        struct DirCandidate { std::string display; std::string insert; int rank; };
        std::vector<DirCandidate> dirs;
        for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            const auto name = entry.path().filename().string();
            if (!frn::fuzzy_match_nucleo(name, leaf)) continue;
            auto insert = prefix + name + "/";
            dirs.push_back(DirCandidate{
                .display = insert,
                .insert = insert,
                .rank = frn::fuzzy_rank_nucleo(name, leaf),
            });
            if (dirs.size() >= 50) break;
        }
        std::ranges::sort(dirs, [](const auto& a, const auto& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.display < b.display;
        });
        for (auto& dir : dirs) {
            add_suggestion(
                std::move(dir.display),
                "Directory",
                std::move(dir.insert),
                token.start,
                token.end,
                false);
        }
    };

    auto add_session_suggestions = [&](std::string_view partial) {
        if (!storage_) return;
        auto sessions = storage_->list_sessions(50);
        if (!sessions) return;
        for (const auto& session : *sessions) {
            const auto& id = session.metadata.id;
            const auto& title = session.metadata.title;
            if (!frn::fuzzy_match_nucleo(id, partial) &&
                !frn::fuzzy_match_nucleo(title, partial)) {
                continue;
            }
            add_suggestion(
                id.substr(0, std::min<std::size_t>(id.size(), 8)),
                title.empty() ? "Session" : title,
                id,
                token.start,
                token.end,
                true);
        }
    };

    const auto before_cursor = input.substr(0, cursor);
    if (before_cursor.starts_with("/add-dir ") ||
        before_cursor.starts_with("/add-dir\t")) {
        add_directory_suggestions(token.text);
        restore_index();
        return;
    }
    if (before_cursor.starts_with("/resume ") ||
        before_cursor.starts_with("/resume\t") ||
        before_cursor.starts_with("/r ") ||
        before_cursor.starts_with("/r\t")) {
        add_session_suggestions(token.text);
        restore_index();
        return;
    }

    // @history with-space trigger: user typed "@history <query>"
    // TS REF: src/hooks/useHistorySearch.ts:151 (Ctrl+R history search)
    if (before_cursor.starts_with("@history ") ||
        before_cursor.starts_with("@history\t")) {
        // Use everything after "@history " as the search query, not just
        // the token around cursor — multi-word history search should work.
        // Trim leading whitespace so "@history  " (extra spaces) is treated
        // the same as "@history " (single space) — both yield an empty query.
        std::string_view history_query(before_cursor);
        history_query.remove_prefix(9);  // len("@history ") = 9
        while (!history_query.empty() &&
               (history_query.front() == ' ' || history_query.front() == '\t')) {
            history_query.remove_prefix(1);
        }
        for (const auto& sug : acsrc::build_history_suggestions(
                 std::string(history_query), 0, cursor, 50)) {
            add_suggestion(sug.display_text, sug.description,
                sug.insert_text, sug.replacement_start, sug.replacement_end,
                sug.submit_on_return, sug.id, sug.icon, sug.color_name);
        }
        restore_index();
        return;
    }

    if (input.starts_with('/') &&
        cursor <= input.size() &&
        before_cursor.find_first_of(" \t\n") != std::string::npos &&
        cmd_registry_) {
        auto completions = cmd_registry_->complete(before_cursor);
        for (auto& completion : completions) {
            const bool whole_command = completion.starts_with('/');
            add_suggestion(
                completion,
                "Command argument",
                whole_command ? completion + " " : completion,
                whole_command ? 0 : token.start,
                whole_command ? cursor : token.end,
                true);
        }
        if (!screen_state_->autocomplete_suggestions.empty()) {
            restore_index();
            return;
        }
    }

    if (token.text.starts_with('/')) {
        const auto query = std::string_view(token.text).substr(1);
        struct SlashCandidate {
            std::string display;
            std::string description;
            std::string insert;
            int rank = 0;
            bool submit = true;
            std::string id;  // INF-02: source-aware stable id
        };
        std::vector<SlashCandidate> candidates;

        if (cmd_registry_) {
            for (const auto* def : cmd_registry_->visible_commands()) {
                if (!def) continue;
                // SL-02: multi-key match — name (exact/prefix/substring/subseq)
                // outranks a description-word match, so commands are still
                // surfaced when the user types a description term (TS Fuse
                // weights descriptionKey×0.5; cpp previously matched name only).
                int cmd_rank = -1;
                if (frn::fuzzy_match_nucleo(def->name, query)) {
                    cmd_rank = frn::fuzzy_rank_nucleo(def->name, query);
                } else {
                    const auto d = lowercase_ascii(def->description);
                    const auto q = lowercase_ascii(query);
                    if (!q.empty() && d.find(q) != std::string::npos) {
                        cmd_rank = 10;  // description match, lower priority
                    }
                }
                if (cmd_rank >= 0) {
                    // SL-06: only auto-execute (submit on Enter) commands that
                    // don't require arguments — commands with required args
                    // expand to "/cmd " so the user can type them. Faithful
                    // to TS shouldExecute gated on argNames.length.
                    bool needs_args = false;
                    for (const auto& a : def->args) {
                        if (a.required) { needs_args = true; break; }
                    }
                    // SL-07: TS shows the matched alias as a parenthetical on
                    // the CANONICAL row (commandSuggestions.ts:265-287
                    // createCommandSuggestionItem — aliasText = ` (${alias})`).
                    std::string matched_alias;
                    if (!query.empty() && !def->aliases.empty()) {
                        const auto q = lowercase_ascii(query);
                        for (const auto& alias : def->aliases) {
                            if (lowercase_ascii(alias).starts_with(q)) {
                                matched_alias = alias;
                                break;
                            }
                        }
                    }
                    std::string display = "/" + def->name;
                    if (!matched_alias.empty()) display += " (" + matched_alias + ")";
                    candidates.push_back(SlashCandidate{
                        .display = std::move(display),
                        .description = def->description,
                        .insert = "/" + def->name + " ",
                        .rank = cmd_rank,
                        .submit = !needs_args,
                        .id = "cmd:" + def->name,
                    });
                }
                for (const auto& alias : def->aliases) {
                    if (!frn::fuzzy_match_nucleo(alias, query)) continue;
                    candidates.push_back(SlashCandidate{
                        .display = "/" + alias,
                        .description = "Alias for /" + def->name,
                        .insert = "/" + alias + " ",
                        .rank = frn::fuzzy_rank_nucleo(alias, query) + 1,
                        .submit = true,
                        .id = "alias:" + alias,
                    });
                }
            }
        }

        for (const auto& skill : cached_skills_) {
            // SL-02: name match outranks a description-word match (Fuse
            // keeps descriptionKey at lower weight; cpp matched name only).
            int skill_rank = -1;
            if (frn::fuzzy_match_nucleo(skill.name, query)) {
                skill_rank = frn::fuzzy_rank_nucleo(skill.name, query) + 4;
            } else {
                const auto d = lowercase_ascii(skill.description);
                const auto q = lowercase_ascii(query);
                if (!q.empty() && d.find(q) != std::string::npos) skill_rank = 14;
            }
            if (skill_rank < 0) continue;
            // SL-04: recency boost — recently-used skills rank higher
            // (TS getSkillUsageScore). Bounded so fuzzy relevance still wins
            // on non-empty queries; on empty '/' all skills tie on fuzzy rank
            // so recency dominates, surfacing recent skills first.
            const int recency_bonus = static_cast<int>(
                std::min(cc::utils::skill_usage::get_skill_usage_score(skill.name), 3.0));
            candidates.push_back(SlashCandidate{
                .display = "/" + skill.name,
                .description = (skill.kind == "workflow")
                    ? std::format("[workflow] {} skill · {}", skill.source, skill.description)
                    : std::format("{} skill · {}", skill.source, skill.description),
                .insert = "/" + skill.name + " ",
                .rank = skill_rank - recency_bonus,
                .submit = true,
                .id = "skill:" + skill.name + ":" + skill.source,
            });
        }

        for (const auto& plugin_command : cached_plugin_commands_) {
            int plugin_rank = -1;
            if (frn::fuzzy_match_nucleo(plugin_command.command, query)) {
                plugin_rank = frn::fuzzy_rank_nucleo(plugin_command.command, query) + 6;
            } else {
                const auto d = lowercase_ascii(plugin_command.plugin_name);
                const auto q = lowercase_ascii(query);
                if (!q.empty() && d.find(q) != std::string::npos) plugin_rank = 16;
            }
            if (plugin_rank < 0) continue;
            candidates.push_back(SlashCandidate{
                .display = "/" + plugin_command.command,
                .description = "Plugin command · " + plugin_command.plugin_name,
                .insert = "/" + plugin_command.command + " ",
                .rank = plugin_rank,
                .submit = false,
                .id = "plugin:" + plugin_command.command + ":" + plugin_command.plugin_name,
            });
        }

        // SL-01: hidden-command exact-name escape hatch — if the user typed
        // the full name of a hidden command, surface it at the top (TS
        // commandSuggestions.ts:391-401 hiddenExact). visible_commands()
        // otherwise hides them entirely.
        if (auto* hidden = cmd_registry_->hidden_command_if_exact(query)) {
            candidates.push_back(SlashCandidate{
                .display = "/" + hidden->name,
                .description = hidden->description,
                .insert = "/" + hidden->name + " ",
                .rank = -1000,  // force top (rank ascending = smaller first)
                .submit = true,
                .id = "hidden-cmd:" + hidden->name,
            });
        }

        std::ranges::sort(candidates, [](const auto& a, const auto& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.display < b.display;
        });
        // INF-03: precompute stable name-column width over ALL candidates
        // (not just the visible window) so the description column doesn't
        // jitter as the user filters. Slash names are ASCII, so size() is a
        // faithful width measure here. Mirrors TS maxColumnWidth over
        // visible commands (src/hooks/useTypeahead.tsx:380-386).
        {
            int widest = 0;
            for (const auto& c : candidates) {
                widest = std::max(widest, static_cast<int>(c.display.size()));
            }
            screen_state_->autocomplete_stable_name_width = widest;
        }
        const std::size_t limit = std::min<std::size_t>(candidates.size(), 80);
        for (std::size_t i = 0; i < limit; ++i) {
            add_suggestion(
                std::move(candidates[i].display),
                std::move(candidates[i].description),
                std::move(candidates[i].insert),
                token.start,
                token.end,
                candidates[i].submit,
                std::move(candidates[i].id));
        }
        restore_index();
        return;
    }

    // INF-01/AT-08: @ mentions are suppressed in bash mode. Faithful to TS
    // useTypeahead (the @DM/@file branches are gated on mode !== 'bash');
    // in bash mode we fall through to the $PATH shell-command scan below.
    if (token.text.starts_with('@') &&
        !repl::effective_is_bash(*screen_state_)) {
        // TS REF: src/hooks/useTypeahead.tsx:123-135 (extractSearchToken) —
        // strip @" prefix and trailing " for quoted paths.
        std::string raw_query_text(token.text.substr(1));
        const bool is_quoted = !raw_query_text.empty() && raw_query_text.front() == '"';
        if (is_quoted) {
            raw_query_text.erase(raw_query_text.begin());  // strip leading "
            if (!raw_query_text.empty() && raw_query_text.back() == '"') {
                raw_query_text.pop_back();  // strip trailing "
            }
        }
        const std::string_view query(raw_query_text);

        // ── @history trigger: prompt history search ──────────────────────
        // TS REF: src/hooks/useHistorySearch.ts:151 (handleStartSearch —
        //   Ctrl+R enters history search mode with substring matching)
        // When user types @history<query>, surface persisted prompt history.
        if (raw_query_text.starts_with("history")) {
            auto after = std::string_view(raw_query_text).substr(7);
            while (!after.empty() && (after.front() == ' ' || after.front() == '\t'))
                after.remove_prefix(1);
            for (const auto& sug : acsrc::build_history_suggestions(
                     std::string(after), token.start, token.end, 50)) {
                add_suggestion(sug.display_text, sug.description,
                    sug.insert_text, sug.replacement_start, sug.replacement_end,
                    sug.submit_on_return, sug.id, sug.icon, sug.color_name);
            }
            restore_index();
            return;
        }

        std::filesystem::path base = screen_state_->cwd.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(screen_state_->cwd);

        // TS REF: src/utils/suggestions/directoryCompletion.ts:55-78
        // (parsePartialPath) — expand ~ before resolving the directory to scan.
        // The display prefix keeps the original unexpanded form so the user
        // sees @~/Documents not @/home/user/Documents.
        std::filesystem::path raw_path(raw_query_text);
        std::filesystem::path expanded_path = cc::utils::path::expand_tilde(raw_path);

        // Compute the display prefix (unexpanded, e.g. "~/src/")
        const auto prefix = raw_path.has_parent_path()
            ? raw_path.parent_path().string() + "/"
            : std::string{};

        // Compute the actual directory to scan (expanded, e.g. /home/user/src)
        std::filesystem::path parent;
        std::string leaf;
        if (expanded_path.has_parent_path()) {
            parent = expanded_path.parent_path();
            leaf = expanded_path.filename().string();
        } else {
            parent = expanded_path;  // e.g. "~" alone → home dir
            leaf.clear();
        }
        // If parent is still relative (e.g. "src" without ./), resolve against base
        if (!parent.is_absolute()) {
            parent = base / parent;
        }

        // AT-01: bare @-queries (no path separator) fuzzy-match the whole
        // repo via the file index — TS searches the index, so "@readme"
        // finds src/readme.md. The directory_iterator block below still
        // handles explicit path browsing (@src/...).
        // TS REF: src/utils/suggestions/directoryCompletion.ts:152-162
        // (isPathLikeToken) — ~/ ./ ../ / all trigger path completion.
        const bool has_separator = query.find('/') != std::string_view::npos ||
                                   query.find('\\') != std::string_view::npos;
        const bool is_path_like = has_separator ||
                                  query == "~" || query == "." || query == ".." ||
                                  query.starts_with("~/") || query.starts_with("./") ||
                                  query.starts_with("../");
        const bool bare_query = !is_path_like;

        // AT-05: bare-@ teammate DM precedence — when the query has no path
        // separator, teammate (native-agent) DM matches are shown EXCLUSIVELY
        // before file/agent/mcp, mirroring TS useTypeahead @DM
        // (src/hooks/useTypeahead.tsx:596-637, startsWith on lowercased name).
        // With a match we return immediately so the popup is DM-only.
        if (bare_query) {
            auto starts_with_ci = [](std::string_view name, std::string_view q) {
                if (name.size() < q.size()) return false;
                for (std::size_t i = 0; i < q.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(name[i])) !=
                        std::tolower(static_cast<unsigned char>(q[i]))) {
                        return false;
                    }
                }
                return true;
            };
            struct DmCandidate { std::string display; std::string insert; std::string desc; std::string color; };
            std::vector<DmCandidate> dms;
            for (const auto& record : agent_runtime::load_all_native_agent_records()) {
                const auto name = record.name.value_or(record.agent_id);
                if (!starts_with_ci(name, query)) continue;
                dms.push_back(DmCandidate{
                    .display = "@" + std::string(name),
                    .insert = "@" + std::string(name) + " ",
                    .desc = "Teammate · " +
                            std::string(agent_runtime::native_agent_status_name(record.status)),
                    .color = record.teammate_color.value_or(""),
                });
            }
            if (!dms.empty()) {
                for (auto& d : dms) {
                    add_suggestion(std::move(d.display), std::move(d.desc),
                                   std::move(d.insert), token.start, token.end,
                                   false, "", "👤", std::move(d.color));
                }
                restore_index();
                return;  // exclusive: bare-@ with teammate match shows DMs only
            }
        }

        std::error_code ec;
        // AT-06: skip the file index when the query is empty (bare "@") —
        // TS shows teammates/MCP/agents on an empty @ query, not every file
        // in the repo. With a non-empty query the index fuzzy-matches.
        if (bare_query && !query.empty()) {
            struct RepoCandidate { std::string display; std::string insert; std::string desc; int rank; std::string icon; };
            std::vector<RepoCandidate> repos;
            for (const auto& rel : fidx::collect_repo_files(screen_state_->cwd)) {
                const std::size_t slash = rel.find_last_of("/\\");
                const std::string base = (slash == std::string_view::npos)
                    ? rel : rel.substr(slash + 1);
                const bool match_base = frn::fuzzy_match_nucleo(base, query);
                const bool match_path = !match_base && frn::fuzzy_match_nucleo(rel, query);
                if (!match_base && !match_path) continue;
                // TS REF: PromptInputFooterSuggestions.tsx — files get a file
                // icon. Directories (trailing /) get a directory icon.
                const bool is_dir = !rel.empty() && rel.back() == '/';
                repos.push_back(RepoCandidate{
                    .display = "@" + rel,
                    .insert = "@" + rel + " ",
                    .desc = is_dir ? "Directory" : "File",
                    .rank = frn::fuzzy_rank_nucleo(base, query) + (match_base ? 0 : 2),
                    .icon = is_dir ? "📁" : "📄",
                });
                if (repos.size() >= 50) break;  // TS REF: MAX_UNIFIED_SUGGESTIONS = 15; we cap at 50 for terminal
            }
            std::ranges::sort(repos, [](const auto& a, const auto& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.display < b.display;
            });
            for (auto& r : repos) {
                add_suggestion(std::move(r.display), std::move(r.desc),
                               std::move(r.insert), token.start, token.end,
                               false, "", std::move(r.icon));
            }
        } else if (std::filesystem::is_directory(parent, ec)) {
            struct FileCandidate { std::string display; std::string insert; std::string desc; int rank; std::string icon; };
            std::vector<FileCandidate> files;
            for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
                if (ec) break;
                const auto name = entry.path().filename().string();
                if (name.starts_with(".")) continue;
                if (!frn::fuzzy_match_nucleo(name, leaf)) continue;
                const bool is_dir = entry.is_directory(ec);
                auto rel = prefix + name + (is_dir ? "/" : "");
                // TS REF: useTypeahead.tsx:244-246 (applyDirectoryCompletion) —
                // directories get "/" suffix so user can continue browsing.
                // Icons: 📄 for files, 📁 for directories.
                files.push_back(FileCandidate{
                    .display = "@" + rel,
                    .insert = is_quoted
                        ? std::format("@\"{}\"", rel)  // re-wrap in quotes
                        : "@" + rel,
                    .desc = is_dir ? "Directory" : "File",
                    .rank = frn::fuzzy_rank_nucleo(name, leaf),
                    .icon = is_dir ? "📁" : "📄",
                });
                if (files.size() >= 50) break;  // cap at 50 results
            }
            std::ranges::sort(files, [](const auto& a, const auto& b) {
                if (a.rank != b.rank) return a.rank < b.rank;
                return a.display < b.display;
            });
            for (auto& file : files) {
                add_suggestion(
                    std::move(file.display),
                    std::move(file.desc),
                    std::move(file.insert),
                    token.start,
                    token.end,
                    false,
                    "",
                    std::move(file.icon));
            }
        }

        // ── Agent / teammate autocomplete ─────────────────────────────
        // TS REF: src/hooks/unifiedSuggestions.ts:77-108 (agent defs with
        //   color + truncated whenToUse)
        // TS REF: src/hooks/useTypeahead.tsx:604-625 (teammate DMs with
        //   status, prefix-matched on lowercased name)
        // Heavy lifting (fuzzy filter + formatting) lives in build_agent_suggestions()
        // to keep app.cppm under clang's source-location budget.
        for (const auto& sug : acsrc::build_agent_suggestions(
                 screen_state_->cwd, query, token.start, token.end)) {
            add_suggestion(sug.display_text, sug.description,
                sug.insert_text, sug.replacement_start, sug.replacement_end,
                sug.submit_on_return, sug.id, sug.icon, sug.color_name);
        }
        // TS REF: src/hooks/useMcpResourceAutocomplete.ts — MCP server
        //         resources (list_native_mcp_resources) are collected and
        //         passed to the unified @-mention autocomplete dropdown
        //         alongside files, agents, skills, and history.  The
        //         display name is shown in the picker; insert_text is
        //         what gets injected into the prompt buffer on accept.
        //         CPP: collect_mcp_resource_suggestions() → add_suggestion()
        //         → autocomplete_suggestions vector → RenderPromptSuggestions().
        for (const auto& resource : acsrc::collect_mcp_resource_suggestions()) {
            if (!frn::fuzzy_match_nucleo(resource.display, query) &&
                !frn::fuzzy_match_nucleo(resource.insert_text, query)) {
                continue;
            }
            add_suggestion(
                "@" + resource.display,
                resource.description,
                "@" + resource.insert_text + " ",
                token.start,
                token.end,
                false,
                "",
                "🔌");
        }

        // AT-10: @-history session autocomplete — search past sessions by
        // title/ID. Faithful to TS searchSessionsByCustomTitle
        // (src/utils/sessionStorage.ts:3066-3107) exposed as an @-mention
        // source alongside files/agents/MCP. Sessions are sorted by
        // recency (newest first) per SessionStorage::list_sessions.
        if (storage_) {
            auto sessions = storage_->list_sessions(30);
            if (sessions) {
                for (const auto& session : *sessions) {
                    const auto& id = session.metadata.id;
                    const auto& title = session.metadata.title;
                    const std::string short_id =
                        id.substr(0, std::min<std::size_t>(id.size(), 8));
                    const std::string display_label =
                        title.empty() ? short_id : title;

                    if (!frn::fuzzy_match_nucleo(display_label, query) &&
                        !frn::fuzzy_match_nucleo(id, query) &&
                        !frn::fuzzy_match_nucleo(short_id, query)) {
                            continue;
                        }

                    // Build description: "Session" + short-ID hint + msg count
                    std::string desc = "Session";
                    if (!title.empty() && title != short_id) {
                        desc += " · " + short_id;
                    }
                    if (session.metadata.message_count > 0) {
                        desc += std::format(
                            " ({} msgs)", session.metadata.message_count);
                    }

                    add_suggestion(
                        "@" + display_label,
                        std::move(desc),
                        "@" + id,
                        token.start,
                        token.end,
                        /*submit_on_return=*/false,
                        /*id=*/"session:" + id,
                        /*icon=*/"🕘");
                }
            }
        }

        // AT-11: @-history prompt autocomplete — show recent prompt history
        // entries alongside sessions.  Faithful to TS getHistory()
        // (src/history.ts:190-228) which yields HistoryEntry objects for
        // the current project, newest-first, deduped by display text.
        // The @history with-space trigger (above) shows prompts too, but
        // the bare-@ section surfaces them inline so users can @-mention
        // a recent prompt without typing @history first.
        {
            auto prompt_hist = acsrc::collect_history_suggestions(query, 20);
            for (const auto& entry : prompt_hist) {
                std::string display = entry.prompt_text;
                if (display.size() > 80) display = display.substr(0, 77) + "...";

                // Build description with relative time + session hint
                std::string desc = "Prompt";
                if (entry.timestamp_ms > 0) {
                    using namespace std::chrono;
                    auto now = duration_cast<milliseconds>(
                        system_clock::now().time_since_epoch()).count();
                    auto delta_sec = (now - entry.timestamp_ms) / 1000;
                    if (delta_sec < 60) desc += " · just now";
                    else if (delta_sec < 3600) desc += std::format(" · {}m ago", delta_sec / 60);
                    else if (delta_sec < 86400) desc += std::format(" · {}h ago", delta_sec / 3600);
                    else desc += std::format(" · {}d ago", delta_sec / 86400);
                }
                if (!entry.session_id.empty()) {
                    desc += std::format(" · {}", entry.session_id.substr(0, 8));
                }

                // Insert: the full prompt text so accepting it pastes the
                // complete previous prompt into the input.
                add_suggestion(
                    "@" + display,
                    std::move(desc),
                    entry.full_text,
                    token.start,
                    token.end,
                    /*submit_on_return=*/false,
                    /*id=*/"prompt-history:" + std::to_string(entry.timestamp_ms),
                    /*icon=*/"📝");
            }
        }

        restore_index();
        return;
    }

    // INF-01/AT-08: # channels are suppressed in bash mode. Faithful to TS
    // useTypeahead (the #slack branch is gated on mode === 'prompt'); in
    // bash mode we fall through to the $PATH shell-command scan below.
    if (token.text.starts_with('#') &&
        !repl::effective_is_bash(*screen_state_)) {
        const auto query = std::string_view(token.text).substr(1);
        // TS REF: src/hooks/useMcpResourceAutocomplete.ts — channel-like
        //         MCP resources (name starts with '#' or uri contains
        //         "channel"/"slack") are passed to the #-channel autocomplete
        //         dropdown.  This is how MCP server channels surface in the
        //         #-mention picker alongside native Slack channels.
        //         CPP: collect_mcp_resource_suggestions() filters to
        //         channel_like entries → add_suggestion() → dropdown.
        for (const auto& resource : acsrc::collect_mcp_resource_suggestions()) {
            if (!resource.channel_like) continue;
            const auto display = resource.display.starts_with("#")
                ? resource.display
                : "#" + resource.display;
            if (!frn::fuzzy_match_nucleo(display, token.text) &&
                !frn::fuzzy_match_nucleo(resource.insert_text, query)) {
                    continue;
                }
            add_suggestion(
                display,
                resource.description,
                display + " ",
                token.start,
                token.end,
                false,
                "",
                "💬");
        }
        restore_index();
        return;
    }

    if (repl::effective_is_bash(*screen_state_) && !token.text.empty()) {
        std::unordered_set<std::string> seen;
        if (const char* path_env = std::getenv("PATH")) {
            std::string_view paths(path_env);
            while (!paths.empty()) {
                auto sep = paths.find(':');
                auto current = sep == std::string_view::npos
                    ? paths
                    : paths.substr(0, sep);
                if (sep == std::string_view::npos) paths = {};
                else paths.remove_prefix(sep + 1);

                std::error_code ec;
                std::filesystem::path dir{std::string(current)};
                if (!std::filesystem::is_directory(dir, ec)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec) break;
                    auto name = entry.path().filename().string();
                    if (seen.contains(name) || !frn::fuzzy_match_nucleo(name, token.text)) continue;
                    seen.insert(name);
                    add_suggestion(
                        name,
                        "Shell command",
                        name + " ",
                        token.start,
                        token.end,
                        false,
                        "",
                        "▶_");
                    if (screen_state_->autocomplete_suggestions.size() >= 50) break;
                }
                if (screen_state_->autocomplete_suggestions.size() >= 50) break;
            }
        }
        restore_index();
    }
}

// ── Virtual/override methods (moved out of class body to fix vtable crash) ──
// These were previously defined inline in AppAdapter's class body.  Moving
// them out makes Render() the key function (first non-inline virtual), so
// the vtable is emitted in this impl TU rather than in every TU that imports
// cc.ui.app.  This avoids a clang crash in DefineUsedVTables during app.cppm
// compilation (exit code 139 in NamespaceDecl::getMostRecentDeclImpl).

AppAdapter::~AppAdapter() {
    if (query_running_.load() && engine_) {
        engine_->abort();
    }
    if (query_thread_.joinable()) query_thread_.request_stop();
    if (spinner_thread_.joinable()) spinner_thread_.request_stop();
    if (statusline_thread_.joinable()) statusline_thread_.request_stop();
    if (bash_thread_.joinable()) bash_thread_.request_stop();
    {
        std::lock_guard lk(statusline_mutex_);
        statusline_dirty_.store(true);
    }
    {
        std::lock_guard lk(permission_mutex_);
        permission_response_ = false;
    }
    {
        std::lock_guard lk(elicitation_mutex_);
        elicitation_response_ = false;
    }
    {
        std::lock_guard lk(ask_user_mutex_);
        ask_user_response_ = std::optional<std::string>{};
    }
    statusline_cv_.notify_all();
    permission_cv_.notify_all();
    elicitation_cv_.notify_all();
    ask_user_cv_.notify_all();

    // Unsubscribe from dynamic skill discovery callbacks.
    if (skills_changed_unsubscribe_) {
        try { skills_changed_unsubscribe_(); } catch (...) {}
    }
}

Element AppAdapter::Render() {
    this->ProjectRuntimeMetadataToScreenState();
    ConsumePendingResult();
    // AT-09: apply any inbound IDE at_mentioned tokens that landed since
    // the last frame (drained on the render thread for input_text safety).
    repl::DrainPendingAtMentionInserts(screen_state_);

    const bool qr = query_running_.load();
    cc::utils::debug("app.render",
        "Render: query_running={}, messages={}, spinner_mode={}",
        qr, screen_state_->messages.size(),
        static_cast<int>(screen_state_->spinner_mode));

    if (qr) {
        std::lock_guard lk(result_mutex_);

        const auto now = std::chrono::system_clock::now();
        auto messages = engine_->get_conversation();
        // TS Messages.tsx:520 collapse chain (background-bash so far).
        messages = ApplyMessageCollapsePipeline(std::move(messages));
        screen_state_->messages.clear();
        screen_state_->messages.reserve(
            messages.size() + streaming_tools_.size() +
            streaming_thinking_.size() + 1);

        // Same uuid-assignment pattern as SyncState above — per-source
        // Message 24-char prefix shared by all derived sub-rows.
        auto make_uuid24 = [](std::uint64_t msg_idx, const std::string& seed) {
            std::uint64_t h = 1469598103934665603ULL;
            for (char c : seed) { h ^= static_cast<std::uint64_t>(c); h *= 1099511628211ULL; }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "msg_%012llx%08llx",
                          (unsigned long long)msg_idx,
                          (unsigned long long)(h & 0xFFFFFFFFULL));
            return std::string(buf, 24);
        };
        std::uint64_t tick_msg_idx = 0;

        // Project completed messages (skip system prompt, split mixed assistant).
        for (const auto& msg : messages) {
            if (std::holds_alternative<SystemMessage>(msg)) continue;
            auto projected = project_messages(msg);
            std::string seed_preview;
            std::visit([&](const auto& m) {
                if constexpr (requires{ m.content; }) {
                    for (const auto& blk : m.content) {
                        if (const auto* tb = std::get_if<TextBlock>(&blk)) {
                            seed_preview += tb->text.substr(0, 64);
                            break;
                        }
                    }
                }
            }, msg);
            const std::string u24 = make_uuid24(tick_msg_idx, seed_preview);
            for (auto& e : projected) {
                e.id = u24;
                screen_state_->messages.push_back(std::move(e));
            }
            ++tick_msg_idx;
        }
        AppendLocalMessagesToScreenState();
        // Restore chronological order (see SyncState). Applied BEFORE the
        // in-flight streaming projection so streaming rows stay last.
        std::stable_sort(
            screen_state_->messages.begin(),
            screen_state_->messages.end(),
            [](const repl::MessageDisplayEntry& a,
               const repl::MessageDisplayEntry& b) {
                return a.timestamp < b.timestamp;
            });

        // ── Faithful live-path: project in-flight content blocks ──
        // TS renders each content block as a separate message row as it
        // streams in (thinking → text → tool-use, in block-index order).
        // We follow the same pattern: collect all active streaming blocks
        // by their index, then project each into a MessageDisplayEntry
        // that flows through RenderMessages → render_payload_row → the
        // matching faithful Element renderer (thinking / tool-use / text).
        // This way tool-use rows and thinking rows appear progressively
        // during streaming, not just after the full message completes.
        bool has_in_flight = !streaming_text_.empty() ||
                             !streaming_tools_.empty() ||
                             !streaming_thinking_.empty();

        // If any streaming tool has complete=true (ContentBlockStop received),
        // the AssistantMessage for this turn was already committed to the
        // conversation (append_message fires after all blocks finish streaming).
        // All streaming blocks (text + thinking + tool-use) from this turn
        // are now duplicates of what the committed path already projected.

        // TS REF: Messages.tsx L382-389 — prune thinking entries whose
        // 30-second grace period has expired.  Done before has_in_flight
        // so stale entries don't keep the projection path alive.
        {
            auto now = std::chrono::steady_clock::now();
            std::erase_if(streaming_thinking_,
                [&now](const auto& p) {
                    return p.second.complete &&
                           p.second.streaming_ended_at &&
                           std::chrono::duration_cast<std::chrono::seconds>(
                               now - *p.second.streaming_ended_at).count() >= 30;
                });
        }

        if (has_in_flight) {
            bool all_tools_complete = !streaming_tools_.empty() &&
                std::ranges::all_of(streaming_tools_, [](const auto& p) {
                    return p.second.complete;
                });
            // TS REF: Messages.tsx L382-389 + L714-719 — streaming thinking
            // tail stays visible for 30s after completion.  Keep projecting
            // in-flight blocks when thinking is still within the grace
            // period, even if all tools have completed.
            bool thinking_visible = is_streaming_thinking_visible();
            if (all_tools_complete && !thinking_visible) has_in_flight = false;
        }

        cc::utils::debug("app.render",
            "  streaming-path: committed_msgs={}, in_flight={} "
            "(text_len={}, tools={}, thinking={})",
            screen_state_->messages.size(), has_in_flight,
            streaming_text_.size(), streaming_tools_.size(),
            streaming_thinking_.size());

        if (has_in_flight) {
            // Gather block indices (keys of all streaming maps) to sort.
            std::vector<std::uint32_t> block_indices;
            for (const auto& [i, _] : streaming_thinking_)
                block_indices.push_back(i);
            for (const auto& [i, _] : streaming_tools_)
                block_indices.push_back(i);
            // Text block always has index equal to the highest block
            // index (or 0 if no other blocks).  We track it implicitly.
            // Assign it a sentinel index for sorting purposes.
            std::uint32_t text_idx = 0;
            if (!streaming_text_.empty()) {
                // Text block index = max of all other indices + 1, or 0.
                for (std::uint32_t i : block_indices)
                    if (i > text_idx) text_idx = i;
                if (!block_indices.empty()) text_idx += 1;
                block_indices.push_back(text_idx);
            }
            std::sort(block_indices.begin(), block_indices.end());

            // Project each block in index order.
            // All in-flight streaming blocks belong to the same in-progress
            // source AssistantMessage → share a single 24-char uuid prefix so
            // the UnseenDivider still lands even when e.g. the first
            // block is a ThinkingBlock (TS CC-724).
            const std::string streaming_uuid24 =
                make_uuid24(tick_msg_idx, "streaming");
            for (std::uint32_t idx : block_indices) {
                // Thinking block
                auto thk = streaming_thinking_.find(idx);
                if (thk != streaming_thinking_.end()) {
                    repl::MessageDisplayEntry e;
                    e.role = "assistant";
                    e.is_thinking = true;
                    // TS REF: Messages.tsx L382-389 — active while streaming
                    // (not complete) or within 30s grace period after
                    // completion.  thinking_active=true makes the
                    // faithful renderer show the expanded body rather
                    // than the collapsed label, and prevents the
                    // build_visible_rows filter from hiding this row.
                    {
                        auto now = std::chrono::steady_clock::now();
                        bool within_grace = thk->second.streaming_ended_at &&
                            std::chrono::duration_cast<std::chrono::seconds>(
                                now - *thk->second.streaming_ended_at).count() < 30;
                        e.thinking_active = !thk->second.complete || within_grace;
                    }
                    e.content_preview = thk->second.text.substr(0, 200);
                    e.timestamp = now;
                    e.id = streaming_uuid24;
                    screen_state_->messages.push_back(std::move(e));
                    continue;
                }
                // Tool-use block
                auto tlu = streaming_tools_.find(idx);
                if (tlu != streaming_tools_.end()) {
                    // Once exec_done, the committed conversation already
                    // contains this tool_use (AssistantMessage was appended
                    // before execute_pending_tools ran).  Skip it here to
                    // avoid a duplicate green-dot row in the transcript.
                    if (tlu->second.exec_done) continue;

                    repl::MessageDisplayEntry e;
                    e.role = "assistant";
                    e.is_tool_use = true;
                    e.tool_name = tlu->second.tool_name;
                    e.tool_input_json = tlu->second.input_json;
                    // Status: if tool execution has completed
                    // (ToolExecutionEnd received), show resolved status
                    // so the faithful renderer paints a green/red dot
                    // instead of the animated spinner.  The actual result
                    // lives in the separate UserToolResult card (committed
                    // ToolResultMessage), NOT in the tool_use card's
                    // Output section — TS parity.
                    //
                    // We check exec_done (set by ToolExecutionEnd), NOT
                    // complete (set by ContentBlockStop when input_json
                    // finishes streaming — happens before execution).
                    if (tlu->second.exec_done) {
                        e.tool_status = tlu->second.is_error
                            ? "error" : "success";
                    } else {
                        e.tool_status = "running";
                    }
                    e.content_preview = tlu->second.input_json;
                    // result_preview: ONLY forward while the tool is
                    // still executing (progress output).  Once exec_done,
                    // suppress it — the committed ✓ card shows the final
                    // result, and showing it here too would duplicate
                    // the output and defeat the "separate result card"
                    // UX that TS uses (AssistantToolUseMessage +
                    // UserToolSuccessMessage).
                    if (!tlu->second.exec_done &&
                        !tlu->second.result_preview.empty())
                        e.tool_result_preview = tlu->second.result_preview;
                    e.is_error = tlu->second.is_error;
                    e.timestamp = now;
                    e.id = streaming_uuid24;
                    screen_state_->messages.push_back(std::move(e));
                    continue;
                }
                // Text block (streaming)
                if (!streaming_text_.empty() && idx == text_idx) {
                    repl::MessageDisplayEntry e;
                    e.role = "assistant";
                    e.content_preview = streaming_text_;
                    e.is_streaming = true;
                    e.timestamp = now;
                    e.id = streaming_uuid24;
                    screen_state_->messages.push_back(std::move(e));
                    continue;
                }
            }

            screen_state_->scroll_pinned_to_bottom = true;
            // Clear unseen divider on repin (TS: onRepin → setDividerIndex(null))
            screen_state_->divider_index.reset();
            screen_state_->unseen_divider.reset();
            screen_state_->unseen_message_count = 0;
            screen_state_->pill_visible = false;
        }
    }

    // Reset cursor to hidden each frame (TS Ink default behavior).
    // Any active declared_cursor decorator on descendant elements can
    // override this with a physical cursor anchor at the declared position.
    // This enables IME preedit text to appear inline at the insertion
    // point and lets screen readers / magnifiers follow the input.
    namespace dc = cc::ui::common::declared_cursor;
    // Drain background paste results on every render frame so the user
    // doesn't need to press another key to see the image data filled in.
    this->ProcessCompletedPastes();
    // TS REF: Messages.tsx L382-389 + L395-419 — thread the streaming-
    // thinking-visible flag to the messages list so it can hide ALL
    // completed thinking rows when the streaming-thinking tail is on
    // screen (TS: lastThinkingBlockId = 'streaming').
    screen_state_->streaming_thinking_globally_visible =
        is_streaming_thinking_visible();
    return repl_component_->Render() | dc::cursor_reset();
}

bool AppAdapter::OnEvent(Event event) {
    // Drain any background-thread paste results that completed since the
    // last event.  Must happen on the render thread (we mutate
    // pasted_contents_ and possibly input_text).
    this->ProcessCompletedPastes();

    // Clipboard image paste (TS chat:imagePaste = ctrl+v / cmd+v).
    //
    // NOTE: We detect Ctrl+V via `Event::Character('\x16')` — the same
    // pattern used by text_input.cppm L586.  `event.input() == "\x16"`
    // is unreliable because FTXUI normalizes control-character input()
    // to empty string in some code paths.
    //
    // PERFORMANCE NOTE: osascript clipboard reads are offloaded to a
    // background thread because std::system() + osascript fork + PNG
    // encoding can take 600ms+ and would block the FTXUI event loop
    // if done synchronously.  The "[Image #N]" placeholder is inserted
    // IMMEDIATELY so the user sees instant feedback; the actual image
    // data fills in asynchronously via ProcessCompletedPastes() (drained
    // on every OnEvent AND every Render() frame).
    //
    // TS REF: src/utils/imagePaste.ts (clipboard read + size-cap)
    //         + PromptInput.tsx L1151-1183 (onImagePaste → state + placeholder).
    if (event == Event::Character('\x16') && !query_running_.load()) {
        // TS REF: PromptInput.tsx L1154-1182
        //   pasteId = nextPasteIdRef.current++
        //   setPastedContents(prev => ({...prev, [pasteId]: newContent}))
        //   insertTextAtCursor(prefix + formatImageRef(pasteId))
        const int id = next_paste_id_++;

        // Insert " [Image #N]" at cursor IMMEDIATELY (instant feedback).
        const std::string placeholder = cc::utils::format_image_ref(id);
        const auto cursor = repl::input_cursor_or_end(*screen_state_);
        std::string to_insert;
        if (cursor > 0 && cursor <= screen_state_->input_text.size() &&
            screen_state_->input_text[cursor - 1] != ' ') {
            to_insert = " " + placeholder;
        } else {
            to_insert = placeholder;
        }
        repl::insert_prompt_text(screen_state_, to_insert);

        // Offload the actual clipboard read to a background thread.
        this->SpawnPasteWorker(id);
        return true;
    }

    // ── Ctrl+S: manual stash/unstash (TS chat:stash) ─────────────────
    // TS REF: PromptInput.tsx L1357-1383 handleStash —
    //   If input is empty AND stash exists → pop stash (restore text).
    //   If input is non-empty → stash input and clear input.
    //   This lets users temporarily put aside a long prompt to run a
    //   quick command, then restore it.
    if (event == Event::Character('\x13') && !query_running_.load()) {
        namespace repl = cc::ui::repl_screen;
        const auto& input = screen_state_->input_text;

        // Trim check: TS does `input.trim() === ''` to decide pop-vs-push.
        // An input of only whitespace is treated as "empty" for stash pop.
        const bool input_has_content = [&] {
            for (char c : input) {
                if (!std::isspace(static_cast<unsigned char>(c))) return true;
            }
            return false;
        }();

        if (!input_has_content && repl::HasStashedPrompt(screen_state_)) {
            // Pop stash: restore stashed text + pasted contents.
            std::unordered_map<int, ::cc::core::ImageBlock> restored_images;
            std::unordered_map<int, std::string> restored_texts;
            repl::RestoreStashedPrompt(screen_state_, &restored_images, &restored_texts);
            // Merge restored pasted contents back into engine maps.
            for (auto& [id, img] : restored_images) {
                pasted_contents_[id] = std::move(img);
            }
            for (auto& [id, txt] : restored_texts) {
                pasted_text_contents_[id] = std::move(txt);
            }
            PostRenderEvent();
            return true;
        }
        if (input_has_content) {
            // Push stash: save current input + referenced pasted contents,
            // then clear input.
            const auto refs = cc::utils::parse_references(input);
            std::unordered_map<int, ::cc::core::ImageBlock> ref_images;
            std::unordered_map<int, std::string> ref_texts;
            for (const auto& r : refs) {
                if (auto it = pasted_contents_.find(r.id);
                    it != pasted_contents_.end()) {
                    ref_images[r.id] = it->second;
                }
                if (auto it = pasted_text_contents_.find(r.id);
                    it != pasted_text_contents_.end()) {
                    ref_texts[r.id] = it->second;
                }
            }
            repl::StashCurrentPrompt(screen_state_,
                std::move(ref_images), std::move(ref_texts));
            // Clear input (TS: trackAndSetInput('') + setCursorOffset(0)).
            repl::set_prompt_input_text(screen_state_, {}, 0);
            // Clear pasted contents that were only referenced by the
            // stashed text (orphan cleanup will handle this naturally).
            PostRenderEvent();
            return true;
        }
        // Empty input with no stash → nothing to do.
        return false;
    }

    // ── Markdown hyperlink click-to-open ─────────────────────────────
    // TS REF: FullscreenLayout.tsx L630-667
    //   useLayoutEffect(() => {
    //     ink.onHyperlinkClick = url => {
    //       if (url.startsWith('file:')) openPath(fileURLToPath(url))
    //       else openBrowser(url)
    //     }
    //   })
    //
    // In fullscreen / alternate-screen mode, mouse tracking intercepts
    // all clicks before the terminal can natively open OSC 8 hyperlinks.
    // We compensate by detecting left-button releases at pixels that
    // carry a hyperlink ID (set by FTXUI's `hyperlink(url)` decorator
    // in markdown.cppm render_inlines → InlineTokenKind::Link).
    //
    // The Screen stores hyperlink URLs in hyperlinks_[], indexed by
    // the uint8_t ID written to Pixel.hyperlink during Render().  We
    // look up the URL via screen.Hyperlink(id) and route it through
    // cc::utils::try_open_hyperlink() (file: → open_file_path,
    // http(s): → open_browser).
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        // Left-button release = "click".  Using release (not press)
        // lets the user cancel by dragging the cursor off the link
        // before releasing — standard web/desktop link behavior.
        if (mouse.button == Mouse::Left &&
            mouse.motion == Mouse::Released) {
            if (auto* screen = screen_.load(std::memory_order_acquire)) {
                const int x = mouse.x;
                const int y = mouse.y;
                // Bounds check — screen dims may change between render
                // and event delivery (terminal resize race).
                if (x >= 0 && x < screen->dimx() &&
                    y >= 0 && y < screen->dimy()) {
                    const uint8_t link_id = screen->PixelAt(x, y).hyperlink;
                    if (link_id != 0) {
                        // id 0 = no hyperlink (Screen default).
                        const std::string& url = screen->Hyperlink(link_id);
                        if (!url.empty()) {
                            // Fire-and-forget: open the link.  The
                            // system call (open / xdg-open) returns
                            // quickly; we don't block on browser
                            // launch completion.
                            (void)cc::utils::try_open_hyperlink(url);
                            return true;  // event consumed
                        }
                    }
                }
            }
        }
    }

    const bool handled = repl_component_->OnEvent(event);
    if (handled) {
        // SL-11: any accepted keystroke that fills input retires the
        // next-action suggestion for this turn (RefreshAutocompleteSuggestions
        // also clears on non-empty input, but this is the unambiguous
        // reset for the accept-on-Return path).
        if (!screen_state_->input_text.empty()) {
            screen_state_->next_action_suggestion.reset();
        }
        RefreshAutocompleteSuggestions();

        // TS REF: PromptInput.tsx L1185-1200 — orphan cleanup.
        // Prune pasted_contents_ entries whose [Image #N] placeholder is
        // no longer in the input text (covers backspace-over-pill, Ctrl+U,
        // char-by-char deletion — any edit that drops the ref).
        // Also prune pasted_text_contents_ entries whose [...Truncated text #N]
        // ref is no longer in the input (same orphan scenarios for text pastes).
        if (!pasted_contents_.empty() || !pasted_text_contents_.empty()) {
            const auto refs = cc::utils::parse_references(screen_state_->input_text);
            std::unordered_set<int> referenced_ids;
            for (const auto& r : refs) referenced_ids.insert(r.id);
            for (auto it = pasted_contents_.begin(); it != pasted_contents_.end(); ) {
                if (!referenced_ids.contains(it->first)) {
                    it = pasted_contents_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = pasted_text_contents_.begin(); it != pasted_text_contents_.end(); ) {
                if (!referenced_ids.contains(it->first)) {
                    it = pasted_text_contents_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    return handled;
}

Component AppAdapter::ActiveChild() {
    return repl_component_;
}

}  // namespace cc::ui
