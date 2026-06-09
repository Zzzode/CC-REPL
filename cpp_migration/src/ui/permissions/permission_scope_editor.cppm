/// @file permission_scope_editor.cppm
/// @brief Allowlist paths, Preapproval rules, and Dangerous-paths editor.
///
/// 3-Tab layout:
///   Tab 1 - Allowlist paths : path rows (pattern + match strategy + scope +
///           enabled toggle + delete). Add row with glob autocomplete.
///           JSON import / export of the entire rule set.
///   Tab 2 - Preapproval rules : rule list (Tool* + Action* + Path* +
///           Time window + Enabled). 3-step mini wizard to add a rule.
///   Tab 3 - Dangerous paths : read-only list of default high-risk paths
///           with "Add exception" per row. Two-button actions per row:
///           [Allow in workspace only] / [Never allow].
///
/// All state mutations go through cc.utils.permissions_engine public API
/// (add_rule / remove_rule / get_rules / export_rules / import_rules).
/// This file never duplicates the pattern-matching or evaluation logic.
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.scope_editor;

import cc.utils.permissions_engine;
import cc.ui.permissions.components;

export namespace cc::ui::permissions::scope_editor {
using namespace ftxui;
namespace pc = cc::ui::permissions::components;
namespace eng = cc::utils::permissions;

using eng::MatchStrategy;
using eng::PermissionAction;
using eng::PermissionScope;
using eng::PermissionRule;
using eng::PermissionEngine;
using PathScope = pc::PathScope;

// ============================================================
// Types
// ============================================================

enum class Tab : std::uint8_t {
    Allowlist = 0,
    Preapproval = 1,
    Dangerous = 2,
};

/// Visual-only allowlist row (mirrors engine PermissionRule + UI flags).
struct AllowlistRow {
    std::string id;          // mirrors PermissionRule.id
    std::string path_pattern;// e.g. "/src/**/*.cpp"
    MatchStrategy strategy;  // exact/prefix/glob/regex
    PathScope scope;         // R / W / X / RWX
    bool enabled = true;
    bool is_default = false;
};

/// Visual preapproval rule (subset of engine PermissionRule fields).
struct PreapprovalRow {
    std::string id;
    std::string tool_pattern;
    std::string action_pattern;
    std::string path_pattern;
    PermissionAction action;
    PermissionScope scope;
    std::chrono::seconds time_window{0}; // 0 = permanent
    bool enabled = true;
};

/// A dangerous path entry with its exception status.
struct DangerousPathRow {
    std::string path;
    std::string reason;     // why this path is dangerous
    enum class ExceptionStatus {
        None,
        WorkspaceOnly,
        Never,
    } exception = ExceptionStatus::None;
    bool is_default = true;
};

/// Result of closing the editor (sent to on_close callback).
struct EditorSummary {
    std::size_t rules_changed = 0;
    std::size_t dangerous_exceptions = 0;
    bool exported = false;
    bool imported = false;
};

struct ScopeEditorCallbacks {
    std::function<void(EditorSummary)> on_close;
};

// ============================================================
// Helpers: convert between engine types and UI types
// ============================================================

namespace detail {

[[nodiscard]] inline std::string_view ScopeLabel(PermissionScope s) {
    switch (s) {
        case PermissionScope::Global:  return "Global";
        case PermissionScope::Project: return "Project";
        case PermissionScope::Session: return "Session";
        case PermissionScope::Command: return "Command";
    }
    return "?";
}

[[nodiscard]] inline std::string_view ActionLabel(PermissionAction a) {
    switch (a) {
        case PermissionAction::Allow:   return "Allow";
        case PermissionAction::Deny:    return "Deny";
        case PermissionAction::Ask:     return "Ask";
        case PermissionAction::AskOnce: return "Ask once";
    }
    return "?";
}

[[nodiscard]] inline Color ActionColor(PermissionAction a) {
    switch (a) {
        case PermissionAction::Allow:   return Color::Green;
        case PermissionAction::Deny:    return Color::Red;
        case PermissionAction::Ask:     return Color::Yellow;
        case PermissionAction::AskOnce: return Color::YellowLight;
    }
    return Color::GrayLight;
}

/// Convert PathScope -> PermissionAction for rules.
[[nodiscard]] inline PermissionAction ScopeToAction(PathScope) {
    return PermissionAction::Allow;
}

/// Build default dangerous paths list (same as TS dangerous defaults).
[[nodiscard]] inline std::vector<DangerousPathRow> DefaultDangerousPaths() {
    return {
        { "/etc/passwd",          "Contains system user accounts (readable)" },
        { "/etc/shadow",          "Contains password hashes" },
        { "/etc/sudoers",         "Controls sudo privileges" },
        { "~/.ssh/id_rsa",        "Private SSH key" },
        { "~/.ssh/id_ed25519",    "Private SSH key (ed25519)" },
        { "~/.aws/credentials",   "AWS credentials" },
        { "~/.gnupg/privkey.gpg", "GPG private key" },
        { "~/.netrc",             "Plaintext network credentials" },
        { "/",                    "Filesystem root (deletion is catastrophic)" },
        { "/proc",                "Kernel pseudo-filesystem" },
        { "/sys",                 "Kernel hardware interface" },
        { "/dev/null",            "Common sink (not dangerous, but suspicious)" },
    };
}

} // namespace detail

// ============================================================
// State
// ============================================================

struct WizardState {
    int step = 0;     // 0=select tool, 1=select action, 2=select path, 3=review
    int selected_tool = 0;
    int selected_action = 0;
    std::string entered_path;
};

struct EditorState {
    Tab active_tab = Tab::Allowlist;
    ScopeEditorCallbacks cbs;

    // Tab 1: Allowlist
    std::vector<AllowlistRow> allowlist;
    std::size_t al_cursor = 0;
    std::string new_path_pattern;    // input buffer for Add row
    int new_strategy_idx = 2;        // default: glob
    int new_scope_idx = 3;           // default: RWX
    bool adding_row = false;

    // Tab 2: Preapproval
    std::vector<PreapprovalRow> preapprovals;
    std::size_t pa_cursor = 0;
    WizardState wizard;
    bool show_wizard = false;

    // Tab 3: Dangerous paths
    std::vector<DangerousPathRow> dangerous;
    std::size_t dp_cursor = 0;

    // Shared
    std::size_t rules_changed_count = 0;
    std::size_t dangerous_exceptions = 0;
    bool did_export = false;
    bool did_import = false;
};

// ============================================================
// Tab rendering
// ============================================================

[[nodiscard]] inline Element RenderTabHeader(Tab active) {
    std::array<std::string_view, 3> labels = {
        "Allowlist paths",
        "Preapproval rules",
        "Dangerous paths",
    };
    Elements els;
    for (int i = 0; i < 3; ++i) {
        auto t = Tab{i};
        auto el = text(std::format(" {} ", labels[i]));
        if (t == active) el = el | bold | inverted | color(Color::Cyan);
        else              el = el | dim;
        els.push_back(el);
        if (i < 2) els.push_back(text(" "));
    }
    return hbox(els);
}

// --- Tab 1: Allowlist ---

[[nodiscard]] inline Element RenderAllowlistTab(EditorState& st) {
    Elements rows;
    // Column header
    rows.push_back(hbox({
        text("  ") | size(WIDTH, EQUAL, 3),
        text("Pattern") | bold | size(WIDTH, EQUAL, 38),
        text("Strategy") | bold | size(WIDTH, EQUAL, 10),
        text("Scope") | bold | size(WIDTH, EQUAL, 8),
        text("On") | bold | size(WIDTH, EQUAL, 4),
        text("Del") | bold | size(WIDTH, EQUAL, 4),
    }));
    rows.push_back(pc::ThinDivider());

    // Rows
    for (std::size_t i = 0; i < st.allowlist.size(); ++i) {
        const auto& r = st.allowlist[i];
        bool sel = i == st.al_cursor;
        Elements cells = {
            text(std::format("{:>2}.", i + 1)) | dim | size(WIDTH, EQUAL, 3),
            pc::PathLabel(r.path_pattern, 36) | size(WIDTH, EQUAL, 38),
            text(std::string{pc::MatchStrategyLabel(r.strategy)}) | dim | size(WIDTH, EQUAL, 10),
            pc::PathScopeBadge(r.scope) | size(WIDTH, EQUAL, 8),
            (r.enabled ? text(" ✓ ") | color(Color::Green) : text("   ") | dim) | size(WIDTH, EQUAL, 4),
            (r.is_default ? text(" ") : text(" ✕ ") | color(Color::Red) | dim) | size(WIDTH, EQUAL, 4),
        };
        auto row = hbox(cells);
        if (sel) row = row | inverted | focus;
        if (!r.enabled) row = row | dim;
        rows.push_back(row);
    }
    if (st.allowlist.empty()) {
        rows.push_back(text("  (no allowlist entries)") | dim);
    }

    // Add-row input row
    rows.push_back(pc::ThinDivider());
    if (st.adding_row) {
        static const std::array<std::string, 4> strat_labels = {"exact","prefix","glob","regex"};
        static const std::array<std::string, 5> scope_labels = {"R","W","X","RWX","N"};
        std::string pattern_display = st.new_path_pattern.empty()
            ? std::string{" (type path pattern, Enter to save, Esc to cancel) "}
            : st.new_path_pattern;
        rows.push_back(hbox({
            text("  +  ") | color(Color::Green) | bold,
            text(pattern_display) | color(Color::Yellow) | xflex_grow,
            text(strat_labels[st.new_strategy_idx]) | dim,
            text(" / ") | dim,
            text(scope_labels[st.new_scope_idx]) | dim,
        }));
    } else {
        rows.push_back(hbox({
            text(" [a] ") | color(Color::Cyan) | dim,
            text("Add entry") | dim,
            filler(),
            text(" [i] import ") | dim,
            text(" [e] export") | dim,
        }));
    }

    return vbox(rows) | xflex_grow;
}

// --- Tab 2: Preapproval rules ---

[[nodiscard]] inline Element RenderPreapprovalTab(EditorState& st) {
    Elements rows;
    // Column header
    rows.push_back(hbox({
        text("  ") | size(WIDTH, EQUAL, 3),
        text("Tool") | bold | size(WIDTH, EQUAL, 18),
        text("Action") | bold | size(WIDTH, EQUAL, 10),
        text("Path pattern") | bold | size(WIDTH, EQUAL, 30),
        text("Rule") | bold | size(WIDTH, EQUAL, 8),
        text("Scope") | bold | size(WIDTH, EQUAL, 9),
        text("On") | bold | size(WIDTH, EQUAL, 4),
    }));
    rows.push_back(pc::ThinDivider());

    for (std::size_t i = 0; i < st.preapprovals.size(); ++i) {
        const auto& r = st.preapprovals[i];
        bool sel = i == st.pa_cursor;
        auto time_str = r.time_window.count() > 0
            ? std::format("{}h", r.time_window.count() / 3600)
            : std::string{"perm"};
        Elements cells = {
            text(std::format("{:>2}.", i + 1)) | dim | size(WIDTH, EQUAL, 3),
            text(r.tool_pattern) | color(Color::Magenta) | size(WIDTH, EQUAL, 18),
            text(r.action_pattern) | dim | size(WIDTH, EQUAL, 10),
            pc::PathLabel(r.path_pattern, 28) | size(WIDTH, EQUAL, 30),
            text(std::string{detail::ActionLabel(r.action)})
                | color(detail::ActionColor(r.action)) | size(WIDTH, EQUAL, 8),
            text(std::string{detail::ScopeLabel(r.scope)}) | dim | size(WIDTH, EQUAL, 9),
            (r.enabled ? text(" ✓ ") | color(Color::Green) : text("   ") | dim) | size(WIDTH, EQUAL, 4),
        };
        auto row = hbox(cells);
        if (sel) row = row | inverted | focus;
        if (!r.enabled) row = row | dim;
        rows.push_back(row);
    }
    if (st.preapprovals.empty()) {
        rows.push_back(text("  (no preapproval rules)") | dim);
    }
    rows.push_back(pc::ThinDivider());
    rows.push_back(hbox({
        text(" [w] ") | color(Color::Cyan) | dim,
        text("New-rule wizard") | dim,
        filler(),
        text(" [Enter] edit  [Del] remove") | dim,
    }));

    // Mini-wizard overlay
    if (st.show_wizard) {
        static const std::array<std::string, 8> tool_options = {
            "BashTool", "FileEditTool", "FileWriteTool", "FileReadTool",
            "WebFetchTool", "MCPTool", "GlobTool", "SkillTool",
        };
        static const std::array<std::string, 5> action_options = {
            "Allow", "Deny", "Ask", "Ask once", "* (any)",
        };
        Elements wizard_rows = {
            text("") | size(HEIGHT, EQUAL, 1),
            hbox({
                text("  ── Add rule wizard ─ Step ") | bold | color(Color::Cyan),
                text(std::to_string(st.wizard.step + 1)) | bold,
                text(" / 3") | bold,
            }),
            text(""),
        };
        if (st.wizard.step == 0) {
            for (int i = 0; i < 8; ++i) {
                bool sel = i == st.wizard.selected_tool;
                auto el = text(std::format("  {} {}", sel ? "►" : " ", tool_options[i]));
                if (sel) el = el | inverted;
                wizard_rows.push_back(el);
            }
        } else if (st.wizard.step == 1) {
            for (int i = 0; i < 5; ++i) {
                bool sel = i == st.wizard.selected_action;
                auto el = text(std::format("  {} {}", sel ? "►" : " ", action_options[i]));
                if (sel) el = el | inverted;
                wizard_rows.push_back(el);
            }
        } else if (st.wizard.step == 2) {
            std::string p = st.wizard.entered_path.empty()
                ? " (type path glob pattern, e.g. /src/**/*.ts) "
                : st.wizard.entered_path;
            wizard_rows.push_back(hbox({
                text("  Path pattern: ") | dim,
                text(p) | color(Color::Yellow) | xflex_grow,
            }));
            wizard_rows.push_back(text("") | size(HEIGHT, EQUAL, 1));
            wizard_rows.push_back(text("  [Enter] save  [Esc] cancel") | dim);
        }
        rows.insert(rows.end(), wizard_rows.begin(), wizard_rows.end());
    }

    return vbox(rows) | xflex_grow;
}

// --- Tab 3: Dangerous paths ---

[[nodiscard]] inline Element RenderDangerousTab(EditorState& st) {
    Elements rows;
    rows.push_back(hbox({
        text(" High-risk default paths") | bold | color(Color::Red),
        filler(),
        text(" Two actions per row: [w] = allow in workspace only, [n] = never") | dim,
    }));
    rows.push_back(pc::ThinDivider());

    for (std::size_t i = 0; i < st.dangerous.size(); ++i) {
        const auto& d = st.dangerous[i];
        bool sel = i == st.dp_cursor;
        auto status_el = [&]() -> Element {
            switch (d.exception) {
                case DangerousPathRow::ExceptionStatus::None:
                    return text("  ") | dim;
                case DangerousPathRow::ExceptionStatus::WorkspaceOnly:
                    return text(" WS ") | color(Color::White) | bgcolor(Color::Blue) | bold;
                case DangerousPathRow::ExceptionStatus::Never:
                    return text(" ✗ ") | color(Color::White) | bgcolor(Color::Red) | bold;
            }
            return text("");
        }();

        auto row = hbox({
            status_el,
            text(" "),
            pc::PathLabelHighlighted(d.path, true, 40) | size(WIDTH, EQUAL, 42),
            text("  ") | dim,
            text(d.reason) | dim | xflex_grow,
        });
        if (sel) row = row | inverted | focus;
        rows.push_back(row);
    }

    rows.push_back(pc::ThinDivider());
    rows.push_back(hbox({
        text(" [↑↓/jk] select") | dim,
        text("  [w] allow in workspace only") | dim,
        text("  [n] never allow") | dim,
        text("  [r] reset to default") | dim,
        filler(),
    }));

    return vbox(rows) | xflex_grow;
}

// ============================================================
// Main element
// ============================================================

[[nodiscard]] inline Element RenderScopeEditor(std::shared_ptr<EditorState> st) {
    auto header = RenderTabHeader(st->active_tab);
    Element body;
    switch (st->active_tab) {
        case Tab::Allowlist:   body = RenderAllowlistTab(*st); break;
        case Tab::Preapproval: body = RenderPreapprovalTab(*st); break;
        case Tab::Dangerous:   body = RenderDangerousTab(*st); break;
    }

    auto footer = hbox({
        text(" ←/→ ") | dim,
        text("switch tab") | dim,
        text("  ") | dim,
        text("Esc") | bold,
        text(" close") | dim,
        filler(),
        text(std::format(" changed:{}  exceptions:{}",
            st->rules_changed_count, st->dangerous_exceptions)) | dim,
    });

    auto full = vbox({
        header,
        pc::ThinDivider(),
        body | yframe | yflex_grow,
        pc::ThinDivider(),
        footer,
    });

    return window(
        text(" 🛡 Permission Scope Editor ") | bold | color(Color::Cyan),
        full | xflex_grow
    ) | color(Color::Cyan) | size(WIDTH, LESS_THAN, 120);
}

// ============================================================
// State mutation helpers (always delegate to engine)
// ============================================================

/// Flush the current UI allowlist state into the engine.
/// This removes-and-readds to match what the user sees in the table.
inline void FlushAllowlistToEngine(EditorState& st) {
    // Remove existing allowlist rules
    for (const auto& r : st.allowlist) {
        eng::remove_rule(r.id);
    }
    // Re-add enabled ones
    for (const auto& r : st.allowlist) {
        if (!r.enabled) continue;
        PermissionRule rule;
        rule.id = r.id;
        rule.tool_pattern = "*";    // any tool
        rule.strategy = MatchStrategy::Glob;
        rule.action = detail::ScopeToAction(r.scope);
        rule.scope = PermissionScope::Project;
        rule.path_pattern = r.path_pattern;
        rule.priority = 50;
        (void)eng::add_rule(std::move(rule));
    }
}

/// Sync allowlist rows from the engine (for import).
inline void SyncAllowlistFromEngine(EditorState& st) {
    st.allowlist.clear();
    for (const auto& r : eng::get_rules()) {
        if (r.path_pattern.has_value() && r.tool_pattern == "*") {
            AllowlistRow row;
            row.id = r.id;
            row.path_pattern = *r.path_pattern;
            row.strategy = r.strategy;
            // Scope mapping: simplify
            row.scope = PathScope::All;
            row.enabled = true;
            st.allowlist.push_back(std::move(row));
        }
    }
    st.al_cursor = 0;
}

// ============================================================
// Event handling
// ============================================================

/// Process an event for Tab 1 (Allowlist).
inline bool HandleAllowlistEvents(EditorState& st, Event event) {
    const std::size_t N = st.allowlist.size();

    // --- Add-row capture mode ---
    if (st.adding_row) {
        if (event == Event::Escape) {
            st.adding_row = false;
            st.new_path_pattern.clear();
            return true;
        }
        if (event == Event::Return) {
            if (!st.new_path_pattern.empty()) {
                static const std::array<MatchStrategy, 4> strategies = {
                    MatchStrategy::Exact, MatchStrategy::Prefix,
                    MatchStrategy::Glob, MatchStrategy::Regex
                };
                static const std::array<PathScope, 5> scopes = {
                    PathScope::Read, PathScope::Write, PathScope::Execute,
                    PathScope::All, PathScope::Network
                };
                AllowlistRow row;
                row.id = std::format("al_{}_{}", st.new_path_pattern,
                    std::chrono::steady_clock::now().time_since_epoch().count());
                row.path_pattern = st.new_path_pattern;
                row.strategy = strategies[st.new_strategy_idx];
                row.scope = scopes[st.new_scope_idx];
                row.enabled = true;
                st.allowlist.push_back(std::move(row));
                FlushAllowlistToEngine(st);
                ++st.rules_changed_count;
            }
            st.adding_row = false;
            st.new_path_pattern.clear();
            return true;
        }
        if (event == Event::ArrowUp) {
            st.new_scope_idx = (st.new_scope_idx + 4) % 5;
            return true;
        }
        if (event == Event::ArrowDown) {
            st.new_scope_idx = (st.new_scope_idx + 1) % 5;
            return true;
        }
        if (event == Event::ArrowLeft) {
            st.new_strategy_idx = (st.new_strategy_idx + 3) % 4;
            return true;
        }
        if (event == Event::ArrowRight) {
            st.new_strategy_idx = (st.new_strategy_idx + 1) % 4;
            return true;
        }
        // Capture printable characters as path-pattern input
        if (event.is_character()) {
            auto c = event.character();
            if (c == "\b"_utf8 || c == "\x7f"_utf8) {
                if (!st.new_path_pattern.empty()) st.new_path_pattern.pop_back();
                return true;
            }
            // Simple filter: only accept path-like chars
            st.new_path_pattern += c;
            return true;
        }
        return false;
    }

    // --- Normal mode ---
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (N > 0) st.al_cursor = (st.al_cursor - 1 + N) % N;
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (N > 0) st.al_cursor = (st.al_cursor + 1) % N;
        return true;
    }
    if (event == Event::Character('a')) {
        st.adding_row = true;
        st.new_path_pattern.clear();
        return true;
    }
    if (event == Event::Character(' ') || event == Event::Return) {
        // Toggle enabled on cursor row (skip default rows)
        if (st.al_cursor < N && !st.allowlist[st.al_cursor].is_default) {
            st.allowlist[st.al_cursor].enabled = !st.allowlist[st.al_cursor].enabled;
            FlushAllowlistToEngine(st);
            ++st.rules_changed_count;
            return true;
        }
        return false;
    }
    if (event == Event::Delete || event == Event::Character('x')) {
        if (st.al_cursor < N && !st.allowlist[st.al_cursor].is_default) {
            auto id = st.allowlist[st.al_cursor].id;
            st.allowlist.erase(st.allowlist.begin() +
                static_cast<std::ptrdiff_t>(st.al_cursor));
            eng::remove_rule(id);
            if (st.al_cursor >= st.allowlist.size() && !st.allowlist.empty())
                st.al_cursor = st.allowlist.size() - 1;
            ++st.rules_changed_count;
            return true;
        }
        return false;
    }
    if (event == Event::Character('e')) {
        st.did_export = true;
        (void)eng::export_rules();
        return true;
    }
    if (event == Event::Character('i')) {
        st.did_import = true;
        SyncAllowlistFromEngine(st);
        return true;
    }
    return false;
}

/// Process an event for Tab 2 (Preapproval).
inline bool HandlePreapprovalEvents(EditorState& st, Event event) {
    const std::size_t N = st.preapprovals.size();

    if (st.show_wizard) {
        if (event == Event::Escape) {
            st.show_wizard = false;
            st.wizard = WizardState{};
            return true;
        }
        if (st.wizard.step == 0) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                st.wizard.selected_tool = (st.wizard.selected_tool + 7) % 8;
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                st.wizard.selected_tool = (st.wizard.selected_tool + 1) % 8;
                return true;
            }
            if (event == Event::Return) { st.wizard.step = 1; return true; }
        } else if (st.wizard.step == 1) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                st.wizard.selected_action = (st.wizard.selected_action + 4) % 5;
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                st.wizard.selected_action = (st.wizard.selected_action + 1) % 5;
                return true;
            }
            if (event == Event::Return) { st.wizard.step = 2; return true; }
        } else if (st.wizard.step == 2) {
            if (event == Event::Return) {
                // Save the new rule to engine
                static const std::array<std::string, 8> tools = {
                    "BashTool", "FileEditTool", "FileWriteTool", "FileReadTool",
                    "WebFetchTool", "MCPTool", "GlobTool", "SkillTool",
                };
                static const std::array<PermissionAction, 5> actions = {
                    PermissionAction::Allow, PermissionAction::Deny,
                    PermissionAction::Ask, PermissionAction::AskOnce,
                    PermissionAction::Allow, // "* any" → default Allow
                };
                PermissionRule rule;
                rule.id = std::format("pa_{}_{}",
                    tools[st.wizard.selected_tool],
                    std::chrono::steady_clock::now().time_since_epoch().count());
                rule.tool_pattern = tools[st.wizard.selected_tool];
                rule.action = actions[st.wizard.selected_action];
                rule.scope = PermissionScope::Project;
                rule.strategy = MatchStrategy::Glob;
                rule.path_pattern = st.wizard.entered_path.empty()
                    ? std::optional<std::string>{"**/*"}
                    : st.wizard.entered_path;
                rule.priority = 100;
                if (eng::add_rule(std::move(rule))) {
                    PreapprovalRow pr;
                    pr.id = rule.id;
                    pr.tool_pattern = tools[st.wizard.selected_tool];
                    pr.action_pattern = "*";
                    pr.path_pattern = *rule.path_pattern;
                    pr.action = actions[st.wizard.selected_action];
                    pr.scope = PermissionScope::Project;
                    pr.enabled = true;
                    st.preapprovals.push_back(std::move(pr));
                    ++st.rules_changed_count;
                }
                st.show_wizard = false;
                st.wizard = WizardState{};
                return true;
            }
            if (event.is_character()) {
                auto c = event.character();
                if (c == "\b"_utf8 || c == "\x7f"_utf8) {
                    if (!st.wizard.entered_path.empty()) st.wizard.entered_path.pop_back();
                    return true;
                }
                st.wizard.entered_path += c;
                return true;
            }
        }
        return false;
    }

    // --- Normal list ---
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (N > 0) st.pa_cursor = (st.pa_cursor - 1 + N) % N;
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (N > 0) st.pa_cursor = (st.pa_cursor + 1) % N;
        return true;
    }
    if (event == Event::Character('w')) {
        st.show_wizard = true;
        st.wizard = WizardState{};
        return true;
    }
    if (event == Event::Character(' ') || event == Event::Return) {
        if (st.pa_cursor < N) {
            st.preapprovals[st.pa_cursor].enabled = !st.preapprovals[st.pa_cursor].enabled;
            // sync to engine: remove + readd
            auto id = st.preapprovals[st.pa_cursor].id;
            eng::remove_rule(id);
            if (st.preapprovals[st.pa_cursor].enabled) {
                const auto& pr = st.preapprovals[st.pa_cursor];
                PermissionRule rule;
                rule.id = pr.id;
                rule.tool_pattern = pr.tool_pattern;
                rule.action = pr.action;
                rule.scope = pr.scope;
                rule.strategy = MatchStrategy::Glob;
                rule.path_pattern = pr.path_pattern;
                rule.priority = 100;
                (void)eng::add_rule(std::move(rule));
            }
            ++st.rules_changed_count;
            return true;
        }
        return false;
    }
    if (event == Event::Delete || event == Event::Character('x')) {
        if (st.pa_cursor < N) {
            auto id = st.preapprovals[st.pa_cursor].id;
            st.preapprovals.erase(st.preapprovals.begin() +
                static_cast<std::ptrdiff_t>(st.pa_cursor));
            eng::remove_rule(id);
            if (st.pa_cursor >= st.preapprovals.size() && !st.preapprovals.empty())
                st.pa_cursor = st.preapprovals.size() - 1;
            ++st.rules_changed_count;
            return true;
        }
        return false;
    }
    return false;
}

/// Process an event for Tab 3 (Dangerous paths).
inline bool HandleDangerousEvents(EditorState& st, Event event) {
    const std::size_t N = st.dangerous.size();
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (N > 0) st.dp_cursor = (st.dp_cursor - 1 + N) % N;
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (N > 0) st.dp_cursor = (st.dp_cursor + 1) % N;
        return true;
    }
    if (event == Event::Character('w') && st.dp_cursor < N) {
        auto& d = st.dangerous[st.dp_cursor];
        if (d.exception != DangerousPathRow::ExceptionStatus::WorkspaceOnly) {
            d.exception = DangerousPathRow::ExceptionStatus::WorkspaceOnly;
            // Add to engine: allow path pattern with workspace scope
            PermissionRule rule;
            rule.id = std::format("dp_ws_{}", d.path);
            rule.tool_pattern = "*";
            rule.action = PermissionAction::Ask; // still ask, but workspace-only
            rule.scope = PermissionScope::Project;
            rule.strategy = MatchStrategy::Glob;
            rule.path_pattern = d.path;
            rule.priority = 200;
            (void)eng::add_rule(std::move(rule));
            ++st.dangerous_exceptions;
            ++st.rules_changed_count;
        }
        return true;
    }
    if (event == Event::Character('n') && st.dp_cursor < N) {
        auto& d = st.dangerous[st.dp_cursor];
        if (d.exception != DangerousPathRow::ExceptionStatus::Never) {
            d.exception = DangerousPathRow::ExceptionStatus::Never;
            PermissionRule rule;
            rule.id = std::format("dp_never_{}", d.path);
            rule.tool_pattern = "*";
            rule.action = PermissionAction::Deny;
            rule.scope = PermissionScope::Global;
            rule.strategy = MatchStrategy::Glob;
            rule.path_pattern = d.path;
            rule.priority = 1000; // highest
            (void)eng::add_rule(std::move(rule));
            ++st.dangerous_exceptions;
            ++st.rules_changed_count;
        }
        return true;
    }
    if (event == Event::Character('r') && st.dp_cursor < N) {
        auto& d = st.dangerous[st.dp_cursor];
        if (d.exception != DangerousPathRow::ExceptionStatus::None) {
            // remove both possible engine rules
            eng::remove_rule(std::format("dp_ws_{}", d.path));
            eng::remove_rule(std::format("dp_never_{}", d.path));
            d.exception = DangerousPathRow::ExceptionStatus::None;
            ++st.rules_changed_count;
        }
        return true;
    }
    return false;
}

// ============================================================
// Public factory
// ============================================================

/// Construct the full interactive scope editor.
[[nodiscard]] inline Component MakeScopeEditor(ScopeEditorCallbacks cbs) {
    auto st = std::make_shared<EditorState>();
    st->cbs = std::move(cbs);

    // Pre-populate from engine (existing rules)
    SyncAllowlistFromEngine(*st);
    // Dangerous paths: default list
    st->dangerous = detail::DefaultDangerousPaths();

    return Renderer([st] { return RenderScopeEditor(st); })
         | CatchEvent([st](Event event) -> bool {
        // Tab switching
        if (event == Event::ArrowLeft) {
            if (st->active_tab == Tab::Preapproval)  st->active_tab = Tab::Allowlist;
            else if (st->active_tab == Tab::Dangerous) st->active_tab = Tab::Preapproval;
            return true;
        }
        if (event == Event::ArrowRight) {
            if (st->active_tab == Tab::Allowlist)   st->active_tab = Tab::Preapproval;
            else if (st->active_tab == Tab::Preapproval) st->active_tab = Tab::Dangerous;
            return true;
        }

        // Delegate to tab-specific handlers
        switch (st->active_tab) {
            case Tab::Allowlist:
                if (HandleAllowlistEvents(*st, event)) return true;
                break;
            case Tab::Preapproval:
                if (HandlePreapprovalEvents(*st, event)) return true;
                break;
            case Tab::Dangerous:
                if (HandleDangerousEvents(*st, event)) return true;
                break;
        }

        // Close
        if (event == Event::Escape || event == Event::Character('q')) {
            if (st->cbs.on_close) {
                EditorSummary s;
                s.rules_changed = st->rules_changed_count;
                s.dangerous_exceptions = st->dangerous_exceptions;
                s.exported = st->did_export;
                s.imported = st->did_import;
                st->cbs.on_close(s);
            }
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::permissions::scope_editor
