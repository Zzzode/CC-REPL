/// @file permission_rule_list.cppm
/// @brief Three-column permission rule editor (groups | rules table w/ virtual
/// scroll + batch ops | editor + hit-test firewall).  JSON import/export diff.
/// All mutations flow through callbacks to cc.utils.permissions_engine.
/// Migrated from src/components/permissions/rules/PermissionRuleList.tsx.
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.rule_list;

import cc.utils.permissions_engine;
import cc.ui.permissions.scope_editor;
import cc.ui.permissions.components;
import cc.ui.design.tokens;
import cc.ui.custom_select;
import cc.ui.structured_diff;

export namespace cc::ui::permissions::rule_list {
using namespace ftxui;
namespace dt     = cc::ui::design::tokens;
namespace se     = cc::ui::permissions::scope_editor;
namespace pc     = cc::ui::permissions::components;
namespace eng    = cc::utils::permissions;
namespace cs     = cc::ui::custom_select;
namespace sd     = cc::ui::structured_diff;

using eng::MatchStrategy;
using eng::PermissionAction;
using eng::PermissionScope;
using eng::PermissionRule;

// --- Enums & Types (mirror TS PermissionRuleList + UI9 data contracts) ---

/// Batch operation dispatcher (for multi-selected rules)
enum class BatchOp : std::uint8_t {
    Enable,        // Set enabled = true
    Disable,       // Set enabled = false
    SetStrategy,   // Change MatchStrategy (payload in op_arg)
    SetScope,      // Change PermissionScope
    SetAction,     // Change PermissionAction
    Delete,        // Remove rule(s)
    ExportJson,    // Serialize selected rules to JSON string
};

/// A single logical group of rules.  Mirrors the TS "rule tabs" (recent /
/// allow / ask / deny / workspace / custom x3) — 8 canonical groups total.
struct RuleGroup {
    std::string id;        // e.g. "g1", "recent", "allow"
    std::string label;     // "Recently used", "Allowed"
    std::string hotkey;    // single-char shortcut: "1" / "2" … "8"
    std::string icon;      // 1-2 glyph prefix
    Color       accent;    // group accent color
};

/// A single rule visual row.  Field alignment with scope_editor::AllowlistRow
/// and engine::PermissionRule; additional UI-only flags: selected, edited,
/// newly_created, search_hits.
struct RuleEntry {
    std::string                  id;
    std::string                  tool_pattern;
    MatchStrategy                strategy  = MatchStrategy::Glob;
    PermissionAction             action    = PermissionAction::Ask;
    PermissionScope              scope     = PermissionScope::Session;
    std::optional<std::string>   path_pattern;
    int                          priority  = 50;

    std::string                  group_id;       // e.g. "g3"
    std::vector<std::string>     tools;          // explicit tool list (ToolMulti)
    std::string                  description;    // human-readable rule memo
    std::string                  source;         // Bundled / User / CustomPath
    bool                         enabled     = true;
    bool                         is_default  = false;
    std::size_t                  enabled_count = 0; // cache for group badges
};

struct HitSample {
    std::string test_path;      // e.g. "/etc/shadow"
    std::string test_tool;      // e.g. "BashTool"
    std::string note;           // short sample label
};

struct FilterSet {
    std::string           group_id;        // empty = all
    PermissionAction      action_filter = PermissionAction::Ask;
    std::optional<bool>   enabled_only;
    bool                  use_action_filter = false;
};

struct RuleListInput {
    std::vector<RuleEntry>    rules;
    std::vector<RuleGroup>    groups;        // 8 canonical
    std::string               search_query;
    FilterSet                 filters;
    std::vector<HitSample>    hit_samples;   // up to 3 for preview
};

/// All mutations go through these callbacks to the engine layer.
struct RuleListCallbacks {
    std::function<void(RuleEntry rule)>                          on_add;
    std::function<void(std::string_view rule_id, RuleEntry upd)> on_update;
    std::function<void(std::string_view rule_id)>                on_delete;
    std::function<void(std::vector<std::string> ids,
                       BatchOp op, std::string arg)>             on_batch;
    std::function<void(std::string_view json_text)>              on_import;
    std::function<std::string()>                                 on_export;
    std::function<std::string(const HitSample&, bool matched)>   on_hit_test;
};

// --- Constants (8 groups + column layout + virtual-scroll) ---

namespace detail {

/// Default 8 rule groups.  Callers can override in RuleListInput::groups but
/// the factory seeds this list when the input groups vector is empty.
[[nodiscard]] inline std::array<RuleGroup, 8> DefaultGroups() {
    return std::array<RuleGroup, 8>{{
        {"g1", "Recent",     "1", "⧗", Color::Cyan},
        {"g2", "Allowed",    "2", "✓", Color::Green},
        {"g3", "Ask",        "3", "?", Color::Yellow},
        {"g4", "Denied",     "4", "✗", Color::Red},
        {"g5", "Workspace",  "5", "⚑", Color::Blue},
        {"g6", "Custom A",   "6", "A", Color::Magenta},
        {"g7", "Custom B",   "7", "B", Color::MagentaLight},
        {"g8", "Custom C",   "8", "C", Color::Purple4},
    }};
}

/// Strategy / scope / action names (reused in editor dropdowns + batch op UI)
inline constexpr std::array<std::string_view, 4> kStrategyNames = {
    "Exact", "Prefix", "Glob", "Regex"
};
inline constexpr std::array<std::string_view, 4> kScopeNames    = {
    "Global", "Project", "Session", "Command"
};
inline constexpr std::array<std::string_view, 4> kActionNames   = {
    "Allow", "Deny", "Ask", "Ask once"
};

/// 3 sample firewall hit-test paths used when caller omits hit_samples.
inline constexpr std::array<const char*, 3> kDefaultSamples = {
    "/home/user/project/src/**/*.ts",
    "~/.aws/credentials",
    "/tmp/cc-repl-workdir/build/**/*",
};

/// Virtual-scroll geometry constants.
inline constexpr int kVScrollVisible = 18;     // visible rule rows
inline constexpr int kVScrollBuffer  = 8;      // off-screen buffer for smooth
inline constexpr int kLeftColWidth   = 22;     // left column fixed width
inline constexpr int kRightColWidth  = 36;     // right column fixed width
inline constexpr int kDescriptionRows = 3;     // rule description editor height
inline constexpr int kDescriptionCols = 80;    // soft column cap for wrapping

} // namespace detail

// --- State ---

enum class FocusZone : std::uint8_t {
    Groups,     // left column group list
    Rules,      // middle rule table
    Editor,     // right column editor
    Toolbar,    // bottom toolbar
    DiffModal,  // JSON-diff fullscreen modal
    ImportModal,// Import JSON paste dialog
};

/// RuleEditor state (right column, subset of fields editable)
struct EditorState {
    bool dirty = false;
    std::string pattern_buf;
    int         strategy_idx = 2;  // Glob default
    int         scope_idx    = 2;  // Session default
    int         action_idx   = 2;  // Ask default
    int         group_idx    = 0;
    std::string description_buf;  // 3-line description
    std::vector<std::string> selected_tools;
};

/// Batch-pending state
struct BatchState {
    bool pending = false;
    BatchOp op;
    std::string op_arg;
};

struct RuleListState {
    RuleListInput     input;
    RuleListCallbacks cbs;

    std::size_t       active_group_idx = 0;
    std::size_t       rule_cursor      = 0;
    std::vector<std::size_t> selected_indexes;
    std::optional<std::size_t> anchor_index; // for Shift-range

    std::string       search_buf;
    bool              search_focused = false;
    FilterSet         filters;

    FocusZone         focus = FocusZone::Rules;

    EditorState       editor;
    std::optional<std::size_t> editing_idx;

    std::string       import_buf;
    bool              show_diff   = false;
    std::string       diff_old;
    std::string       diff_new;

    BatchState        batch;

    std::size_t       vscroll_offset = 0;

    std::vector<std::size_t> filtered;

    std::vector<HitSample> hit_samples;
    std::string status_bar_msg;
    std::size_t status_expire = 0;

    int toolbar_cursor = 0;
};

// --- Helpers: filter + badges + formatting ---

namespace detail {

[[nodiscard]] inline std::string_view StrategyName(MatchStrategy s) {
    return kStrategyNames[static_cast<std::size_t>(s)];
}
[[nodiscard]] inline std::string_view ScopeName(PermissionScope s) {
    return kScopeNames[static_cast<std::size_t>(s)];
}
[[nodiscard]] inline std::string_view ActionName(PermissionAction a) {
    return kActionNames[static_cast<std::size_t>(a)];
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

/// Apply search + filters; list of indices into state.input.rules.
inline void RefreshFiltered(RuleListState& st) {
    st.filtered.clear();
    st.filtered.reserve(st.input.rules.size());
    std::string query = st.search_buf;
    std::transform(query.begin(), query.end(), query.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (std::size_t i = 0; i < st.input.rules.size(); ++i) {
        const auto& r = st.input.rules[i];
        if (!st.filters.group_id.empty() && r.group_id != st.filters.group_id) continue;
        if (st.filters.enabled_only && r.enabled != *st.filters.enabled_only) continue;
        if (st.filters.use_action_filter && r.action != st.filters.action_filter) continue;
        if (!query.empty()) {
            std::string hay = std::format("{} {} {} {}",
                r.tool_pattern, r.path_pattern.value_or(""),
                r.description, r.source);
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (hay.find(query) == std::string::npos) continue;
        }
        st.filtered.push_back(i);
    }
    if (!st.filtered.empty())
        st.rule_cursor = std::min(st.rule_cursor, st.filtered.size() - 1);
    else st.rule_cursor = 0;
    std::erase_if(st.selected_indexes,
                  [&](std::size_t s) { return s >= st.filtered.size(); });
    if (st.anchor_index && *st.anchor_index >= st.filtered.size())
        st.anchor_index.reset();
}

/// Per-group enabled/total badge numbers.
[[nodiscard]] inline auto GroupCounts(const RuleListState& st) {
    using Counts = std::unordered_map<std::string, std::pair<std::size_t, std::size_t>>;
    Counts out;
    for (const auto& g : st.input.groups)
        out.emplace(g.id, std::make_pair(0, 0));
    for (const auto& r : st.input.rules) {
        auto it = out.find(r.group_id);
        if (it == out.end()) continue;
        auto& [e, t] = it->second;
        ++t; if (r.enabled) ++e;
    }
    return out;
}

/// Selected filtered-indices -> matching rule ids.
[[nodiscard]] inline std::vector<std::string>
SelectedRuleIds(const RuleListState& st) {
    std::vector<std::string> ids;
    ids.reserve(st.selected_indexes.size());
    for (auto si : st.selected_indexes) {
        if (si >= st.filtered.size()) continue;
        ids.push_back(st.input.rules[st.filtered[si]].id);
    }
    return ids;
}

} // namespace detail

// --- Rendering helpers ---

namespace render {

[[nodiscard]] inline Element GroupsColumn(RuleListState& st) {
    auto counts = detail::GroupCounts(st);
    Elements rows;
    rows.push_back(hbox({
        text("  Groups ") | bold | color(Color::Cyan),
        filler(), text("(g1..g8) ") | dim,
    }));
    rows.push_back(pc::ThinDivider());

    const auto& groups = st.input.groups;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        const auto [en, tot] = [&]() {
            auto it = counts.find(g.id);
            if (it != counts.end()) return it->second;
            return std::make_pair(std::size_t{0}, std::size_t{0});
        }();
        const bool active = st.filters.group_id == g.id;
        const bool focus  = (st.focus == FocusZone::Groups &&
                            gi == st.active_group_idx);

        std::string left_pad = focus ? "▶ " : "  ";
        auto badge = text(std::format(" {}/{} ", en, tot))
                   | color(Color::GrayDark)
                   | bgcolor(Color::RGB(40, 40, 46));

        Elements cells = {
            text(left_pad),
            text(g.icon) | color(g.accent),
            text(" "),
            text(std::format("[{}] {}", g.hotkey, g.label))
                | (active ? (bold | color(g.accent)) : color(Color::White)),
            filler(),
            std::move(badge),
        };
        auto row = hbox(std::move(cells))
                 | size(WIDTH, EQUAL, detail::kLeftColWidth - 2);
        if (focus)  row = row | inverted | ftxui::focus;
        if (active) row = row | bgcolor(Color::RGB(28, 34, 46));
        rows.push_back(std::move(row));
    }

    rows.push_back(pc::ThinDivider());
    rows.push_back(
        hbox({
            text(" [Tab] ") | color(Color::Cyan) | dim,
            text("next zone") | dim,
        })
    );

    auto body = vbox(std::move(rows)) | yflex_grow | yframe;
    return window(
        text(" 🗂 ") | bold | color(Color::Cyan),
        body | xflex
    ) | color(Color::Cyan) | size(WIDTH, EQUAL, detail::kLeftColWidth);
}

inline Element RulesHeader() {
    constexpr int kIdx = 5;
    constexpr int kSt  = 5;
    constexpr int kTool = 20;
    constexpr int kAct = 10;
    constexpr int kStra = 8;
    constexpr int kSc = 9;
    constexpr int kPrio = 5;
    return hbox({
        text("  # ") | bold | size(WIDTH, EQUAL, kIdx),
        text(" On ") | bold | size(WIDTH, EQUAL, kSt),
        text(" Tool / pattern ") | bold | size(WIDTH, EQUAL, kTool),
        text(" Action ") | bold | size(WIDTH, EQUAL, kAct),
        text(" Strategy ") | bold | size(WIDTH, EQUAL, kStra),
        text(" Scope ") | bold | size(WIDTH, EQUAL, kSc),
        text(" Pri ") | bold | size(WIDTH, EQUAL, kPrio),
        text(" Source ") | bold | xflex_grow,
    }) | bgcolor(Color::RGB(30, 32, 36));
}

[[nodiscard]] inline Element RuleRow(const RuleEntry& r, std::size_t row_num,
                                     bool hovered, bool selected, bool editing) {
    constexpr int kIdx = 5;
    constexpr int kSt  = 5;
    constexpr int kTool = 20;
    constexpr int kAct = 10;
    constexpr int kStra = 8;
    constexpr int kSc = 9;
    constexpr int kPrio = 5;

    Element check   = selected
        ? text(" [✓]") | color(Color::Green) | bold
        : text("    ") ;

    Element status = r.enabled
        ? text("  ✓ ") | color(Color::Green)
        : text("  ✕ ") | color(Color::RedLight) | dim;

    Element tool = pc::PathLabel(
        std::format("{}{}{}",
                    r.tool_pattern,
                    r.path_pattern ? " ∣ " : "",
                    r.path_pattern.value_or("")),
        kTool - 4);
    if (!r.path_pattern && !r.description.empty())
        tool = hbox({tool, text("  ‹") | dim,
                     pc::PathLabel(r.description, 10) | dim,
                     text("›") | dim});

    Element action = text(std::string{detail::ActionName(r.action)})
                   | color(detail::ActionColor(r.action))
                   | bold;

    Element strategy = text(std::string{detail::StrategyName(r.strategy)}) | dim;
    Element scope    = text(std::string{detail::ScopeName(r.scope)}) | dim;
    Element priority = text(std::format(" {:>2} ", r.priority)) | dim;

    Element source = [&]() {
        std::string_view s = r.source.empty() ? "—" : std::string_view{r.source};
        Color c = Color::GrayLight;
        if (s == "Bundled") c = Color::Green;
        else if (s == "User") c = Color::Yellow;
        else if (s == "CustomPath") c = Color::Red;
        return text(std::format(" {} ", s)) | color(c) | dim;
    }();

    Elements cells = {
        text(std::format(" {:>2} ", row_num))
            | dim | size(WIDTH, EQUAL, kIdx),
        std::move(check) | size(WIDTH, EQUAL, kSt),
        std::move(tool) | size(WIDTH, EQUAL, kTool),
        std::move(action) | size(WIDTH, EQUAL, kAct),
        std::move(strategy) | size(WIDTH, EQUAL, kStra),
        std::move(scope) | size(WIDTH, EQUAL, kSc),
        std::move(priority) | size(WIDTH, EQUAL, kPrio),
        std::move(source) | xflex_grow,
    };
    auto row = hbox(std::move(cells));
    if (!r.enabled) row = row | dim;
    if (editing)   row = row | color(Color::CyanLight);
    if (selected)  row = row | bgcolor(Color::RGB(28, 48, 62));
    if (hovered)   row = row | inverted | focus;
    return row;
}

[[nodiscard]] inline Element RulesColumn(RuleListState& st) {
    const auto& idx = st.filtered;
    const std::size_t total = idx.size();

    const int visible = detail::kVScrollVisible;
    if (st.rule_cursor < st.vscroll_offset)
        st.vscroll_offset = st.rule_cursor;
    else if (st.rule_cursor >= st.vscroll_offset + std::size_t(visible))
        st.vscroll_offset = st.rule_cursor - std::size_t(visible - 1);
    const std::size_t begin = st.vscroll_offset;
    const std::size_t end   = std::min(begin + std::size_t(visible +
                                          detail::kVScrollBuffer), total);

    Elements header_parts = {
        RulesHeader(),
        pc::ThinDivider(),
    };
    Element body;
    if (idx.empty()) {
        Elements empty_els = std::move(header_parts);
        for (int i = 0; i < visible; ++i) {
            if (i == visible / 2) {
                empty_els.push_back(
                    hbox({
                        filler(),
                        text(st.search_buf.empty()
                                 ? "  (no rules — press [n] to add)  "
                                 : std::format("  (no matches for '{}')  ",
                                               st.search_buf))
                            | dim | color(Color::Yellow),
                        filler(),
                    })
                );
            } else {
                empty_els.push_back(text(""));
            }
        }
        body = vbox(std::move(empty_els));
    } else {
        Elements rows = std::move(header_parts);
        rows.reserve(visible + 4);
        for (std::size_t di = begin; di < end; ++di) {
            const bool hovered = (di == st.rule_cursor &&
                                  st.focus == FocusZone::Rules);
            const bool selected =
                std::find(st.selected_indexes.begin(),
                          st.selected_indexes.end(), di)
                != st.selected_indexes.end();
            const bool editing =
                st.editing_idx && *st.editing_idx == di;
            rows.push_back(RuleRow(st.input.rules[idx[di]], di + 1,
                                   hovered, selected, editing));
        }
        // Pad to fixed visible height
        const std::size_t rendered = end - begin;
        if (rendered < std::size_t(visible)) {
            for (std::size_t i = rendered; i < std::size_t(visible); ++i)
                rows.push_back(text(""));
        }
        body = vbox(std::move(rows));
    }

    auto batch_chip = [&]() -> Element {
        if (st.selected_indexes.empty()) return text("");
        return text(std::format(" ◈{} selected ",
                                st.selected_indexes.size()))
             | color(Color::White) | bgcolor(Color::Blue) | bold;
    }();
    auto search_chip = !st.search_buf.empty()
        ? text(std::format(" 🔍 '{}' ", st.search_buf))
            | color(Color::CyanLight) | dim
        : text("");

    Elements footer_parts = {
        text(st.focus == FocusZone::Rules ? " ▣ Rules" : " ▢ Rules")
            | color(st.focus == FocusZone::Rules
                        ? Color::Cyan : Color::GrayDark),
        text(std::format(" {} / {} ", st.rule_cursor + 1, total)) | dim,
        std::move(search_chip),
        filler(),
        std::move(batch_chip),
    };
    if (!st.status_bar_msg.empty()) {
        footer_parts.push_back(text(std::format(" • {}", st.status_bar_msg))
                                   | color(Color::Yellow) | dim);
    }
    auto footer = hbox(std::move(footer_parts));

    auto head = [&]() {
        Elements top;
        // Search row
        if (st.search_focused) {
            top.push_back(
                hbox({
                    text(" 🔍 ") | color(Color::Cyan),
                    text(st.search_buf.empty()
                             ? std::string{" (type to filter, Esc cancel) "}
                             : st.search_buf + "▊")
                        | color(Color::CyanLight) | bold,
                    filler(),
                    text(std::format(" {:>3} results ", total)) | dim,
                })
            );
        } else {
            top.push_back(
                hbox({
                    text(" Rule list ") | bold | color(Color::Cyan),
                    filler(),
                    text(std::format(" {:>3} rules total ",
                                     st.input.rules.size())) | dim,
                    text("  [/] filter  [n] new  [space] sel  [a] all ") | dim,
                })
            );
        }
        top.push_back(pc::ThinDivider());
        return vbox(std::move(top));
    }();

    auto full = vbox({
        head,
        body | yflex_grow | yframe,
        pc::ThinDivider(),
        footer,
    });
    return window(
        text(" 🛡 Permission Rules ") | bold | color(Color::Cyan),
        full | xflex_grow
    ) | color(Color::Cyan);
}

} // namespace render

namespace render {

/// Generic mini segmented picker rendered from a string-view list.
template<std::size_t N>
[[nodiscard]] inline Element MiniPicker(int sel, Color c,
    const std::array<std::string_view, N>& names)
{
    Elements els; els.reserve(2 * N);
    for (std::size_t i = 0; i < N; ++i) {
        auto e = text(std::format(" {}", names[i]))
               | (std::size_t(sel) == i ? (bold | color(c) | inverted) : dim);
        els.push_back(std::move(e));
        if (i + 1 < N) els.push_back(text(" "));
    }
    return hbox(std::move(els));
}

[[nodiscard]] inline Element MiniStrategyPicker(const EditorState& ed) {
    return MiniPicker(ed.strategy_idx, Color::Cyan, detail::kStrategyNames);
}
[[nodiscard]] inline Element MiniScopePicker(const EditorState& ed) {
    return MiniPicker(ed.scope_idx, Color::Blue, detail::kScopeNames);
}
[[nodiscard]] inline Element MiniActionPicker(const EditorState& ed) {
    return MiniPicker(ed.action_idx,
        detail::ActionColor(static_cast<PermissionAction>(ed.action_idx)),
        detail::kActionNames);
}

/// Very simple word-wrap for the 3-line description buffer.
[[nodiscard]] inline std::vector<std::string>
SoftWrap(std::string_view text, std::size_t col) {
    std::vector<std::string> out;
    std::string cur; cur.reserve(col);
    std::istringstream iss(std::string{text});
    std::string word;
    while (iss >> word) {
        if (cur.size() + 1 + word.size() > col) {
            out.push_back(cur); cur = word;
            if (out.size() >= detail::kDescriptionRows - 1) break;
        } else {
            if (!cur.empty()) cur += ' ';
            cur += word;
        }
    }
    if (!cur.empty() && out.size() < detail::kDescriptionRows)
        out.push_back(std::move(cur));
    while (out.size() < detail::kDescriptionRows) out.emplace_back();
    if (out.empty()) out.emplace_back();
    return out;
}

[[nodiscard]] inline Element EffectivePreview(const RuleEntry* r,
                                              const std::vector<HitSample>& samples,
                                              RuleListState& st) {
    Elements rows;
    rows.push_back(hbox({
        text(" Effective Preview ") | bold | color(Color::Magenta),
        filler(), text(" 🔬 firewall ") | dim,
    }));
    rows.push_back(pc::ThinDivider());
    if (!r) {
        for (int i = 0; i < 3; ++i)
            rows.push_back(text("  (no rule selected) ") | dim);
        return vbox(std::move(rows));
    }
    const int shown = std::min<int>(3, (int)samples.size());
    for (int i = 0; i < shown; ++i) {
        const auto& s = samples[i];
        const bool matched_path = !r->path_pattern ||
            eng::match_path_pattern(s.test_path, *r->path_pattern,
                                    r->strategy);
        const bool matched_tool =
            eng::match_path_pattern(s.test_tool, r->tool_pattern,
                                    r->strategy);
        const bool hit = matched_path && matched_tool && r->enabled;

        std::string note = s.note.empty()
            ? std::format(" #{} tool={}", i + 1, s.test_tool) : s.note;
        Element outcome = hit
            ? text(std::format(" ◉ HIT → {} ",
                               detail::ActionName(r->action)))
                | color(detail::ActionColor(r->action)) | bold
            : text(" ○ miss ") | color(Color::GrayDark) | dim;

        std::string auth_tag = st.cbs.on_hit_test
            ? st.cbs.on_hit_test(s, hit) : "";
        rows.push_back(vbox({
            hbox({
                text(std::format(" {} ", i + 1)) | dim,
                pc::PathLabel(s.test_path, 28),
                filler(), std::move(outcome),
            }),
            hbox({
                text("    ") | dim, text(note) | dim,
                auth_tag.empty() ? text("")
                    : hbox({text("  ") | dim,
                            text(auth_tag) | color(Color::CyanLight)}),
            }),
        }));
    }
    return vbox(std::move(rows))
         | size(WIDTH, EQUAL, detail::kRightColWidth - 2) | xflex_grow;
}

[[nodiscard]] inline Element EditorColumn(RuleListState& st) {
    const RuleEntry* editing = nullptr;
    if (st.editing_idx && *st.editing_idx < st.filtered.size())
        editing = &st.input.rules[st.filtered[*st.editing_idx]];
    else if (!st.filtered.empty())
        editing = &st.input.rules[st.filtered[st.rule_cursor]];

    Elements group_els;
    for (std::size_t gi = 0; gi < st.input.groups.size(); ++gi) {
        const auto& g = st.input.groups[gi];
        const bool sel = std::size_t(st.editor.group_idx) == gi;
        auto e = text(std::format("{}", g.icon))
               | (sel ? (bold | color(g.accent) | inverted)
                      : color(g.accent) | dim);
        group_els.push_back(std::move(e));
        if (gi + 1 < st.input.groups.size()) group_els.push_back(text("·"));
    }

    auto pattern_line = [&]() {
        const auto& p = st.editor.pattern_buf;
        return hbox({
            text("pattern: ") | dim,
            text(p.empty() ? std::string{"(type with [e] + Tab)"} : p)
                | color(Color::Yellow) | bold,
        });
    }();

    auto tools_line = [&]() {
        if (st.editor.selected_tools.empty())
            return text("tools:   (all)  ") | dim;
        Elements cells = {text("tools:   ") | dim};
        for (std::size_t i = 0; i < st.editor.selected_tools.size(); ++i) {
            const auto& t = st.editor.selected_tools[i];
            cells.push_back(
                hbox({
                    text("[") | dim,
                    text(t.substr(0, 4)) | color(Color::Magenta),
                    text("]") | dim,
                })
            );
        }
        return hbox(std::move(cells));
    }();

    // 3-line description editor (word-wrapped)
    auto desc_lines = SoftWrap(st.editor.description_buf,
                               detail::kDescriptionCols);
    Elements desc_row;
    desc_row.push_back(text("desc:    ") | dim);
    Elements desc_body;
    for (auto& l : desc_lines) {
        if (l.empty()) l = " " ;
        desc_body.push_back(text("   " + l) | color(Color::GrayLight));
    }
    auto desc_block =
        hbox({text("desc: ") | dim,
              vbox(std::move(desc_body)) | xflex_grow})
        | size(WIDTH, EQUAL, detail::kRightColWidth - 6);

    auto head = hbox({
        text(" Rule Editor ") | bold | color(Color::Cyan),
        filler(),
        st.editor.dirty ? text(" •") | color(Color::YellowLight) : text(""),
    });

    Elements body_rows;
    body_rows.push_back(head);
    body_rows.push_back(pc::ThinDivider());
    body_rows.push_back(
        hbox({
            text("group: ") | dim,
            hbox(std::move(group_els)),
            filler(),
            editing
                ? pc::RiskPill(static_cast<pc::RiskLevel>(
                      editing->action == PermissionAction::Allow     ? 0
                    : editing->action == PermissionAction::Ask       ? 1
                    : editing->action == PermissionAction::Deny      ? 2
                                                                      : 3))
                : text(""),
        })
    );
    body_rows.push_back(std::move(pattern_line));
    body_rows.push_back(std::move(tools_line));
    body_rows.push_back(
        hbox({text("strat: ") | dim, MiniStrategyPicker(st.editor)})
    );
    body_rows.push_back(
        hbox({text("scope: ") | dim, MiniScopePicker(st.editor)})
    );
    body_rows.push_back(
        hbox({text("action:") | dim, MiniActionPicker(st.editor)})
    );
    body_rows.push_back(std::move(desc_block));
    body_rows.push_back(pc::ThinDivider());
    body_rows.push_back(EffectivePreview(editing, st.hit_samples, st));

    body_rows.push_back(pc::ThinDivider());
    body_rows.push_back(
        hbox({
            text(" [Ctrl+s] save ") | color(Color::Cyan) | dim,
            text(" [Esc] abandon ") | dim,
            filler(),
            text(editing ? std::format(" id {} ", editing->id)
                                    .substr(0, 16) : std::string{" (new) "})
                | dim,
        })
    );

    auto body = vbox(std::move(body_rows)) | yflex_grow | yframe;
    return window(
        text(" ✎ Editor ") | bold | color(Color::Cyan),
        body | xflex_grow
    ) | color(Color::Cyan) | size(WIDTH, EQUAL, detail::kRightColWidth);
}

} // namespace render

namespace render {

[[nodiscard]] inline Element Toolbar(RuleListState& st) {
    static constexpr std::array<std::string_view, 4> kLabels = {
        "+ New rule", "⤴ Import", "⤵ Export", "⎺ JSON diff"
    };
    static const std::array<Color, 4> kColors = {
        Color::Green, Color::Blue, Color::Cyan, Color::Magenta
    };
    Elements cells;
    cells.push_back(text(" "));
    for (int i = 0; i < 4; ++i) {
        const bool hovered = (st.focus == FocusZone::Toolbar &&
                              st.toolbar_cursor == i);
        auto chip = text(std::format(" {} ", kLabels[i]))
                  | color(kColors[i]);
        if (hovered) chip = chip | bold | inverted;
        else         chip = chip | dim;
        cells.push_back(std::move(chip));
        if (i < 3) cells.push_back(text("   "));
    }
    cells.push_back(filler());
    cells.push_back(
        text(" [n/i/e/d] trigger • [Tab] cycle focus ") | dim
    );
    return hbox(std::move(cells))
         | bgcolor(Color::RGB(26, 28, 32))
         | borderDashed | color(Color::GrayDark);
}

[[nodiscard]] inline Element ImportOverlay(RuleListState& st) {
    std::string hint = st.import_buf.empty()
        ? std::string{"  (paste JSON, Ctrl+v / [Enter] submit, Esc cancel)  "}
        : st.import_buf;
    if (hint.size() > 70) hint = hint.substr(0, 67) + "...";
    return dbox({
        text("") | clear_under,
        window(
            text(" ⤴ Import Rules ") | bold | color(Color::Blue),
            vbox({
                text(" Paste a JSON array of rule objects ") | dim,
                pc::ThinDivider(),
                text(std::string{" "} + hint + std::string{"▊"})
                    | color(Color::YellowLight) | bold,
                pc::ThinDivider(),
                hbox({
                    text(" [Enter] import ") | color(Color::Blue) | dim,
                    text(" [Esc] cancel ") | dim,
                    filler(),
                    text(std::format(" {} bytes typed ",
                                     st.import_buf.size())) | dim,
                }),
            }) | size(WIDTH, EQUAL, 80) | size(HEIGHT, EQUAL, 8)
        ) | color(Color::Blue),
    });
}

[[nodiscard]] inline Element DiffOverlay(RuleListState& st) {
    std::istringstream iss_old(st.diff_old);
    std::istringstream iss_new(st.diff_new);
    std::vector<std::string> old_lines, new_lines;
    std::string line;
    while (std::getline(iss_old, line)) old_lines.push_back(line);
    while (std::getline(iss_new, line)) new_lines.push_back(line);

    sd::StructuredPatchHunk hunk;
    hunk.old_start = 1; hunk.old_lines = (int)old_lines.size();
    hunk.new_start = 1; hunk.new_lines = (int)new_lines.size();
    hunk.header = "@@ rules JSON before/after import @@";
    for (std::size_t i = 0; i < old_lines.size(); ++i) {
        using L = sd::StructuredDiffLine::Type;
        hunk.lines.push_back(sd::StructuredDiffLine{
            L::Removed, old_lines[i],
            (int)i + 1, std::nullopt, {}
        });
    }
    for (std::size_t i = 0; i < new_lines.size(); ++i) {
        using L = sd::StructuredDiffLine::Type;
        hunk.lines.push_back(sd::StructuredDiffLine{
            L::Added, new_lines[i],
            std::nullopt, (int)i + 1, {}
        });
    }

    sd::DiffDetailViewProps dp;
    dp.file_path = "rules.json  (before → after import)";
    dp.hunks = {std::move(hunk)};

    sd::StructuredDiffOptions opts;
    opts.diff = std::move(dp);
    opts.visible_height = 40;
    opts.max_display_lines = 1200;

    auto diff_el = sd::RenderStructuredDiff(opts);

    return dbox({
        text("") | clear_under,
        window(
            text(" ⎺ Rules JSON Diff — [Esc] close ")
                | bold | color(Color::Magenta),
            vbox({
                diff_el | yflex_grow | yframe | size(HEIGHT, GREATER_THAN, 20),
            }) | size(WIDTH, GREATER_THAN, 80)
        ) | color(Color::Magenta),
    });
}

} // namespace render

// --- Main composition ---

[[nodiscard]] inline Element RenderRuleList(std::shared_ptr<RuleListState> st) {
    auto top = hbox({
        text(" 🛡 Permission Rule List ") | bold | color(dt::palette::dark.primary),
        filler(),
        text(" [Esc] close ") | dim,
    });

    auto three_col = hbox({
        render::GroupsColumn(*st),
        text(" "),
        render::RulesColumn(*st) | xflex_grow,
        text(" "),
        render::EditorColumn(*st),
    }) | yflex_grow;

    Element main = vbox({
        top,
        pc::ThinDivider(),
        three_col | yflex_grow,
        pc::ThinDivider(),
        render::Toolbar(*st),
    }) | yflex_grow;

    if (st->focus == FocusZone::ImportModal)
        main = dbox({main, render::ImportOverlay(*st)});
    else if (st->focus == FocusZone::DiffModal)
        main = dbox({main, render::DiffOverlay(*st)});

    return main;
}

// --- Event handling helpers ---

namespace ev {

/// Cycle focus zones: Groups → Rules → Editor → Toolbar → Groups …
inline void CycleFocusForward(RuleListState& st) {
    switch (st.focus) {
        case FocusZone::Groups:    st.focus = FocusZone::Rules;   break;
        case FocusZone::Rules:     st.focus = FocusZone::Editor;  break;
        case FocusZone::Editor:    st.focus = FocusZone::Toolbar; break;
        case FocusZone::Toolbar:   st.focus = FocusZone::Groups;  break;
        case FocusZone::DiffModal: break;
        case FocusZone::ImportModal: break;
    }
}
inline void CycleFocusBack(RuleListState& st) {
    switch (st.focus) {
        case FocusZone::Groups:    st.focus = FocusZone::Toolbar; break;
        case FocusZone::Toolbar:   st.focus = FocusZone::Editor;  break;
        case FocusZone::Editor:    st.focus = FocusZone::Rules;   break;
        case FocusZone::Rules:     st.focus = FocusZone::Groups;  break;
        case FocusZone::DiffModal: break;
        case FocusZone::ImportModal: break;
    }
}

/// Toggle a single filtered-index into the selection set.
inline void ToggleIndex(RuleListState& st, std::size_t fi) {
    auto it = std::find(st.selected_indexes.begin(),
                        st.selected_indexes.end(), fi);
    if (it == st.selected_indexes.end())
        st.selected_indexes.push_back(fi);
    else
        st.selected_indexes.erase(it);
}

/// Select the index range [min(fi, anchor), max(fi, anchor)] inclusive.
inline void SelectRange(RuleListState& st, std::size_t fi) {
    if (!st.anchor_index) {
        st.anchor_index = fi;
        ToggleIndex(st, fi);
        return;
    }
    const std::size_t a = std::min(*st.anchor_index, fi);
    const std::size_t b = std::max(*st.anchor_index, fi);
    std::set<std::size_t> merge(st.selected_indexes.begin(),
                                st.selected_indexes.end());
    for (std::size_t i = a; i <= b; ++i) merge.insert(i);
    st.selected_indexes.assign(merge.begin(), merge.end());
}

inline void SelectAll(RuleListState& st) {
    st.selected_indexes.clear();
    for (std::size_t i = 0; i < st.filtered.size(); ++i)
        st.selected_indexes.push_back(i);
}

/// Apply the current EditorState buffer back to the edited rule entry.
inline void CommitEditorChanges(RuleListState& st) {
    if (!st.editing_idx || !st.editor.dirty) return;
    if (*st.editing_idx >= st.filtered.size()) return;

    RuleEntry upd_copy = st.input.rules[st.filtered[*st.editing_idx]];
    upd_copy.path_pattern = st.editor.pattern_buf.empty()
        ? std::nullopt
        : std::optional<std::string>{st.editor.pattern_buf};
    upd_copy.tool_pattern = st.editor.selected_tools.empty()
        ? std::string{"*"} : [&]() {
              std::string out = st.editor.selected_tools.front();
              for (std::size_t i = 1; i < st.editor.selected_tools.size(); ++i)
                  out += "|" + st.editor.selected_tools[i];
              return out; }();
    upd_copy.strategy    = static_cast<MatchStrategy>(st.editor.strategy_idx);
    upd_copy.scope       = static_cast<PermissionScope>(st.editor.scope_idx);
    upd_copy.action      = static_cast<PermissionAction>(st.editor.action_idx);
    upd_copy.description = st.editor.description_buf;
    if (std::size_t(st.editor.group_idx) < st.input.groups.size())
        upd_copy.group_id = st.input.groups[st.editor.group_idx].id;

    st.input.rules[st.filtered[*st.editing_idx]] = std::move(upd_copy);
    const RuleEntry& committed = st.input.rules[st.filtered[*st.editing_idx]];
    if (st.cbs.on_update) st.cbs.on_update(committed.id, committed);
    st.editor.dirty = false;
    st.status_bar_msg = std::format("saved rule {}",
                                    committed.id.substr(0, 14));
    detail::RefreshFiltered(st);
}

inline void LoadEditorForSelected(RuleListState& st, std::size_t fi) {
    if (fi >= st.filtered.size()) return;
    st.editing_idx = fi;
    const auto& r = st.input.rules[st.filtered[fi]];
    st.editor.pattern_buf     = r.path_pattern.value_or("");
    st.editor.description_buf = r.description;
    st.editor.strategy_idx = static_cast<int>(r.strategy);
    st.editor.scope_idx    = static_cast<int>(r.scope);
    st.editor.action_idx   = static_cast<int>(r.action);
    for (std::size_t gi = 0; gi < st.input.groups.size(); ++gi) {
        if (st.input.groups[gi].id == r.group_id) {
            st.editor.group_idx = (int)gi; break;
        }
    }
    st.editor.selected_tools.clear();
    if (r.tool_pattern != "*") {
        std::stringstream ss(r.tool_pattern);
        std::string tok;
        while (std::getline(ss, tok, '|'))
            st.editor.selected_tools.push_back(tok);
    }
    st.editor.dirty = false;
}

inline void StartNewRule(RuleListState& st) {
    RuleEntry blank;
    blank.id    = std::format("new_{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    blank.strategy = MatchStrategy::Glob;
    blank.action   = PermissionAction::Ask;
    blank.scope    = PermissionScope::Session;
    blank.enabled  = true;
    blank.priority = 50;
    blank.tool_pattern = "*";
    if (st.active_group_idx < st.input.groups.size())
        blank.group_id = st.input.groups[st.active_group_idx].id;
    st.input.rules.insert(st.input.rules.begin(), std::move(blank));
    detail::RefreshFiltered(st);
    st.rule_cursor = 0;
    st.anchor_index.reset();
    st.selected_indexes.clear();
    LoadEditorForSelected(st, 0);
    st.editor.dirty = true;
    st.focus = FocusZone::Editor;
    st.status_bar_msg = "new rule — edit & [Ctrl+s] save";
    if (st.cbs.on_add) st.cbs.on_add(st.input.rules.front());
}

inline void DeleteAtCursor(RuleListState& st) {
    if (st.filtered.empty()) return;
    const std::size_t fi = st.rule_cursor;
    const auto rid = st.input.rules[st.filtered[fi]].id;
    st.input.rules.erase(st.input.rules.begin() +
                         static_cast<std::ptrdiff_t>(st.filtered[fi]));
    detail::RefreshFiltered(st);
    if (st.cbs.on_delete) st.cbs.on_delete(rid);
    st.status_bar_msg = std::format("deleted rule {}", rid.substr(0, 14));
    if (st.editing_idx) { st.editing_idx.reset(); st.editor.dirty = false; }
}

inline void FireBatch(RuleListState& st, BatchOp op, std::string arg = "") {
    if (st.selected_indexes.empty()) {
        st.status_bar_msg = "nothing selected for batch";
        return;
    }
    auto ids = detail::SelectedRuleIds(st);
    for (auto si : st.selected_indexes) {
        if (si >= st.filtered.size()) continue;
        auto& r = st.input.rules[st.filtered[si]];
        switch (op) {
            case BatchOp::Enable:       r.enabled = true;  break;
            case BatchOp::Disable:      r.enabled = false; break;
            case BatchOp::SetStrategy: {
                for (std::size_t i = 0; i < detail::kStrategyNames.size(); ++i)
                    if (detail::kStrategyNames[i] == arg)
                        r.strategy = static_cast<MatchStrategy>(i);
                break;
            }
            case BatchOp::SetScope: {
                for (std::size_t i = 0; i < detail::kScopeNames.size(); ++i)
                    if (detail::kScopeNames[i] == arg)
                        r.scope = static_cast<PermissionScope>(i);
                break;
            }
            case BatchOp::SetAction: {
                for (std::size_t i = 0; i < detail::kActionNames.size(); ++i)
                    if (detail::kActionNames[i] == arg)
                        r.action = static_cast<PermissionAction>(i);
                break;
            }
            case BatchOp::Delete:
                // deferred to caller
                break;
            case BatchOp::ExportJson:
                break;
        }
        if (st.cbs.on_update) st.cbs.on_update(r.id, r);
    }
    if (op == BatchOp::Delete) {
        // Remove in reverse index order (stable with filtered[])
        std::vector<std::size_t> sorted = st.selected_indexes;
        std::sort(sorted.rbegin(), sorted.rend());
        for (auto si : sorted) {
            if (si >= st.filtered.size()) continue;
            auto it = st.input.rules.begin() +
                      static_cast<std::ptrdiff_t>(st.filtered[si]);
            st.input.rules.erase(it);
        }
        st.selected_indexes.clear();
    }
    detail::RefreshFiltered(st);
    if (st.cbs.on_batch) st.cbs.on_batch(std::move(ids), op, std::move(arg));
    st.status_bar_msg = std::format("batch op applied to {} rows",
                                    st.selected_indexes.size());
    st.selected_indexes.clear();
    st.anchor_index.reset();
}

} // namespace ev

// --- Event handlers per focus zone ---

namespace ev {

inline bool HandleGroups(RuleListState& st, Event e) {
    const std::size_t N = st.input.groups.size();

    if (e == Event::ArrowUp   || e == Event::Character('k')) {
        if (N == 0) return true;
        st.active_group_idx = (st.active_group_idx - 1 + N) % N;
        st.filters.group_id = st.input.groups[st.active_group_idx].id;
        detail::RefreshFiltered(st);
        return true;
    }
    if (e == Event::ArrowDown || e == Event::Character('j')) {
        if (N == 0) return true;
        st.active_group_idx = (st.active_group_idx + 1) % N;
        st.filters.group_id = st.input.groups[st.active_group_idx].id;
        detail::RefreshFiltered(st);
        return true;
    }
    if (e == Event::Return || e == Event::Character(' ')) {
        if (!st.filters.group_id.empty() &&
            st.filters.group_id ==
                st.input.groups[st.active_group_idx].id)
            st.filters.group_id.clear();
        else
            st.filters.group_id = st.input.groups[st.active_group_idx].id;
        detail::RefreshFiltered(st);
        return true;
    }
    if (e.is_character()) {
        const std::string& c = e.character();
        for (std::size_t gi = 0; gi < N; ++gi) {
            if (st.input.groups[gi].hotkey == c) {
                st.active_group_idx = gi;
                st.filters.group_id = st.input.groups[gi].id;
                detail::RefreshFiltered(st);
                return true;
            }
        }
    }
    return false;
}

inline bool HandleSearch(RuleListState& st, Event e) {
    if (e == Event::Escape) {
        st.search_buf.clear(); st.search_focused = false;
        detail::RefreshFiltered(st); return true;
    }
    if (e == Event::Return) { st.search_focused = false; return true; }
    if (e == Event::Backspace || e.input() == "\x7f") {
        if (!st.search_buf.empty()) st.search_buf.pop_back();
        detail::RefreshFiltered(st); return true;
    }
    if (e.is_character()) {
        st.search_buf += e.character();
        detail::RefreshFiltered(st); return true;
    }
    return false;
}

inline bool HandleRules(RuleListState& st, Event e) {
    const std::size_t N = st.filtered.size();

    auto has_shift = [](const Event& ev) {
        const std::string& s = ev.input();
        return s.size() >= 5 &&
               s.substr(0, 5) == "\x1B[1;" &&
               s.find(";2") != std::string::npos;
    };

    if (st.search_focused) return HandleSearch(st, e);
    if (e == Event::Character('/')) {
        st.search_focused = true; return true;
    }
    if (e.is_character()) {
        char ch = e.character().front();
        if (std::isprint(static_cast<unsigned char>(ch)) && ch != ' ') {
            st.search_focused = true;
            st.search_buf = std::string{1, ch};
            detail::RefreshFiltered(st);
            return true;
        }
    }
    if (e == Event::ArrowUp   || e == Event::Character('k')) {
        if (N == 0) return true;
        st.rule_cursor = (st.rule_cursor - 1 + N) % N; return true;
    }
    if (e == Event::ArrowDown || e == Event::Character('j')) {
        if (N == 0) return true;
        st.rule_cursor = (st.rule_cursor + 1) % N; return true;
    }
    if (e == Event::PageUp) {
        st.rule_cursor = st.rule_cursor < std::size_t(detail::kVScrollVisible)
            ? 0 : st.rule_cursor - detail::kVScrollVisible; return true;
    }
    if (e == Event::PageDown) {
        st.rule_cursor = std::min(N - 1,
            st.rule_cursor + detail::kVScrollVisible); return true;
    }
    if (e == Event::Home || e == Event::Character('g')) {
        st.rule_cursor = 0; return true;
    }
    if (e == Event::End  || e == Event::Character('G')) {
        if (N > 0) st.rule_cursor = N - 1; return true;
    }
    if (e == Event::Character(' ')) {
        ev::ToggleIndex(st, st.rule_cursor);
        if (!st.anchor_index) st.anchor_index = st.rule_cursor;
        return true;
    }
    auto matches_arrow_with_shift = [](const Event& ev, const Event& base) {
        if (ev == base) return true;
        const std::string& s = ev.input();
        const std::string& b = base.input();
        // Shift+Arrow:  \x1B[A/B/C/D  →  \x1B[1;2X
        return b.size() == 3 && b.substr(0, 2) == "\x1B[" &&
               s.size() == 7 && s.substr(0, 5) == "\x1B[1;" &&
               s[5] == '2' && s.back() == b.back();
    };

    if (has_shift(e) && (matches_arrow_with_shift(e, Event::ArrowDown) ||
                         matches_arrow_with_shift(e, Event::ArrowUp))) {
        if (N == 0) return true;
        if (matches_arrow_with_shift(e, Event::ArrowDown))
            st.rule_cursor = std::min(N - 1, st.rule_cursor + 1);
        else
            st.rule_cursor = st.rule_cursor ? st.rule_cursor - 1 : 0;
        ev::SelectRange(st, st.rule_cursor);
        return true;
    }
    if (e.input() == "ctrl+a") { ev::SelectAll(st); return true; }
    if (e == Event::Character('v')) {
        st.anchor_index = st.rule_cursor;
        ev::ToggleIndex(st, st.rule_cursor);
        return true;
    }
    if (e == Event::Character('n')) { ev::StartNewRule(st); return true; }
    if (e == Event::Delete || e == Event::Character('x')) {
        ev::DeleteAtCursor(st); return true;
    }
    if (e == Event::Return) {
        ev::LoadEditorForSelected(st, st.rule_cursor);
        ev::ToggleIndex(st, st.rule_cursor);
        st.focus = FocusZone::Editor; return true;
    }
    if (e == Event::Character('E')) { ev::FireBatch(st, BatchOp::Enable);  return true; }
    if (e == Event::Character('D')) { ev::FireBatch(st, BatchOp::Disable); return true; }
    if (e == Event::Character('X')) { ev::FireBatch(st, BatchOp::Delete);  return true; }
    if (e == Event::Character('J')) { ev::FireBatch(st, BatchOp::ExportJson); return true; }
    return false;
}

inline bool HandleEditor(RuleListState& st, Event e) {
    if (e.input() == "ctrl+s") { ev::CommitEditorChanges(st); return true; }
    if (e == Event::Escape) { st.editor.dirty = false; st.focus = FocusZone::Rules; return true; }

    if (e == Event::Character('h')) { st.editor.strategy_idx = (st.editor.strategy_idx + 3) % 4; st.editor.dirty = true; return true; }
    if (e == Event::Character('l')) { st.editor.strategy_idx = (st.editor.strategy_idx + 1) % 4; st.editor.dirty = true; return true; }
    if (e == Event::Character('H')) { st.editor.scope_idx    = (st.editor.scope_idx    + 3) % 4; st.editor.dirty = true; return true; }
    if (e == Event::Character('L')) { st.editor.scope_idx    = (st.editor.scope_idx    + 1) % 4; st.editor.dirty = true; return true; }
    if (e == Event::ArrowLeft)  { st.editor.action_idx = (st.editor.action_idx + 3) % 4; st.editor.dirty = true; return true; }
    if (e == Event::ArrowRight) { st.editor.action_idx = (st.editor.action_idx + 1) % 4; st.editor.dirty = true; return true; }

    auto G = (int)st.input.groups.size();
    if (e == Event::Character('<')) { st.editor.group_idx = (st.editor.group_idx - 1 + G) % G; st.editor.dirty = true; return true; }
    if (e == Event::Character('>')) { st.editor.group_idx = (st.editor.group_idx + 1) % G; st.editor.dirty = true; return true; }

    static constexpr std::array<const char*, 8> kToolNames = {
        "BashTool","FileEditTool","FileWriteTool","FileReadTool",
        "WebFetchTool","MCPTool","GlobTool","SkillTool",
    };
    if (e.is_character()) {
        char ch = e.character().front();
        if (ch >= '1' && ch <= '8') {
            int idx = ch - '1';
            std::string t = kToolNames[idx];
            auto it = std::find(st.editor.selected_tools.begin(),
                                st.editor.selected_tools.end(), t);
            if (it == st.editor.selected_tools.end())
                st.editor.selected_tools.push_back(std::move(t));
            else
                st.editor.selected_tools.erase(it);
            st.editor.dirty = true;
            return true;
        }
    }

    if (e.is_character()) {
        auto c = e.character();
        if (c == "\b" || c == "\x7f") {
            if (!st.editor.description_buf.empty())
                st.editor.description_buf.pop_back();
            st.editor.dirty = true;
            return true;
        }
        if (c.size() == 1 &&
            std::isprint(static_cast<unsigned char>(c.front()))) {
            if (st.editor.description_buf.size() <
                detail::kDescriptionRows * detail::kDescriptionCols) {
                st.editor.description_buf += c.front();
                st.editor.dirty = true;
            }
            return true;
        }
    }
    return false;
}

inline bool HandleToolbar(RuleListState& st, Event e) {
    if (e == Event::ArrowLeft  || e == Event::Character('h')) {
        st.toolbar_cursor = (st.toolbar_cursor + 3) % 4; return true;
    }
    if (e == Event::ArrowRight || e == Event::Character('l')) {
        st.toolbar_cursor = (st.toolbar_cursor + 1) % 4; return true;
    }
    if (e == Event::Return || e == Event::Character(' ')) {
        switch (st.toolbar_cursor) {
            case 0: ev::StartNewRule(st);                         return true;
            case 1:
                st.focus = FocusZone::ImportModal;
                st.import_buf.clear();
                st.status_bar_msg = "paste JSON rules";
                return true;
            case 2: {
                std::string path = st.cbs.on_export ? st.cbs.on_export()
                                                    : std::string{};
                st.status_bar_msg = path.empty()
                    ? std::string{"exported (no callback)"}
                    : std::format("exported → {}", path);
                return true;
            }
            case 3: {
                // Diff = current state vs initial state (best-effort).
                st.show_diff = true;
                st.focus = FocusZone::DiffModal;
                return true;
            }
        }
    }
    if (e == Event::Character('n')) { ev::StartNewRule(st); return true; }
    if (e == Event::Character('i')) {
        st.focus = FocusZone::ImportModal; st.import_buf.clear(); return true;
    }
    if (e == Event::Character('e')) {
        std::string path = st.cbs.on_export ? st.cbs.on_export()
                                            : std::string{};
        st.status_bar_msg = path.empty()
            ? std::string{"exported (no callback)"}
            : std::format("exported → {}", path);
        return true;
    }
    if (e == Event::Character('d')) {
        st.show_diff = true; st.focus = FocusZone::DiffModal; return true;
    }
    return false;
}

inline bool HandleImportModal(RuleListState& st, Event e) {
    if (e == Event::Escape) {
        st.focus = FocusZone::Toolbar; return true;
    }
    if (e == Event::Return) {
        if (st.cbs.on_import && !st.import_buf.empty())
            st.cbs.on_import(st.import_buf);
        st.status_bar_msg = std::format("imported {} bytes",
                                        st.import_buf.size());
        st.focus = FocusZone::Toolbar;
        return true;
    }
    if (e == Event::Backspace || e.input() == "\x7f") {
        if (!st.import_buf.empty()) st.import_buf.pop_back();
        return true;
    }
    if (e.is_character()) {
        st.import_buf += e.character();
        return true;
    }
    return false;
}

inline bool HandleDiffModal(RuleListState& st, Event e) {
    if (e == Event::Escape || e == Event::Character('q')) {
        st.focus = FocusZone::Toolbar;
        st.show_diff = false;
        return true;
    }
    return false;
}

} // namespace ev

// --- Public factory ---

[[nodiscard]] inline Component MakePermissionRuleList(
    RuleListInput input, RuleListCallbacks cbs)
{
    auto st = std::make_shared<RuleListState>();
    st->input = std::move(input);
    st->cbs   = std::move(cbs);

    if (st->input.groups.empty()) {
        auto def = detail::DefaultGroups();
        st->input.groups.assign(def.begin(), def.end());
    }
    if (st->active_group_idx >= st->input.groups.size())
        st->active_group_idx = 0;

    if (st->input.hit_samples.empty()) {
        st->hit_samples.reserve(3);
        static constexpr std::array<const char*, 3> kTools = {
            "FileReadTool", "BashTool", "FileWriteTool"
        };
        for (int i = 0; i < 3; ++i) {
            st->hit_samples.push_back(HitSample{
                detail::kDefaultSamples[i],
                kTools[i],
                std::string{"sample "} + char('A' + i),
            });
        }
    } else {
        st->hit_samples = std::move(st->input.hit_samples);
    }

    st->filters = st->input.filters;
    st->search_buf = st->input.search_query;
    {
        std::ostringstream oss;
        oss << std::format("// {} rules (before)\n",
                           st->input.rules.size());
        for (const auto& r : st->input.rules) {
            oss << std::format(
                "id={} tool={} path={} strategy={} action={} scope={}\n",
                r.id, r.tool_pattern,
                r.path_pattern.value_or("(none)"),
                detail::StrategyName(r.strategy),
                detail::ActionName(r.action),
                detail::ScopeName(r.scope));
        }
        st->diff_old = oss.str();
        st->diff_new = st->diff_old;   // no diff initially
    }

    detail::RefreshFiltered(*st);

    return Renderer([st] { return RenderRuleList(st); })
         | CatchEvent([st](Event e) -> bool {
        // Global focus cycle (Tab / Shift+Tab)
        if (e == Event::Tab) {
            ev::CycleFocusForward(*st);
            return true;
        }
        if (e == Event::TabReverse) {
            ev::CycleFocusBack(*st);
            return true;
        }

        // Zone-specific handlers
        switch (st->focus) {
            case FocusZone::ImportModal:
                if (ev::HandleImportModal(*st, e)) return true;
                break;
            case FocusZone::DiffModal:
                if (ev::HandleDiffModal(*st, e)) return true;
                break;
            case FocusZone::Groups:
                if (ev::HandleGroups(*st, e)) return true;
                break;
            case FocusZone::Rules:
                if (ev::HandleRules(*st, e)) return true;
                break;
            case FocusZone::Editor:
                if (ev::HandleEditor(*st, e)) return true;
                break;
            case FocusZone::Toolbar:
                if (ev::HandleToolbar(*st, e)) return true;
                break;
        }

        // Global Esc → close only when inside a safe zone
        if (e == Event::Character('q')) {
            // Leave focus as-is; caller controls lifetime via unmount
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::permissions::rule_list

// ═══════════════════════════════════════════════════════════════════════════
// NEW CONTENT FOR P2-04: Permissions rules UI tabs
// 7 additional functions + cc::utils::permissions_engine singleton state.
// ═══════════════════════════════════════════════════════════════════════════

// =========================================================================
// cc::utils::permissions_engine – NEW namespace: denial + workspace state
// =========================================================================
// This namespace is declared locally because the existing cc.utils.permissions_engine
// module exports into cc::utils::permissions.  We follow the task spec and
// use a distinct namespace so that callers can write
//   cc::utils::permissions_engine::recent_denials(50)
// per the P2-04 contract.
// =========================================================================

export namespace cc::utils::permissions_engine {

using namespace ftxui;
namespace fs = std::filesystem;

// --- Denial tracking -----------------------------------------------------

/// A single auto-mode / denied tool invocation.
struct DenialEntry {
    int64_t     ts_ms = 0;         // epoch ms (we use system_clock)
    std::string tool_name;         // e.g. "Bash", "FileWrite"
    std::string action;            // short human summary
    std::string path;              // affected path / target
    std::string deny_reason;       // why it was denied
    bool        resolved = false;
    std::string rule_that_would_allow;
};

// --- Workspace directory -------------------------------------------------

struct WorkspaceEntry {
    fs::path path;
    enum class Policy { Allow, Deny, Default } policy = Policy::Default;
    bool is_default = false;
};

// --- Singleton storage (GlobalStateSlot pattern) ------------------------
// Uses an unnamed namespace with a mutex + vector, matching how
// cc::utils::permissions::PermissionEngine stores rules in permissions_engine.cppm.

namespace rl_anon_0 {

struct GlobalStateSlot {
    mutable std::mutex               mx;
    std::vector<DenialEntry>         denials;
    std::vector<WorkspaceEntry>      workspaces;
};

GlobalStateSlot& g_state() noexcept {
    static GlobalStateSlot s;
    return s;
}

} // namespace rl_anon_0
using namespace rl_anon_0; // unnamed namespace

// --- Denial public API ---------------------------------------------------

/// Return up to `limit` most recent denials, sorted newest first.
[[nodiscard]] inline auto recent_denials(int limit = 50)
    -> std::vector<DenialEntry>
{
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    std::vector<DenialEntry> out = s.denials;
    // newest first (stable by ts_ms; already push_back in order)
    std::reverse(out.begin(), out.end());
    if (limit >= 0 && static_cast<int>(out.size()) > limit)
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

/// Mark the entry at filtered position `idx` (0 = newest) as acknowledged.
/// `idx` is interpreted relative to recent_denials() order.
inline auto acknowledge(int idx) -> void {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    if (s.denials.empty()) return;
    const int n = static_cast<int>(s.denials.size());
    // 0 (newest) -> s.denials.back(); convert index
    int real = n - 1 - idx;
    if (real < 0 || real >= n) return;
    s.denials[static_cast<std::size_t>(real)].resolved = true;
}

/// Internal helper: push a fresh denial.  Used by unit tests and by hooks
/// that feed denials into the UI.
[[nodiscard]] inline auto push_denial(DenialEntry d) -> void {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    if (d.ts_ms == 0) {
        using namespace std::chrono;
        const auto now = system_clock::now().time_since_epoch();
        d.ts_ms = duration_cast<milliseconds>(now).count();
    }
    s.denials.push_back(std::move(d));
    // Cap at 500 entries (FIFO eviction of oldest)
    constexpr std::size_t kCap = 500;
    while (s.denials.size() > kCap)
        s.denials.erase(s.denials.begin());
}

/// Reset for unit tests only.
inline auto __test_reset_denials() -> void {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    s.denials.clear();
}

// --- Workspace public API ------------------------------------------------

[[nodiscard]] inline auto workspace_directories()
    -> std::vector<WorkspaceEntry>
{
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    return s.workspaces;
}

[[nodiscard]] inline auto add_workspace_dir(fs::path p,
                                            WorkspaceEntry::Policy policy)
    -> std::expected<void, std::string>
{
    if (p.empty())
        return std::unexpected(std::string{"path must not be empty"});
    std::error_code ec;
    p = fs::weakly_canonical(p, ec); // best-effort normalization
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    for (const auto& w : s.workspaces) {
        if (fs::equivalent(w.path, p, ec) || w.path == p) {
            return std::unexpected(
                std::string{"workspace directory already exists"});
        }
    }
    s.workspaces.push_back(WorkspaceEntry{
        .path = std::move(p),
        .policy = policy,
        .is_default = false,
    });
    return {};
}

[[nodiscard]] inline auto remove_workspace_dir(const fs::path& p) -> bool {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    std::error_code ec;
    auto it = std::find_if(s.workspaces.begin(), s.workspaces.end(),
        [&](const WorkspaceEntry& w) {
            if (w.is_default) return false; // cannot remove defaults
            return fs::equivalent(w.path, p, ec) || w.path == p;
        });
    if (it == s.workspaces.end()) return false;
    s.workspaces.erase(it);
    return true;
}

/// Seed a default workspace entry (used during startup to reflect the
/// initial project directory).  The `is_default = true` flag makes the
/// remove button a no-op.
inline auto seed_default_workspace(fs::path p) -> void {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    // Only add if no default entry exists yet AND exact path not present.
    for (const auto& w : s.workspaces) {
        std::error_code ec;
        if (fs::equivalent(w.path, p, ec) || w.path == p) return;
    }
    s.workspaces.push_back(WorkspaceEntry{
        .path = std::move(p),
        .policy = WorkspaceEntry::Policy::Default,
        .is_default = true,
    });
}

/// Reset for unit tests only.
inline auto __test_reset_workspaces() -> void {
    auto& s = g_state();
    std::lock_guard<std::mutex> lk(s.mx);
    s.workspaces.clear();
}

} // namespace cc::utils::permissions_engine

// =========================================================================
// Now extend cc::ui::permissions::rule_list with the 7 builder functions.
// =========================================================================

export namespace cc::ui::permissions::rule_list {

using namespace ftxui;
namespace fs = std::filesystem;
namespace peng = cc::utils::permissions_engine;
namespace pc   = cc::ui::permissions::components;
namespace dt   = cc::ui::design::tokens;
namespace eng  = cc::utils::permissions;

// =========================================================================
// Shared helpers used by multiple builders
// =========================================================================

namespace ui_tabs_detail {

/// Convert a millisecond timestamp delta to a compact "5s ago" / "3m ago"
/// / "2h ago" / "1d ago" string.
[[nodiscard]] inline std::string TimeAgo(int64_t ts_ms) {
    using namespace std::chrono;
    const auto now_ms = duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
    int64_t diff = now_ms - ts_ms;
    if (diff < 0) diff = 0;
    const int64_t s = diff / 1000;
    if (s < 60)
        return std::format("{}s ago", std::max<int64_t>(1, s));
    const int64_t m = s / 60;
    if (m < 60) return std::format("{}m ago", m);
    const int64_t h = m / 60;
    if (h < 24) return std::format("{}h ago", h);
    const int64_t d = h / 24;
    return std::format("{}d ago", d);
}

/// Apply a substring filter on the text fields of a denial row.
[[nodiscard]] inline bool DenialMatches(const peng::DenialEntry& d,
                                        std::string_view filter) {
    if (filter.empty()) return true;
    std::string f(filter);
    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto in = [&](std::string_view sv) {
        std::string hay(sv);
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return hay.find(f) != std::string::npos;
    };
    return in(d.tool_name) || in(d.action) || in(d.path) || in(d.deny_reason);
}

/// Simple "text + [ok/cancel]" dialog wrapper rendered on top of dbox, used
/// by BuildAddWorkspaceDirectoryModal and BuildRemoveWorkspaceDirectoryConfirm.
[[nodiscard]] inline Element DialogFrame(std::string_view title,
                                         Color accent,
                                         Element body,
                                         Element footer) {
    return dbox({
        text("") | clear_under,
        window(
            hbox({text(std::string{" "}), text(std::string{title})}) | bold | color(accent),
            vbox({
                std::move(body) | size(WIDTH, GREATER_THAN, 40),
                separator(),
                std::move(footer),
            }) | size(WIDTH, GREATER_THAN, 50)
               | size(HEIGHT, GREATER_THAN, 6)
        ) | color(accent),
    });
}

/// Render a decision badge (colored pill) for Allow/Deny/Abort actions.
[[nodiscard]] inline Element DecisionBadge(eng::PermissionAction a) {
    std::string_view label;
    Color c;
    switch (a) {
        case eng::PermissionAction::Allow:
            label = "ALLOW"; c = Color::Green; break;
        case eng::PermissionAction::AskOnce:
            label = "ALLOW_ONCE"; c = Color::GreenLight; break;
        case eng::PermissionAction::Ask:
            label = "ASK"; c = Color::Yellow; break;
        case eng::PermissionAction::Deny:
            label = "DENY"; c = Color::Red; break;
    }
    return text(std::format(" {} ", label)) | color(c) | bgcolor(Color::RGB(20,20,24)) | bold;
}

[[nodiscard]] inline Element WorkspacePolicyBadge(peng::WorkspaceEntry::Policy p,
                                                  bool is_default) {
    if (is_default) {
        return text(" DEFAULT ") | color(Color::GrayLight)
               | bgcolor(Color::RGB(40,40,46)) | dim;
    }
    switch (p) {
        case peng::WorkspaceEntry::Policy::Allow:
            return text(" ALLOW ") | color(Color::Green)
                   | bgcolor(Color::RGB(16,36,24)) | bold;
        case peng::WorkspaceEntry::Policy::Deny:
            return text(" DENY ") | color(Color::Red)
                   | bgcolor(Color::RGB(40,16,20)) | bold;
        case peng::WorkspaceEntry::Policy::Default:
            return text(" DEFAULT ") | color(Color::GrayLight)
                   | bgcolor(Color::RGB(40,40,46)) | dim;
    }
    return text("");
}

} // namespace ui_tabs_detail

// =========================================================================
// Forward declarations of modal builders (defined later in this file).
// =========================================================================

[[nodiscard]] Component BuildAddWorkspaceDirectoryModal(
    bool& open,
    std::shared_ptr<std::expected<void, std::string>> result_ref,
    std::function<void(fs::path, peng::WorkspaceEntry::Policy)> on_ok,
    std::function<void()> on_cancel);

[[nodiscard]] Component BuildRemoveWorkspaceDirectoryConfirm(
    bool& open,
    const peng::WorkspaceEntry& entry,
    std::function<void()> on_yes,
    std::function<void()> on_no);

/// Resolve a Component to its rendered Element.  FTXUI's Button() returns a
/// Component; many hbox/vbox layouts below compose buttons, so we render them
/// to Elements here.  Decorators applied via `|` (color, bold, size, ...) on
/// a Component are honoured because `Component | Decorator` returns a new
/// Component whose Render() applies the decoration.
[[nodiscard]] inline Element CompEl(Component c) {
    return c ? c->Render() : emptyElement();
}

// =========================================================================
// New public state types (declared here so the header-reading callers have
// a stable definition; they are the input structs for each builder below).
// =========================================================================

/// View-model for the Recent Denials tab.  If `denials` is empty the builder
/// falls back to cc::utils::permissions_engine::recent_denials().
struct RecentDenialsState {
    std::optional<std::vector<peng::DenialEntry>> denials;
};

/// View-model for the Workspaces tab.  Falls back to
/// cc::utils::permissions_engine::workspace_directories() when empty.
struct WorkspaceState {
    std::optional<std::vector<peng::WorkspaceEntry>> entries;
};

/// Application-facing model for the 4-tab permissions panel.  Mirrors the
/// `cc::ui::permissions::PermissionsPanelModel` declared in
/// `permission_rules_ui.cppm` (same field layout so the wrapper there can
/// forward a copy without touching its own struct definition).
struct PermissionsPanelModel {
    std::optional<RuleListInput>      rule_list;
    std::optional<RecentDenialsState> denials;
    std::optional<WorkspaceState>     workspaces;
};

/// Callback bundle for the 4-tab permissions panel.  Same shape as
/// `cc::ui::permissions::PermissionsPanelCallbacks`.
struct PermissionsPanelCallbacks {
    std::function<void(const RuleEntry&)>                 on_add_rule;
    std::function<void(std::string_view, const RuleEntry&)> on_update_rule;
    std::function<void(std::string_view)>                 on_delete_rule;
    std::function<void(int idx)>                          on_allow_once;
    std::function<void(int idx)>                          on_always_allow;
    std::function<void(fs::path)>                         on_add_workspace;
    std::function<void(fs::path)>                         on_remove_workspace;
};

/// Aggregate model + callbacks container, used by BuildPermissionsTabs.
struct PermissionsTabsState {
    PermissionsPanelModel      model;
    PermissionsPanelCallbacks  callbacks;
};

/// Active-tab selector for the 4-tab permissions panel.  Values must match
/// `cc::ui::permissions::PermTab` declared in `permission_rules_ui.cppm`
/// (used by the wrapper that owns the tab bar UI).  Keeping the enum here lets
/// BuildPermissionsTabs route renders/events without cross-module imports.
enum class PermTab : std::uint8_t {
    AllRules   = 0,
    Denials    = 1,
    Workspaces = 2,
    CreateRule = 3,
};

// =========================================================================
// (a) BuildRecentDenialsTab
// =========================================================================

[[nodiscard]] inline Component BuildRecentDenialsTab(
    RecentDenialsState state,
    std::function<void(int idx)> on_allow_once,
    std::function<void(int idx)> on_always_allow)
{
    using ui_tabs_detail::TimeAgo;
    using ui_tabs_detail::DenialMatches;

    // Shared state cells.
    auto filter_buf = std::make_shared<std::string>();
    auto cursor     = std::make_shared<int>(0);
    auto filter_active = std::make_shared<bool>(false);

    auto data_fn = [state]() -> std::vector<peng::DenialEntry> {
        if (state.denials.has_value()) return *state.denials;
        return peng::recent_denials(50);
    };

    auto filtered_fn = [data_fn, filter_buf]() -> std::vector<peng::DenialEntry> {
        auto all = data_fn();
        std::vector<peng::DenialEntry> out;
        out.reserve(all.size());
        for (auto& d : all) {
            if (DenialMatches(d, *filter_buf))
                out.push_back(std::move(d));
        }
        return out;
    };

    return Renderer([cursor, filter_buf, filtered_fn, on_allow_once, on_always_allow, filter_active, data_fn] {
        auto rows = filtered_fn();
        const int n = static_cast<int>(rows.size());
        if (*cursor >= n) *cursor = std::max(0, n - 1);

        Elements lines;
        // Header row
        lines.push_back(hbox({
            text(" Recent Denials ") | bold | color(Color::Red),
            text(std::format(" {} ", n))
                | bold | color(Color::White) | bgcolor(Color::Red),
            filler(),
            text("[j/k] scroll  [a] allow once  [A] always allow  [/] filter")
                | dim,
        }));
        lines.push_back(pc::ThinDivider());

        // Optional filter row
        if (!filter_buf->empty() || true) {
            lines.push_back(hbox({
                text(" Filter: ") | color(Color::Cyan) | dim,
                text(filter_buf->empty()
                         ? std::string{"(type / then a query to filter)"}
                         : *filter_buf + std::string{"| "})
                    | color(filter_buf->empty()
                                ? Color::GrayDark : Color::CyanLight),
            }));
            lines.push_back(pc::ThinDivider());
        }

        // Column header
        lines.push_back(hbox({
            text(" When ") | bold | dim | size(WIDTH, EQUAL, 10),
            text(" Tool ") | bold | dim | size(WIDTH, EQUAL, 16),
            text(" Path / Target ") | bold | dim | xflex_grow,
            text(" Reason ") | bold | dim | size(WIDTH, EQUAL, 20),
            text(" Actions ") | bold | dim | size(WIDTH, EQUAL, 22),
        }));
        lines.push_back(separator() | dim);

        // Data rows or empty state
        if (rows.empty()) {
            for (int i = 0; i < 5; ++i) {
                if (i == 2) {
                    lines.push_back(hbox({
                        filler(),
                        text(" No permission denials yet ")
                            | dim | color(Color::Green) | center,
                        filler(),
                    }));
                } else {
                    lines.push_back(text(""));
                }
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const auto& d = rows[i];
                const bool sel = (i == *cursor);
                Element prefix = sel
                    ? text("> ") | color(Color::Yellow)
                    : text("  ");
                Element when = text(" " + TimeAgo(d.ts_ms) + " ")
                    | size(WIDTH, EQUAL, 10)
                    | (d.resolved ? dim : color(Color::CyanLight));
                Element tool = text(" " + d.tool_name + " ")
                    | size(WIDTH, EQUAL, 16)
                    | (sel ? bold : nothing);
                Element path = pc::PathLabel(
                    d.path.empty() ? d.action : d.path, 40)
                    | xflex_grow
                    | (d.resolved ? dim : nothing);
                Element reason = pc::PathLabel(d.deny_reason, 18)
                    | size(WIDTH, EQUAL, 20)
                    | color(Color::RedLight)
                    | (d.resolved ? dim : nothing);

                Element btns = hbox({
                    CompEl(Button(" Allow once ", [on_allow_once, i] {
                               if (on_allow_once) on_allow_once(i);
                           }) | color(Color::Cyan) | size(WIDTH, EQUAL, 12)),
                    text(" "),
                    CompEl(Button(" Always allow ", [on_always_allow, i] {
                               if (on_always_allow) on_always_allow(i);
                           }) | color(Color::Green) | size(WIDTH, EQUAL, 14)),
                }) | size(WIDTH, EQUAL, 22);

                Element row = hbox({
                    std::move(prefix),
                    std::move(when),
                    std::move(tool),
                    std::move(path),
                    std::move(reason),
                    std::move(btns),
                });
                if (sel) row = row | bgcolor(Color::RGB(30, 32, 42));
                if (d.resolved) row = row | dim;
                lines.push_back(std::move(row));
                if (i + 1 < n) lines.push_back(separator() | dim);
            }
        }

        lines.push_back(pc::ThinDivider());
        lines.push_back(hbox({
            text(std::format(" {} total / {} filtered ",
                             data_fn().size(), n)) | dim,
            filler(),
            text(filter_buf->empty()
                     ? std::string{}
                     : std::format(" filter: '{}' ", *filter_buf))
                | color(Color::Cyan) | dim,
        }));
        return vbox(std::move(lines)) | yflex_grow | yframe;
    }) | CatchEvent([cursor, filter_buf, filter_active](Event e) mutable {
        // Filter mode (/) uses the '/' key to enter filter mode, after which
        // printable chars update the filter.  Backspace, Esc still work in
        // and out of filter mode.
        if (e == Event::Character('/')) {
            *filter_active = true;
            filter_buf->clear();
            return true;
        }
        if (e == Event::Escape) {
            if (*filter_active) {
                *filter_active = false;
                filter_buf->clear();
            }
            return true;
        }
        if (*filter_active) {
            if (e == Event::Backspace || e.input() == "\x7f") {
                if (!filter_buf->empty()) filter_buf->pop_back();
                return true;
            }
            if (e.is_character()) {
                const char ch = e.character().front();
                if (std::isprint(static_cast<unsigned char>(ch))) {
                    *filter_buf += ch;
                    return true;
                }
            }
            return false;
        }
        // Scroll / cursor navigation.
        if (e == Event::ArrowDown || e == Event::Character('j')) {
            ++(*cursor); return true;
        }
        if (e == Event::ArrowUp   || e == Event::Character('k')) {
            if (*cursor > 0) --(*cursor);
            return true;
        }
        if (e == Event::Home || e == Event::Character('g')) {
            *cursor = 0; return true;
        }
        if (e == Event::End  || e == Event::Character('G')) {
            *cursor = 99999; return true; // renderer clamps
        }
        return false;
    });
}

// =========================================================================
// (b) BuildWorkspaceTab
// =========================================================================

[[nodiscard]] inline Component BuildWorkspaceTab(
    WorkspaceState state,
    std::function<void(const fs::path&, peng::WorkspaceEntry::Policy)> on_add,
    std::function<void(const fs::path&)> on_remove)
{
    using ui_tabs_detail::WorkspacePolicyBadge;
    auto cursor = std::make_shared<int>(0);

    // Modal state cells shared with BuildAddWorkspaceDirectoryModal and
    // BuildRemoveWorkspaceDirectoryConfirm (both in-scope via Renderer below).
    auto add_modal_open   = std::make_shared<bool>(false);
    auto rem_modal_open   = std::make_shared<bool>(false);
    auto add_result       = std::make_shared<std::expected<void, std::string>>(std::in_place);
    auto pending_remove   = std::make_shared<std::optional<peng::WorkspaceEntry>>();

    auto list_fn = [state]() -> std::vector<peng::WorkspaceEntry> {
        if (state.entries.has_value()) return *state.entries;
        return peng::workspace_directories();
    };

    auto do_add = [add_modal_open, on_add](fs::path p, peng::WorkspaceEntry::Policy pol) {
        *add_modal_open = false;
        if (on_add) on_add(std::move(p), pol);
        else {
            // Default: mutate the engine singleton directly.
            peng::add_workspace_dir(std::move(p), pol);
        }
    };

    auto do_remove = [rem_modal_open, on_remove, pending_remove](bool yes) {
        if (yes && pending_remove->has_value()) {
            const auto p = (*pending_remove)->path;
            if (on_remove) on_remove(p);
            else peng::remove_workspace_dir(p);
        }
        *pending_remove = std::nullopt;
        *rem_modal_open = false;
    };

    return Renderer([cursor, list_fn, add_modal_open, rem_modal_open,
                     add_result, pending_remove, on_remove,
                     do_add, do_remove] {
        auto rows = list_fn();
        const int n = static_cast<int>(rows.size());
        if (*cursor >= n) *cursor = std::max(0, n - 1);

        Elements lines;
        lines.push_back(hbox({
            text(" Workspaces ") | bold | color(Color::Blue),
            text(std::format(" {} ", n)) | dim,
            filler(),
            CompEl(Button(" + Add directory ", [add_modal_open] {
                *add_modal_open = true;
            }) | color(Color::Green) | bold),
        }));
        lines.push_back(pc::ThinDivider());

        // Columns: policy / path / default-badge / remove button
        lines.push_back(hbox({
            text(" Policy ") | bold | dim | size(WIDTH, EQUAL, 12),
            text(" Path ")   | bold | dim | xflex_grow,
            text(" ")        | bold | dim | size(WIDTH, EQUAL, 10),
            text(" Action ") | bold | dim | size(WIDTH, EQUAL, 12),
        }));
        lines.push_back(separator() | dim);

        if (rows.empty()) {
            for (int i = 0; i < 4; ++i) {
                if (i == 1) {
                    lines.push_back(hbox({
                        filler(),
                        text(" No workspace directories yet — click [+ Add] ")
                            | dim | color(Color::Yellow),
                        filler(),
                    }));
                } else lines.push_back(text(""));
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const auto& w = rows[i];
                const bool sel = (i == *cursor);
                Element prefix = sel ? text("> ") | color(Color::Yellow)
                                     : text("  ");
                Element badge = WorkspacePolicyBadge(w.policy, w.is_default)
                              | size(WIDTH, EQUAL, 12);
                Element path_el = pc::PathLabel(
                    w.path.string(), 60) | xflex_grow;
                Element def_el = w.is_default
                    ? text(" DEFAULT ") | dim | color(Color::GrayLight)
                                       | size(WIDTH, EQUAL, 10)
                    : text("") | size(WIDTH, EQUAL, 10);

                auto rem_lambda = [rem_modal_open, pending_remove, w_copy = w] {
                    *pending_remove = w_copy;
                    *rem_modal_open = true;
                };
                auto btn = Button(" Remove ", std::move(rem_lambda))
                         | color(Color::Red)
                         | size(WIDTH, EQUAL, 12);
                if (w.is_default) btn = btn | dim; // disabled visual

                Element row = hbox({
                    std::move(prefix),
                    std::move(badge),
                    std::move(path_el),
                    std::move(def_el),
                    CompEl(std::move(btn)),
                });
                if (sel) row = row | bgcolor(Color::RGB(20, 28, 48));
                lines.push_back(std::move(row));
                if (i + 1 < n) lines.push_back(separator() | dim);
            }
        }

        lines.push_back(pc::ThinDivider());
        lines.push_back(hbox({
            text(" [j/k] navigate  [+] add  [x] remove ") | dim,
            filler(),
            text(" Default entries cannot be removed ") | dim,
        }));

        Element body = vbox(std::move(lines)) | yflex_grow | yframe;

        // Overlay modals on top using dbox
        if (*add_modal_open) {
            auto modal = BuildAddWorkspaceDirectoryModal(
                *add_modal_open, add_result, do_add,
                [add_modal_open] { *add_modal_open = false; });
            body = dbox({body, modal->Render()});
        }
        if (*rem_modal_open && pending_remove->has_value()) {
            auto modal = BuildRemoveWorkspaceDirectoryConfirm(
                *rem_modal_open, **pending_remove,
                [do_remove] { do_remove(true); },
                [do_remove] { do_remove(false); });
            body = dbox({body, modal->Render()});
        }

        return body;
    }) | CatchEvent([cursor, list_fn, add_modal_open, rem_modal_open](Event e) {
        if (e == Event::ArrowDown || e == Event::Character('j')) {
            ++(*cursor); return true;
        }
        if (e == Event::ArrowUp || e == Event::Character('k')) {
            if (*cursor > 0) --(*cursor);
            return true;
        }
        if (e == Event::Character('+') || e == Event::Character('n')) {
            *add_modal_open = true; return true;
        }
        if (e == Event::Character('x') || e == Event::Delete) {
            *rem_modal_open = true; return true;
        }
        return false;
    });
}

// =========================================================================
// (c) BuildAddWorkspaceDirectoryModal
// =========================================================================

[[nodiscard]] inline Component BuildAddWorkspaceDirectoryModal(
    bool& open,
    std::shared_ptr<std::expected<void, std::string>> result_ref,
    std::function<void(fs::path, peng::WorkspaceEntry::Policy)> on_ok,
    std::function<void()> on_cancel)
{
    using ui_tabs_detail::DialogFrame;

    // Form state cells shared across renders.
    auto path_buf   = std::make_shared<std::string>();
    auto policy_idx = std::make_shared<int>(0); // 0=Allow 1=Deny 2=Default
    static constexpr std::array<const char*, 3> kPolicies = {
        "Allow", "Deny", "Default",
    };

    // Detect duplicate paths in the current workspace list.
    auto is_duplicate = [&path_buf]() -> std::optional<std::string> {
        if (path_buf->empty()) return std::nullopt;
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(*path_buf, ec);
        const auto list = peng::workspace_directories();
        for (const auto& w : list) {
            if (fs::equivalent(w.path, p, ec) || w.path == p)
                return std::string{"duplicate workspace: " + w.path.string()};
        }
        return std::nullopt;
    };

    auto submit = [&open, path_buf, policy_idx, on_ok, result_ref, is_duplicate]() {
        if (path_buf->empty()) {
            *result_ref = std::unexpected(
                std::string{"directory path is required"});
            return;
        }
        const auto dup = is_duplicate();
        if (dup.has_value()) {
            *result_ref = std::unexpected(*dup);
            return;
        }
        auto pol = peng::WorkspaceEntry::Policy::Allow;
        switch (*policy_idx) {
            case 0: pol = peng::WorkspaceEntry::Policy::Allow; break;
            case 1: pol = peng::WorkspaceEntry::Policy::Deny;  break;
            case 2: pol = peng::WorkspaceEntry::Policy::Default; break;
        }
        if (on_ok) on_ok(fs::path{*path_buf}, pol);
        *result_ref = {};
        open = false;
    };

    return Renderer([&open, path_buf, policy_idx, on_cancel, submit,
                     is_duplicate, result_ref] {
        const auto dup_msg = is_duplicate();
        Element dup_line = dup_msg.has_value()
            ? text(" ! " + *dup_msg) | color(Color::Red) | bold
            : text("");

        Element err_line = (!result_ref->has_value() && !dup_msg.has_value())
            ? text(" ! " + result_ref->error()) | color(Color::Red) | bold
            : text("");

        // Render policy radiobox labels
        Elements rb;
        for (std::size_t i = 0; i < kPolicies.size(); ++i) {
            const bool sel = (*policy_idx == static_cast<int>(i));
            Color c = (i == 0) ? Color::Green
                    : (i == 1) ? Color::Red   : Color::GrayLight;
            rb.push_back(
                text(std::format(" {} ", kPolicies[i]))
                | (sel ? (color(c) | inverted | bold) : dim)
            );
            if (i + 1 < kPolicies.size()) rb.push_back(text("  "));
        }

        Element body = vbox({
            hbox({
                text(" Directory: ") | dim,
                text(path_buf->empty()
                         ? std::string{"(type or use [Browse] to pick CWD)"}
                         : *path_buf)
                    | color(Color::Yellow) | bold,
                filler(),
                CompEl(Button(" Browse ", [path_buf] {
                    // Insert CWD via getenv("PWD") or filesystem.
                    const char* pwd = std::getenv("PWD");
                    std::string cwd;
                    if (pwd && *pwd) cwd = pwd;
                    else {
                        std::error_code ec;
                        cwd = fs::current_path(ec).string();
                    }
                    *path_buf = std::move(cwd);
                }) | color(Color::Cyan)),
            }),
            hbox({
                text(" Policy:    ") | dim,
                hbox(std::move(rb)),
            }),
            separator() | dim,
            std::move(dup_line),
            std::move(err_line),
        });

        Element footer = hbox({
            CompEl(Button(" OK ", submit) | color(Color::Green) | bold),
            text("   "),
            CompEl(Button(" Cancel ", [&open, on_cancel] {
                open = false;
                if (on_cancel) on_cancel();
            }) | dim),
            filler(),
            text(" [Enter] OK  [Esc] cancel  [Tab] cycle policy ") | dim,
        });

        return DialogFrame(" Add Workspace Directory ", Color::Green,
                           std::move(body), std::move(footer));
    }) | CatchEvent([&open, path_buf, policy_idx, submit, on_cancel](Event e) {
        if (e == Event::Escape) {
            open = false;
            if (on_cancel) on_cancel();
            return true;
        }
        if (e == Event::Return) { submit(); return true; }
        if (e == Event::Tab) {
            *policy_idx = (*policy_idx + 1) % 3;
            return true;
        }
        if (e == Event::Backspace || e.input() == "\x7f") {
            if (!path_buf->empty()) path_buf->pop_back();
            return true;
        }
        if (e.is_character()) {
            const char ch = e.character().front();
            if (std::isprint(static_cast<unsigned char>(ch))) {
                *path_buf += ch;
                return true;
            }
        }
        return false;
    });
}

// =========================================================================
// (d) BuildRemoveWorkspaceDirectoryConfirm
// =========================================================================

[[nodiscard]] inline Component BuildRemoveWorkspaceDirectoryConfirm(
    bool& open,
    const peng::WorkspaceEntry& entry,
    std::function<void()> on_yes,
    std::function<void()> on_no)
{
    using ui_tabs_detail::DialogFrame;

    auto disabled = entry.is_default;

    auto do_yes = [&open, disabled, on_yes] {
        if (disabled) return;
        open = false;
        if (on_yes) on_yes();
    };
    auto do_no = [&open, on_no] {
        open = false;
        if (on_no) on_no();
    };

    return Renderer([&open, entry, disabled, do_yes, do_no] {
        Elements body_lines;
        body_lines.push_back(paragraph(
            std::format("Remove workspace rule for {}?", entry.path.string())));
        body_lines.push_back(text(""));
        body_lines.push_back(
            text(" Actual directory WON'T be deleted.  Only the permission rule.")
            | dim);
        if (disabled) {
            body_lines.push_back(text(""));
            body_lines.push_back(
                text(" ! Default workspace cannot be removed. ")
                | color(Color::Yellow) | bold);
        }

        Element body = vbox(std::move(body_lines));
        auto yes_comp = Button(" Yes, remove rule ", do_yes)
                       | color(Color::Red) | bold;
        if (disabled) yes_comp = yes_comp | dim;
        Element footer = hbox({
            CompEl(std::move(yes_comp)),
            text("   "),
            CompEl(Button(" No, keep rule ", do_no) | color(Color::Cyan)),
            filler(),
            text(" [y] yes  [n/Esc] no ") | dim,
        });
        return DialogFrame(" Remove Workspace Rule ", Color::Red,
                           std::move(body), std::move(footer));
    }) | CatchEvent([&open, disabled, do_yes, do_no](Event e) {
        if (e == Event::Escape || e == Event::Character('n')
                               || e == Event::Character('N')) {
            do_no(); return true;
        }
        if (e == Event::Character('y') || e == Event::Character('Y')) {
            if (!disabled) do_yes();
            return true;
        }
        return false;
    });
}

// =========================================================================
// (e) BuildPermissionRuleInputForm
// =========================================================================

namespace rl_anon_1 {
struct RuleFormFieldErrors {
    std::string tool_pattern;
    std::string path_pattern;
    std::string description;
    std::string submit; // generic top-level error
};
} // namespace rl_anon_1
using namespace rl_anon_1; // unnamed

using RuleFormErrorsRef = std::shared_ptr<RuleFormFieldErrors>;

/// Decision enum for the form's radio group – slightly richer than the
/// engine's PermissionAction because we allow one-shot decisions.
enum class FormDecision {
    AllowOnce = 0,
    AlwaysAllow,
    Deny,
    AlwaysDeny,
    Abort,
};

namespace rl_anon_2 {
inline constexpr std::array<const char*, 5> kDecisionLabels = {
    "Allow once", "Always allow", "Deny", "Always deny", "Abort",
};
} // namespace rl_anon_2
using namespace rl_anon_2; // unnamed

[[nodiscard]] inline Component BuildPermissionRuleInputForm(
    RuleEntry rule,
    RuleFormErrorsRef errors,
    std::function<void(const RuleEntry&, FormDecision)> on_submit,
    std::function<void()> on_cancel)
{
    // Per-form mutable state.
    auto tool_p = std::make_shared<std::string>(rule.tool_pattern.empty()
                                                 ? std::string{"*"}
                                                 : rule.tool_pattern);
    auto path_p = std::make_shared<std::string>(rule.path_pattern.value_or(""));
    auto desc   = std::make_shared<std::string>(rule.description);
    auto dec    = std::make_shared<int>(1); // AlwaysAllow default
    auto field_cursor = std::make_shared<int>(0); // 0=tool 1=path 2=description

    std::function<void()> submit = [tool_p, path_p, desc, dec, errors, on_submit,
                   original = rule]() mutable {
        // Clear previous errors.
        errors->tool_pattern.clear();
        errors->path_pattern.clear();
        errors->description.clear();
        errors->submit.clear();

        if (tool_p->empty())
            errors->tool_pattern = "tool pattern is required";
        if (path_p->empty())
            errors->path_pattern = "path pattern is required (use ** for all)";
        if (!errors->tool_pattern.empty() || !errors->path_pattern.empty())
            return;

        RuleEntry r = original;
        r.tool_pattern = *tool_p;
        r.path_pattern = *path_p;
        r.description  = *desc;

        const auto d = static_cast<FormDecision>(*dec);
        switch (d) {
            case FormDecision::AllowOnce:
                r.action = eng::PermissionAction::AskOnce; break;
            case FormDecision::AlwaysAllow:
                r.action = eng::PermissionAction::Allow; break;
            case FormDecision::Deny:
            case FormDecision::AlwaysDeny:
                r.action = eng::PermissionAction::Deny; break;
            case FormDecision::Abort:
                r.action = eng::PermissionAction::Ask; break;
        }
        if (d == FormDecision::AlwaysDeny) r.priority = 999; // take precedence

        if (on_submit) on_submit(r, d);
    };

    using ui_tabs_detail::DecisionBadge;

    return Renderer([tool_p, path_p, desc, dec, errors, submit, on_cancel] {
        // Live preview of the rule JSON-like snippet.
        const auto d = static_cast<FormDecision>(*dec);
        const char* decision_s = kDecisionLabels[static_cast<std::size_t>(*dec)];
        const Color dec_color =
            (d == FormDecision::AllowOnce || d == FormDecision::AlwaysAllow)
                ? Color::Green
            : (d == FormDecision::Deny || d == FormDecision::AlwaysDeny)
                ? Color::Red
                : Color::Yellow;
        Element preview = vbox({
            hbox({
                text(" Preview: ") | dim,
                text("{") | dim,
            }),
            text(std::format("   tool:   \"{}\"", *tool_p))
                | color(Color::CyanLight) | dim,
            text(std::format("   path:   \"{}\"",
                             path_p->empty() ? std::string{"**"} : *path_p))
                | color(Color::CyanLight) | dim,
            text(std::format("   action: {}", decision_s))
                | color(dec_color) | dim,
            text(std::format("   desc:   \"{}\"",
                             desc->empty() ? std::string{""} : *desc))
                | color(Color::GrayLight) | dim,
            text("}") | dim,
        }) | bgcolor(Color::RGB(18, 18, 22)) | borderLight;

        // Per-field error lines (red inline).
        auto err_tool = errors->tool_pattern.empty()
            ? text("")
            : text(" ! " + errors->tool_pattern) | color(Color::Red) | bold;
        auto err_path = errors->path_pattern.empty()
            ? text("")
            : text(" ! " + errors->path_pattern) | color(Color::Red) | bold;
        auto err_sub  = errors->submit.empty()
            ? text("")
            : text(" ! " + errors->submit) | color(Color::Red) | bold;

        // Decision radio group visual.
        Elements rb;
        for (std::size_t i = 0; i < kDecisionLabels.size(); ++i) {
            const bool sel = (*dec == static_cast<int>(i));
            Color c =
                (i == 0 || i == 1) ? Color::Green :
                (i == 2 || i == 3) ? Color::Red   : Color::Yellow;
            rb.push_back(text(std::format(" {} ", kDecisionLabels[i]))
                | (sel ? (color(c) | inverted | bold) : dim));
            if (i + 1 < kDecisionLabels.size()) rb.push_back(text("  "));
        }

        Elements form_lines = {
            hbox({
                text(" Create Rule ") | bold | color(Color::Green),
                filler(),
                text(" Fill in the fields, then [Submit]. ") | dim,
            }),
            pc::ThinDivider(),

            // Tool pattern
            hbox({
                text(" Tool pattern: ") | dim | size(WIDTH, EQUAL, 18),
                text(tool_p->empty()
                         ? std::string{"(e.g. bash, file_*)"}
                         : *tool_p)
                    | color(Color::Yellow) | bold,
            }),
            std::move(err_tool),

            separator() | dim,
            // Path pattern
            hbox({
                text(" Path pattern: ") | dim | size(WIDTH, EQUAL, 18),
                text(path_p->empty()
                         ? std::string{"(e.g. **/*.cpp)"}
                         : *path_p)
                    | color(Color::Yellow) | bold,
            }),
            std::move(err_path),

            separator() | dim,
            // Decision radiobox
            hbox({
                text(" Decision:     ") | dim | size(WIDTH, EQUAL, 18),
                hbox(std::move(rb)),
            }),

            separator() | dim,
            // Description (multi-line-ish single input)
            hbox({
                text(" Description:  ") | dim | size(WIDTH, EQUAL, 18),
                text(desc->empty()
                         ? std::string{"(optional human-readable note)"}
                         : *desc)
                    | color(Color::GrayLight),
            }),

            separator() | dim,
            std::move(preview),

            separator() | dim,
            std::move(err_sub),
            hbox({
                CompEl(Button(" Submit ", submit) | color(Color::Green) | bold),
                text("   "),
                CompEl(Button(" Cancel ", [on_cancel] {
                    if (on_cancel) on_cancel();
                }) | dim),
                filler(),
                text(" [Tab] cycle decision  [Enter] submit  [Esc] cancel ") | dim,
            }),
        };

        return vbox(std::move(form_lines)) | yflex_grow | yframe;
    }) | CatchEvent([tool_p, path_p, desc, dec, submit, on_cancel, field_cursor](Event e) {
        // field_cursor tracks which field we're typing into: 0=tool 1=path 2=description
        // Tab cycles the cursor; if already at 2, Tab additionally cycles
        // the decision radiobox.
        if (e == Event::Escape) {
            if (on_cancel) on_cancel();
            *field_cursor = 0;
            return true;
        }
        if (e == Event::Return) { submit(); return true; }
        if (e == Event::Tab) {
            *field_cursor = (*field_cursor + 1) % 3;
            if (*field_cursor == 2) {
                *dec = (*dec + 1) % static_cast<int>(kDecisionLabels.size());
            }
            return true;
        }
        if (e == Event::Backspace || e.input() == "\x7f") {
            switch (*field_cursor) {
                case 0: if (!tool_p->empty()) tool_p->pop_back(); break;
                case 1: if (!path_p->empty()) path_p->pop_back(); break;
                case 2: if (!desc->empty())   desc->pop_back();   break;
            }
            return true;
        }
        if (e.is_character()) {
            const char ch = e.character().front();
            if (std::isprint(static_cast<unsigned char>(ch))) {
                switch (*field_cursor) {
                    case 0: *tool_p += ch; break;
                    case 1: *path_p += ch; break;
                    case 2: *desc   += ch; break;
                }
                return true;
            }
        }
        return false;
    });
}

// =========================================================================
// (f) BuildPermissionRuleDescriptionCard
// =========================================================================

[[nodiscard]] inline Component BuildPermissionRuleDescriptionCard(
    const RuleEntry& rule,
    std::function<void()> on_edit,
    std::function<void()> on_delete,
    std::function<void()> on_duplicate)
{
    using ui_tabs_detail::TimeAgo;
    using ui_tabs_detail::DecisionBadge;

    auto rule_copy = std::make_shared<RuleEntry>(rule);

    // Derive timestamps as strings.  RuleEntry doesn't ship native times, so
    // we render "id suffix" as a pseudo-created-at for display.
    std::string created_str = rule.id.empty() ? std::string{"—"} : rule.id;
    if (created_str.size() > 10)
        created_str = created_str.substr(0, 10) + "…";

    // RuleEntry has no last-used counter; approximate via enabled_count.
    const std::string last_used = rule.enabled_count
        ? std::format("used {} time(s)", rule.enabled_count)
        : std::string{"not used yet"};

    return Renderer([rule_copy, on_edit, on_delete, on_duplicate,
                     created_str, last_used] {
        const RuleEntry& r = *rule_copy;

        Elements lines = {
            hbox({
                text(" Rule: ") | bold | color(Color::Cyan),
                DecisionBadge(r.action),
                filler(),
                text(" id: ") | dim,
                text(std::format(" {}", created_str)) | dim,
            }),
            pc::ThinDivider(),
            // Monospace tool/path patterns
            hbox({
                text(" tool: ") | dim | size(WIDTH, EQUAL, 10),
                text(std::format(" {} ", r.tool_pattern))
                    | color(Color::MagentaLight)
                    | bgcolor(Color::RGB(18, 18, 22)),
            }),
            hbox({
                text(" path: ") | dim | size(WIDTH, EQUAL, 10),
                text(std::format(" {} ",
                                 r.path_pattern.value_or("(none)")))
                    | color(Color::CyanLight)
                    | bgcolor(Color::RGB(18, 18, 22)),
            }),
            separator() | dim,
            // Description block
            hbox({
                text(" desc: ") | dim | size(WIDTH, EQUAL, 10),
                paragraph(r.description.empty()
                              ? std::string{"(no description)"}
                              : r.description),
            }),
            separator() | dim,
            // Metadata row
            hbox({
                text(" created: ") | dim,
                text(created_str) | dim,
                text(" | ") | dim,
                text(last_used) | dim,
                filler(),
                text(std::format(" scope={} strategy={} pri={} ",
                    detail::ScopeName(r.scope),
                    detail::StrategyName(r.strategy),
                    r.priority)) | dim,
            }),
            pc::ThinDivider(),
            // Action buttons
            hbox({
                CompEl(Button(" Edit ", [on_edit] { if (on_edit) on_edit(); })
                    | color(Color::Cyan)),
                text(" "),
                CompEl(Button(" Delete ", [on_delete] { if (on_delete) on_delete(); })
                    | color(Color::Red)),
                text(" "),
                CompEl(Button(" Duplicate ",
                    [on_duplicate] { if (on_duplicate) on_duplicate(); })
                    | color(Color::Green)),
                filler(),
                r.enabled
                    ? text(" ENABLED ") | color(Color::Green) | dim
                    : text(" DISABLED ") | color(Color::Red) | dim,
            }),
        };
        return window(
            text(" Rule Detail ") | bold | color(Color::Cyan),
            vbox(std::move(lines)) | xflex_grow | size(WIDTH, GREATER_THAN, 60)
        ) | color(Color::Cyan);
    });
}

// =========================================================================
// (g) BuildPermissionsTabs – the single entry-point router
// =========================================================================

[[nodiscard]] inline Component BuildPermissionsTabs(
    PermissionsTabsState state,
    std::shared_ptr<PermTab> active_tab)
{
    // --- Build each of the 4 tab contents first (so we can switch between
    //     them without reconstructing state).

    // Tab 0: "All Rules" – use the existing MakePermissionRuleList factory.
    RuleListInput rl_input;
    if (state.model.rule_list.has_value())
        rl_input = std::move(*state.model.rule_list);
    RuleListCallbacks rl_cbs;
    rl_cbs.on_add    = state.callbacks.on_add_rule;
    rl_cbs.on_update = state.callbacks.on_update_rule;
    rl_cbs.on_delete = state.callbacks.on_delete_rule;
    auto tab_all_rules = MakePermissionRuleList(std::move(rl_input),
                                                std::move(rl_cbs));

    // Tab 1: "Recent Denials"
    RecentDenialsState denials_state;
    if (state.model.denials.has_value())
        denials_state = *state.model.denials;
    auto tab_denials = BuildRecentDenialsTab(
        std::move(denials_state),
        state.callbacks.on_allow_once,
        state.callbacks.on_always_allow);

    // Tab 2: "Workspaces"
    WorkspaceState ws_state;
    if (state.model.workspaces.has_value())
        ws_state = *state.model.workspaces;
    auto tab_workspaces = BuildWorkspaceTab(
        std::move(ws_state),
        [cbs = state.callbacks](const fs::path& p, peng::WorkspaceEntry::Policy) {
            if (cbs.on_add_workspace) cbs.on_add_workspace(p);
        },
        state.callbacks.on_remove_workspace);

    // Tab 3: "Create Rule" – form then submit fires on_add_rule callback.
    RuleEntry blank;
    blank.id    = std::format("rule_{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    blank.strategy = MatchStrategy::Glob;
    blank.action   = PermissionAction::Allow;
    blank.scope    = PermissionScope::Session;
    blank.enabled  = true;
    blank.priority = 50;
    blank.group_id = "g6"; // "Custom A"
    blank.tool_pattern = "*";

    auto errors = std::make_shared<RuleFormFieldErrors>();
    auto tab_create_rule = BuildPermissionRuleInputForm(
        std::move(blank), errors,
        [on_add = state.callbacks.on_add_rule, active_tab](
            const RuleEntry& r, FormDecision) {
            if (on_add) on_add(r);
            // Switch back to "All Rules" after successful creation so the
            // user sees the new rule in the list.
            *active_tab = PermTab::AllRules;
        },
        [active_tab] { *active_tab = PermTab::AllRules; });

    // Build a container that dispatches render() based on active_tab.
    // We use CatchEvent to route keystrokes to the active sub-tab via the
    // inner component's OnEvent handler.  This mirrors the FTXUI
    // Container::Tab idiom.
    return Renderer([active_tab, tab_all_rules, tab_denials,
                     tab_workspaces, tab_create_rule] {
        switch (*active_tab) {
            case PermTab::AllRules:   return tab_all_rules->Render();
            case PermTab::Denials:    return tab_denials->Render();
            case PermTab::Workspaces: return tab_workspaces->Render();
            case PermTab::CreateRule: return tab_create_rule->Render();
        }
        return tab_all_rules->Render();
    }) | CatchEvent([active_tab, tab_all_rules, tab_denials,
                      tab_workspaces, tab_create_rule](Event e) {
        // Route events to the active sub-component first so inner controls
        // (arrow keys, typing into the form, etc.) still work.
        switch (*active_tab) {
            case PermTab::AllRules:
                if (tab_all_rules && tab_all_rules->OnEvent(e)) return true;
                break;
            case PermTab::Denials:
                if (tab_denials && tab_denials->OnEvent(e)) return true;
                break;
            case PermTab::Workspaces:
                if (tab_workspaces && tab_workspaces->OnEvent(e)) return true;
                break;
            case PermTab::CreateRule:
                if (tab_create_rule && tab_create_rule->OnEvent(e)) return true;
                break;
        }
        return false;
    });
}

} // namespace cc::ui::permissions::rule_list

// Expose namespace alias so callers can spell
// `peng::recent_denials()` against either the import module's namespace OR
// the namespace declared above.
namespace cc::utils::permissions {
    namespace engine_engine_alias = cc::utils::permissions_engine;
}

