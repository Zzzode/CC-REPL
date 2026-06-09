/// @file agent_wizard.cppm
/// @brief Agent creation/edit wizard. 4 agent-specific steps built on top of
/// the UI11 cc.ui.wizard_dialog framework.
///
/// Replaces the old 6-step skeleton (same filename) and consolidates migration
/// of:
///   - src/components/agents/AgentEditor.tsx        (~700 lines, 4-tab editor)
///   - src/components/agents/ColorPicker.tsx        (avatar color)
///   - src/components/agents/ModelSelector.tsx      (model dropdown)
///   - src/components/agents/ToolSelector.tsx       (L+R tools picker, ~1,600)
///   - new-agent-creation/* subfolder              (4-step wizard flow)
///
/// 4 Steps (UI11 WizardComponent + 4 WizardStep entries):
///   Step 1: Basic         — name (required), short description,
///                           role tag picker (CustomSelect multi),
///                           avatar color
///   Step 2: Tools         — left: available tools (checkbox multi-select)
///                           right: selected tools (reorderable)
///                           + allowed/denied path scope editor
///   Step 3: Model+Prompt  — model dropdown (CustomSelect single)
///                           + temperature slider
///                           + system prompt (multiline code-highlighted)
///   Step 4: Perm+Summary  — default permission mode (Ask/Allow/Deny),
///                           confirm checkbox, full agent-definition summary
///
/// On wizard completion: fires on_save with the aggregated wizard state.
///
/// Reuses:
///   - cc.ui.wizard_dialog  (WizardComponent, WizardStep, WizardProviderProps)
///   - cc.ui.custom_select  (MakeSingleSelect / MakeMultiSelect for roles,
///                           tools, model)
///   - cc.ui.agents.shared_widgets (AgentAvatar, RoleTags, RunStats)
///   - cc.tools.agent_color_manager (8-color palette, explicit set)
module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.agents.agent_wizard;

import cc.ui.wizard_dialog;
import cc.ui.custom_select;
import cc.ui.agents.shared_widgets;
import cc.ui.agents.agent_cards;
import cc.tools.agent_color_manager;
import cc.utils.swarm_backends;

export namespace cc::ui::agents::wizard {
using namespace ftxui;

using cc::ui::wizard_dialog::WizardComponent;
using cc::ui::wizard_dialog::WizardProviderProps;
using cc::ui::wizard_dialog::WizardStep;
using cc::ui::custom_select::MakeMultiSelect;
using cc::ui::custom_select::MakeSingleSelect;
using cc::ui::custom_select::MakeCustomSelect;
using cc::ui::custom_select::SelectOption;
using cc::ui::custom_select::SelectMode;
using cc::ui::custom_select::CustomSelectOptions;
using cc::ui::custom_select::CustomSelectHandle;

using cards::AgentCardData;
using shared::AgentAvatar;
using shared::AvatarOptions;
using shared::RoleTags;
using shared::status_color;
using shared::agent_color_to_ftxui;
using cc::utils::swarm_backends::AgentColor;
using cc::tools::agent_color_manager::parse_color_name;
using cc::tools::agent_color_manager::assign_cycle_color;

// ============================================================
// Wizard State — the mutable draft gathered across all 4 steps.
// ============================================================

/// Permission default mode (Step 4).
enum class PermissionMode : std::uint8_t {
    Ask,     // Always ask the user before invoking a tool.
    Allow,   // Auto-allow unless explicitly denied.
    Deny,    // Auto-deny unless explicitly allowed.
};

[[nodiscard]] inline std::string_view permission_label(PermissionMode m) {
    switch (m) {
        case PermissionMode::Ask:   return "Ask (default)";
        case PermissionMode::Allow: return "Auto-allow";
        case PermissionMode::Deny:  return "Auto-deny";
    }
    return "?";
}

/// Aggregated wizard draft. Step components write into this struct via
/// shared_ptr so that the final summary can render everything.
struct WizardDraft {
    // Step 1
    std::string name;
    std::string description;
    std::vector<std::string> role_tags;
    std::optional<AgentColor> avatar_color;

    // Step 2
    std::vector<std::string> available_tools;    // all known tools
    std::vector<std::string> selected_tools;     // user picks
    std::vector<std::string> path_allowlist;     // glob patterns
    std::vector<std::string> path_denylist;      // glob patterns

    // Step 3
    std::string model = "claude-sonnet-4-20250514";
    double temperature = 1.0;   // 0.0 .. 2.0
    std::string system_prompt;

    // Step 4
    PermissionMode default_permission = PermissionMode::Ask;
    bool confirmed = false;

    // Editing mode: if true, the wizard is editing an existing agent.
    bool is_edit_mode = false;
    std::string editing_agent_id;
};

// ============================================================
// Canonical tool list (mirrors TS ToolSelector's all-tools registry).
// Kept here so Step 2 can populate Available → Selected without pulling in
// the full tools/ registry header.
// ============================================================

inline std::vector<SelectOption> canonical_tool_options() {
    return {
        {.label = "Bash",        .value = "bash",        .description = "Execute shell commands", .group = "System", .icon = "⌘"},
        {.label = "FileRead",    .value = "file_read",   .description = "Read files from disk",   .group = "Files",  .icon = "📄"},
        {.label = "FileWrite",   .value = "file_write",  .description = "Write/create files",     .group = "Files",  .icon = "✎"},
        {.label = "FileEdit",    .value = "file_edit",   .description = "Edit existing files",    .group = "Files",  .icon = "✐"},
        {.label = "Glob",        .value = "glob",        .description = "Pattern file search",    .group = "Search", .icon = "🔍"},
        {.label = "Grep",        .value = "grep",        .description = "Regex content search",   .group = "Search", .icon = "🔎"},
        {.label = "WebFetch",    .value = "web_fetch",   .description = "HTTP GET / scrape",      .group = "Web",    .icon = "🌐"},
        {.label = "WebSearch",   .value = "web_search",  .description = "Web search engine",      .group = "Web",    .icon = "🔍"},
        {.label = "MCPTool",     .value = "mcp",         .description = "MCP server tools",       .group = "MCP",    .icon = "🔌"},
        {.label = "Agent",       .value = "agent",       .description = "Spawn sub-agents",       .group = "Agent",  .icon = "🤖"},
        {.label = "Skill",       .value = "skill",       .description = "Run skill workflows",    .group = "Agent",  .icon = "⚡"},
        {.label = "TaskCreate",  .value = "task_create", .description = "Create background task", .group = "Agent",  .icon = "📋"},
        {.label = "TeamCreate",  .value = "team_create", .description = "Spawn a team",           .group = "Agent",  .icon = "👥"},
    };
}

inline std::vector<SelectOption> canonical_model_options() {
    return {
        {.label = "Claude Sonnet 4",   .value = "claude-sonnet-4-20250514",  .description = "Fast, balanced — default",      .group = "Sonnet",  .icon = "⚡"},
        {.label = "Claude Opus 4.6",   .value = "claude-opus-4-20250514",    .description = "Best reasoning, highest cost",  .group = "Opus",    .icon = "💎"},
        {.label = "Claude Haiku 4",    .value = "claude-haiku-4-20250514",   .description = "Cheapest, fastest — for agents",.group = "Haiku",   .icon = "🌱"},
        {.label = "Claude Code 0.6",   .value = "claude-code-0-20250609",    .description = "Code-specialized model",        .group = "Code",    .icon = "💻"},
    };
}

inline std::vector<SelectOption> canonical_role_options() {
    return {
        {.label = "Coder",      .value = "coder",      .description = "Write, edit, refactor code"},
        {.label = "Reviewer",   .value = "reviewer",   .description = "Review PRs and diffs"},
        {.label = "Researcher", .value = "researcher", .description = "Web research / fact-finding"},
        {.label = "Planner",    .value = "planner",    .description = "Break tasks into plans"},
        {.label = "Tester",     .value = "tester",     .description = "Write and run tests"},
        {.label = "DevOps",     .value = "devops",     .description = "CI/CD, deployments, infra"},
        {.label = "Writer",     .value = "writer",     .description = "Docs, changelogs, text"},
        {.label = "Analyst",    .value = "analyst",    .description = "Data analysis, metrics"},
    };
}

// ============================================================
// Step 1: Basic (name, description, role picker, avatar color)
// ============================================================

/// Build Step 1 content component.
[[nodiscard]] inline Component StepBasic(std::shared_ptr<WizardDraft> draft) {
    // Role multi-select handle.
    auto roles_opts = canonical_role_options();
    // Pre-select based on draft->role_tags.
    std::vector<std::string> defaults = draft->role_tags;
    CustomSelectOptions roles_cfg;
    roles_cfg.options = std::move(roles_opts);
    roles_cfg.mode = SelectMode::Multi;
    roles_cfg.default_values = defaults;
    roles_cfg.visible_count = 8;
    roles_cfg.enable_search = true;
    auto [roles_comp, roles_handle] = MakeCustomSelect(std::move(roles_cfg));

    // Track current color index (palette + 1 "auto" slot).
    static constexpr AgentColor kPalette[] = {
        AgentColor::Red, AgentColor::Blue, AgentColor::Green,  AgentColor::Yellow,
        AgentColor::Purple, AgentColor::Orange, AgentColor::Pink, AgentColor::Cyan,
    };
    constexpr int kColorCount = 8;
    auto color_idx = std::make_shared<int>(-1);   // -1 = auto
    if (draft->avatar_color) {
        for (int i = 0; i < kColorCount; ++i) {
            if (kPalette[i] == *draft->avatar_color) { *color_idx = i; break; }
        }
    }

    // Renderer for the whole step.
    auto name_comp = Input(&draft->name, "agent name (required)");
    auto desc_comp = Input(&draft->description, "short description — what this agent does");

    return Renderer(
        Container::Vertical({
            name_comp,
            desc_comp,
            roles_comp,
        }),
        [draft, roles_handle, color_idx] {
            // Persist multi-select back into draft on every render.
            draft->role_tags = roles_handle->SelectedValues();

            // Persist color.
            if (*color_idx < 0) draft->avatar_color.reset();
            else                draft->avatar_color = kPalette[*color_idx];

            // --- Name row ---
            auto name_row = hbox({
                text(" Name:        ") | color(Color::Cyan) | bold,
                hbox({
                    text(" " + draft->name + " ")
                        | bgcolor(draft->name.empty()
                                      ? Color::RGB(70, 40, 40)
                                      : Color::RGB(30, 40, 55)),
                }) | borderLight
                  | color(draft->name.empty() ? Color::Red : Color::Cyan) | flex,
                draft->name.empty() ? text(" required!") | color(Color::Red) : text(""),
            });

            // --- Description row ---
            auto desc_row = hbox({
                text(" Description: ") | color(Color::Cyan) | bold,
                text(" " + (draft->description.empty()
                                ? std::string("(press Tab then type)")
                                : draft->description) + " ")
                    | dim | borderLight | flex,
            });

            // --- Role tags preview ---
            auto tags_preview = hbox({
                text(" Roles:       ") | color(Color::Cyan) | bold,
                RoleTags(draft->role_tags, 6),
                draft->role_tags.empty() ? text(" (select below)") | dim : text(""),
            });

            // --- Color picker (ASCII palette) ---
            Elements swatches;
            swatches.push_back(text(" Color:       ") | color(Color::Cyan) | bold);
            // Auto swatch.
            {
                std::string glyph = (*color_idx < 0) ? "███" : "░░░";
                Color c = (*color_idx < 0) ? Color::GrayLight : Color::GrayDark;
                swatches.push_back(
                    hbox({
                        text("[") | dim,
                        text(glyph) | color(c),
                        text("]auto") | dim,
                    })
                );
            }
            swatches.push_back(text("  "));
            for (int i = 0; i < kColorCount; ++i) {
                Color c = agent_color_to_ftxui(kPalette[i]);
                std::string glyph = (*color_idx == i) ? "███" : "▓▓▓";
                swatches.push_back(
                    hbox({
                        text(glyph) | color(c),
                        text(" "),
                    })
                );
            }
            auto color_row = hbox(std::move(swatches));

            // --- Avatar live preview ---
            AvatarOptions av;
            av.name = draft->name.empty() ? "?" : draft->name;
            av.agent_type = draft->name.empty() ? "custom" : draft->name;
            av.force_general = false;
            av.size_cells = 4;
            // Override color if user picked one.
            if (draft->avatar_color) {
                cc::tools::agent_color_manager::set_agent_color(
                    av.agent_type, *draft->avatar_color);
            }
            auto preview = hbox({
                text(" Preview:     ") | color(Color::Cyan) | bold,
                AgentAvatar(av),
                text("  " + (draft->name.empty() ? std::string("NewAgent") : draft->name)),
            });

            return vbox({
                text(" Step 1 · Basic info") | bold | color(Color::Cyan),
                separator() | color(Color::Cyan),
                text(""),
                name_row,
                text(""),
                desc_row,
                text(""),
                tags_preview,
                text(""),
                color_row,
                text("  hint: press [←/→] to switch palette slot") | dim,
                text(""),
                preview,
                separator() | dim,
                text(" Role picker (multi-select — Space toggles, / to search):")
                    | bold | color(Color::Magenta),
                roles_comp->Render() | borderLight | flex,
            });
        }) | CatchEvent([color_idx](Event event) -> bool {
            // Color swatch navigation via Alt+Left/Right or just arrow keys
            // when focus is not in the text inputs (best-effort: handle h/l).
            if (event == Event::Character('h')) {
                *color_idx = (*color_idx - 1 + 9) % 9 - 1;
                if (*color_idx < -1) *color_idx = 7;
                return true;
            }
            if (event == Event::Character('l')) {
                *color_idx = (*color_idx + 1) % 9 - 1;
                if (*color_idx < -1) *color_idx = -1;
                return true;
            }
            return false;
        });
}

// ============================================================
// Step 2: Tools (Available → Selected + path scope editor)
// ============================================================

/// Build Step 2 content.
[[nodiscard]] inline Component StepTools(std::shared_ptr<WizardDraft> draft) {
    // Make sure available_tools is populated.
    if (draft->available_tools.empty()) {
        auto opts = canonical_tool_options();
        draft->available_tools.reserve(opts.size());
        for (auto& o : opts) draft->available_tools.push_back(o.value);
    }

    // Selected-tools multi-select with live write-back.
    auto tool_opts = canonical_tool_options();
    CustomSelectOptions tc;
    tc.options = std::move(tool_opts);
    tc.mode = SelectMode::Multi;
    tc.default_values = draft->selected_tools;
    tc.visible_count = 10;
    tc.enable_search = true;
    auto [tools_comp, tools_handle] = MakeCustomSelect(std::move(tc));

    // Path allow/deny lists are just textarea-like strings the user edits
    // via simple Input components (one line per pattern).
    std::string allow_joined;
    for (auto& p : draft->path_allowlist) { allow_joined += p; allow_joined += '\n'; }
    if (!allow_joined.empty()) allow_joined.pop_back();
    std::string deny_joined;
    for (auto& p : draft->path_denylist) { deny_joined += p; deny_joined += '\n'; }
    if (!deny_joined.empty()) deny_joined.pop_back();

    auto allow_store = std::make_shared<std::string>(std::move(allow_joined));
    auto deny_store  = std::make_shared<std::string>(std::move(deny_joined));
    auto allow_input = Input(allow_store.get(), "src/**  tests/**  (one per line)");
    auto deny_input  = Input(deny_store.get(),  "**/node_modules/**  **/.git/**");

    // Helpers to split/join.
    auto split_lines = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string ln;
        while (std::getline(iss, ln)) {
            // Trim whitespace.
            size_t a = ln.find_first_not_of(" \t");
            size_t b = ln.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            out.push_back(ln.substr(a, b - a + 1));
        }
        return out;
    };

    return Renderer(
        Container::Vertical({
            tools_comp,
            allow_input,
            deny_input,
        }),
        [draft, tools_handle, allow_store, deny_store, split_lines] {
            // Sync selection back to draft.
            draft->selected_tools = tools_handle->SelectedValues();
            draft->path_allowlist = split_lines(*allow_store);
            draft->path_denylist  = split_lines(*deny_store);

            // --- Selected summary chip row ---
            Elements sel_parts;
            for (size_t i = 0; i < draft->selected_tools.size(); ++i) {
                sel_parts.push_back(
                    hbox({
                        text(" " + draft->selected_tools[i] + " ")
                            | color(Color::Green) | borderLight,
                    })
                );
                sel_parts.push_back(text(" "));
            }
            if (sel_parts.empty()) sel_parts.push_back(text("(none selected)") | dim);

            // --- Path summary ---
            auto path_col = [](const std::string& title, Color c,
                               const std::vector<std::string>& rows) -> Element {
                Elements els = {
                    hbox({text(" " + title + " ") | bold | color(c)}),
                };
                if (rows.empty()) els.push_back(text("  (empty)") | dim);
                else {
                    int shown = std::min(4, static_cast<int>(rows.size()));
                    for (int i = 0; i < shown; ++i)
                        els.push_back(text("  " + rows[i]) | color(c));
                    int rest = static_cast<int>(rows.size()) - shown;
                    if (rest > 0)
                        els.push_back(text(std::format("  ... +{} more", rest)) | dim);
                }
                return vbox(std::move(els)) | borderLight | color(c);
            };

            return vbox({
                text(" Step 2 · Tools & path scope") | bold | color(Color::Magenta),
                separator() | color(Color::Magenta),
                text(""),
                hbox({
                    text(" Selected (") | bold,
                    text(std::format("{}", draft->selected_tools.size()))
                        | bold | color(Color::Green),
                    text("):  ") | bold,
                    hbox(std::move(sel_parts)) | flex,
                }),
                text(""),
                text(" Available tools (Space toggles, / filters):")
                    | bold | color(Color::Magenta),
                tools_comp->Render() | borderLight | size(HEIGHT, LESS_THAN, 12) | flex,
                separator() | dim,
                text(" Path scope (one glob per line):") | bold | color(Color::Blue),
                text(""),
                hbox({
                    vbox({
                        text(" Allow list:") | color(Color::Green),
                        allow_input->Render() | size(HEIGHT, LESS_THAN, 5) | flex | borderLight,
                    }) | flex,
                    text("  "),
                    vbox({
                        text(" Deny list:") | color(Color::Red),
                        deny_input->Render() | size(HEIGHT, LESS_THAN, 5) | flex | borderLight,
                    }) | flex,
                }),
                text(""),
                hbox({
                    path_col("Allow",  Color::Green, draft->path_allowlist) | flex,
                    text(" "),
                    path_col("Deny",   Color::Red,   draft->path_denylist)  | flex,
                }),
            });
        });
}

// ============================================================
// Step 3: Model + Prompt (dropdown + temperature slider + system prompt)
// ============================================================

/// Build Step 3 content.
[[nodiscard]] inline Component StepModelPrompt(std::shared_ptr<WizardDraft> draft) {
    // Model single-select.
    auto model_opts = canonical_model_options();
    CustomSelectOptions mc;
    mc.options = std::move(model_opts);
    mc.mode = SelectMode::Single;
    mc.default_value = draft->model;
    mc.visible_count = 6;
    mc.enable_search = false;
    auto [model_comp, model_handle] = MakeCustomSelect(std::move(mc));

    // Temperature slider: rendered as a manual gauge; + / - keys adjust.
    auto temp = std::make_shared<double>(draft->temperature);

    // System prompt input.
    auto prompt_store = std::make_shared<std::string>(draft->system_prompt);
    auto prompt_input = Input(prompt_store.get(),
        "# System prompt — instructions this agent always follows\n"
        "# Example:\n"
        "You are a senior C++ engineer. Prefer range-based for loops.\n"
        "Always run clang-format before committing.");

    return Renderer(
        Container::Vertical({
            model_comp,
            prompt_input,
        }),
        [draft, model_handle, temp, prompt_store] {
            // Sync back.
            auto mv = model_handle->SelectedValues();
            if (!mv.empty()) draft->model = mv[0];
            draft->temperature = std::clamp(*temp, 0.0, 2.0);
            draft->system_prompt = *prompt_store;

            // Model picker row.
            auto current = draft->model;
            auto model_row = vbox({
                text(" Model: ") | bold | color(Color::Yellow),
                text(""),
                model_comp->Render() | borderLight | size(HEIGHT, LESS_THAN, 8) | flex,
                text(""),
                hbox({
                    text("  Selected: ") | dim,
                    text(current) | color(Color::Yellow) | bold,
                }),
            });

            // Temperature gauge.
            int bar_w = 30;
            int filled = static_cast<int>((draft->temperature / 2.0) * bar_w);
            std::string bar(filled, '█');
            bar += std::string(bar_w - filled, '░');
            // Color transition: blue (cold) -> green -> red (hot).
            Color tcolor = Color::Blue;
            if (draft->temperature > 0.5) tcolor = Color::Green;
            if (draft->temperature > 1.2) tcolor = Color::Yellow;
            if (draft->temperature > 1.7) tcolor = Color::Red;

            auto temp_row = vbox({
                hbox({
                    text(" Temperature: ") | bold | color(tcolor),
                    text(bar) | color(tcolor),
                    text(std::format(" {:.2f} ", draft->temperature)) | bold,
                }),
                text("  hint: [+] / [-] to adjust (0.0 = deterministic, 2.0 = wild)") | dim,
            });

            // System prompt block (code-style).
            auto prompt_view = vbox({
                text(" System prompt:") | bold | color(Color::Cyan),
                text(""),
                hbox({
                    text(" │") | dim | color(Color::Cyan),
                    prompt_input->Render()
                        | bgcolor(Color::RGB(20, 25, 35))
                        | size(HEIGHT, LESS_THAN, 10) | flex | borderLight,
                }),
                text(std::format("  {} chars", draft->system_prompt.size())) | dim,
            });

            return vbox({
                text(" Step 3 · Model + system prompt") | bold | color(Color::Yellow),
                separator() | color(Color::Yellow),
                text(""),
                model_row | borderLight,
                text(""),
                temp_row,
                text(""),
                prompt_view,
            });
        }) | CatchEvent([temp](Event event) -> bool {
            if (event == Event::Character('+') || event == Event::Character('=')) {
                *temp = std::min(2.0, *temp + 0.1);
                return true;
            }
            if (event == Event::Character('-') || event == Event::Character('_')) {
                *temp = std::max(0.0, *temp - 0.1);
                return true;
            }
            if (event == Event::Character('0')) { *temp = 0.0; return true; }
            if (event == Event::Character('1')) { *temp = 1.0; return true; }
            if (event == Event::Character('2')) { *temp = 2.0; return true; }
            return false;
        });
}

// ============================================================
// Step 4: Permissions + Summary
// ============================================================

/// Build Step 4 content.
[[nodiscard]] inline Component StepPermissions(std::shared_ptr<WizardDraft> draft) {
    auto perm = std::make_shared<int>(static_cast<int>(draft->default_permission));
    auto confirmed = std::make_shared<bool>(draft->confirmed);

    return Renderer([draft, perm, confirmed] {
        draft->default_permission = static_cast<PermissionMode>(
            std::clamp(*perm, 0, 2));
        draft->confirmed = *confirmed;

        // --- Permission radio row ---
        auto pill = [&](PermissionMode m, Color c) {
            bool on = draft->default_permission == m;
            std::string mark = on ? "● " : "○ ";
            auto t = hbox({
                text(mark) | color(c),
                text(std::string(permission_label(m))) | (on ? bold : dim),
            });
            if (on) t = t | borderLight | color(c);
            return t;
        };

        auto perm_row = vbox({
            text(" Default permission mode:") | bold | color(Color::Cyan),
            text(""),
            hbox({
                pill(PermissionMode::Ask,   Color::Yellow) | flex,
                text("  "),
                pill(PermissionMode::Allow, Color::Green)  | flex,
                text("  "),
                pill(PermissionMode::Deny,  Color::Red)    | flex,
            }),
            text("  hint: [1] Ask  [2] Allow  [3] Deny") | dim,
        });

        // --- Confirm checkbox ---
        auto confirm_row = hbox({
            text(*confirmed ? "[✓] " : "[ ] ") | color(Color::Green) | bold,
            text("I have reviewed this agent configuration and I am ready to ")
                | dim,
            text(draft->is_edit_mode ? "save changes." : "create this agent.")
                | (*confirmed ? color(Color::Green) | bold : dim),
        });

        // --- Summary block ---
        Elements summary_rows = {
            hbox({text(" Name:       ") | dim,
                  text(draft->name.empty() ? "(missing!)" : draft->name) | bold
                      | color(draft->name.empty() ? Color::Red : Color::White)}),
            hbox({text(" Description:") | dim,
                  text(draft->description.empty() ? std::string("(none)") : draft->description)}),
            hbox({text(" Roles:      ") | dim,
                  RoleTags(draft->role_tags, 5)}),
            hbox({text(" Tools:      ") | dim,
                  text(std::format("{} selected", draft->selected_tools.size()))
                      | color(Color::Green)}),
            hbox({text(" Model:      ") | dim,
                  text(draft->model) | color(Color::Yellow)}),
            hbox({text(" Temp:       ") | dim,
                  text(std::format("{:.2f}", draft->temperature))}),
            hbox({text(" Permission: ") | dim,
                  text(std::string(permission_label(draft->default_permission)))
                      | color(Color::Cyan)}),
            hbox({text(" Paths:      ") | dim,
                  text(std::format("{} allow · {} deny",
                                   draft->path_allowlist.size(),
                                   draft->path_denylist.size()))}),
            hbox({text(" Prompt:     ") | dim,
                  text(std::format("{} chars", draft->system_prompt.size())) | dim}),
        };
        auto summary = vbox({
            text(" Configuration summary") | bold | color(Color::Cyan),
            separator() | color(Color::Cyan),
            vbox(std::move(summary_rows)),
        }) | borderLight;

        // --- "Ready?" banner ---
        bool ready = !draft->name.empty() && draft->confirmed;
        Element banner;
        if (ready) {
            banner = hbox({
                text(" ✓ ") | color(Color::Green) | bold,
                text(draft->is_edit_mode
                         ? "Ready to save. Press Enter → update agent registry."
                         : "Ready to create. Press Enter → write to agent registry.")
                    | color(Color::Green) | bold,
            }) | bgcolor(Color::RGB(20, 50, 30)) | borderLight | color(Color::Green);
        } else {
            banner = hbox({
                text(" ! ") | color(Color::Yellow) | bold,
                text((draft->name.empty()
                          ? "Name is required. "
                          : "") +
                     (!draft->confirmed
                          ? "Tick the confirmation box to proceed."
                          : ""))
                    | color(Color::Yellow),
            }) | bgcolor(Color::RGB(60, 50, 15)) | borderLight | color(Color::Yellow);
        }

        return vbox({
            text(" Step 4 · Permissions & summary") | bold | color(Color::Cyan),
            separator() | color(Color::Cyan),
            text(""),
            perm_row,
            separator() | dim,
            summary,
            text(""),
            confirm_row,
            text(""),
            banner,
            text(""),
            text(" hint: [Space] / [c] toggle confirm  [1..3] permission mode") | dim,
        });
    }) | CatchEvent([perm, confirmed](Event event) -> bool {
        if (event == Event::Character('1')) { *perm = 0; return true; }
        if (event == Event::Character('2')) { *perm = 1; return true; }
        if (event == Event::Character('3')) { *perm = 2; return true; }
        if (event == Event::Character(' ') || event == Event::Character('c')) {
            *confirmed = !*confirmed;
            return true;
        }
        if (event == Event::ArrowRight || event == Event::ArrowDown) {
            *perm = (*perm + 1) % 3;
            return true;
        }
        if (event == Event::ArrowLeft || event == Event::ArrowUp) {
            *perm = (*perm + 2) % 3;
            return true;
        }
        return false;
    });
}

// ============================================================
// Top-level AgentWizard factory
// ============================================================

/// Factory options.
struct AgentWizardOptions {
    /// If set, the wizard opens in EDIT mode pre-populated with this agent's
    /// data; otherwise it starts blank (CREATE mode).
    std::optional<cards::AgentCardData> edit_agent;

    std::function<void(const WizardDraft& draft)> on_save;
    std::function<void()> on_cancel;
};

/// Build the full 4-step agent wizard component. Wires WizardComponent with
/// 4 WizardStep entries; the final Enter on step 4 fires `on_save`.
[[nodiscard]] inline Component AgentWizard(AgentWizardOptions opts) {
    auto draft = std::make_shared<WizardDraft>();

    // Pre-populate if editing.
    if (opts.edit_agent) {
        const auto& e = *opts.edit_agent;
        draft->is_edit_mode = true;
        draft->editing_agent_id = e.id;
        draft->name = e.name;
        draft->description = e.description_long.empty()
                                 ? e.description
                                 : e.description_long;
        draft->role_tags = e.role_tags;
        draft->selected_tools = e.tools;
        if (e.model_override) draft->model = *e.model_override;
        // Permission mode parsing.
        if (e.permission_mode) {
            std::string p = *e.permission_mode;
            if (p == "Allow")       draft->default_permission = PermissionMode::Allow;
            else if (p == "Deny")   draft->default_permission = PermissionMode::Deny;
            else                    draft->default_permission = PermissionMode::Ask;
        }
        if (!e.agent_type.empty()) {
            auto c = cc::tools::agent_color_manager::get_agent_color(e.agent_type);
            if (c) draft->avatar_color = c;
        }
    }

    WizardProviderProps props;
    props.title = opts.edit_agent ? "Edit Agent" : "Create New Agent";
    props.show_step_counter = true;
    props.on_cancel = std::move(opts.on_cancel);
    props.on_complete = [draft, cb = std::move(opts.on_save)] {
        if (cb) cb(*draft);
    };

    // Wire 4 steps.
    props.steps.push_back({
        .id = "basic",
        .title = "Basic",
        .description = "Name, description, roles, avatar color",
        .create_content = [draft] { return StepBasic(draft); },
    });
    props.steps.push_back({
        .id = "tools",
        .title = "Tools",
        .description = "Pick enabled tools + path scope",
        .create_content = [draft] { return StepTools(draft); },
    });
    props.steps.push_back({
        .id = "model",
        .title = "Model & Prompt",
        .description = "Model, temperature, system prompt",
        .create_content = [draft] { return StepModelPrompt(draft); },
    });
    props.steps.push_back({
        .id = "summary",
        .title = "Permissions & Summary",
        .description = "Default permission mode, confirm, review",
        .create_content = [draft] { return StepPermissions(draft); },
    });

    return WizardComponent(std::move(props));
}

} // namespace cc::ui::agents::wizard
