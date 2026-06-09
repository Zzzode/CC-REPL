/// =========================================================================
/// @file messages_interactions.cppm
/// @brief Message multi-select FSM + right-click context actions menu.
///
/// PHASE 4B P1 - Merges two TS-side components into one reusable C++20 module:
///   (1) src/components/MessageSelector.tsx  – message multi-select toolbar
///         Normal / From-Start / To-End strategies · Shift-range · visual-v
///         · Ctrl+click append · Esc-2stage · toolbar (copy/export-3fmt/tag/
///         bookmark/LLM-summary/delete-10-Trusted/regenerate-from) · N-K-M
///         status bar with green cost-pill
///   (2) src/components/messageActions.tsx  – per-message context menu
///         Enter/m key · mouse right-click · ⋮ always-on for long messages ·
///         10 items (copy/regen/model-switch/edit/reply/tag/pin/share/
///         delete/redact) + 3 submenus · keyboard j/k/Enter/Esc/1-9/h/l
///
/// ZERO ENGINE I/O - every real operation dispatches through callbacks.
/// Bulk-delete risk classification routes through cc.ui.trust_utils so UI8
/// (TrustDialog) can own the confirmation UX.
///
/// Integration surface (for UI21 messages_list):
///   * import cc.ui.messages.messages_interactions;
///   * keep a `SelectionFSM` and `std::vector<MessageId> selection_state`
///   * wrap list's CatchEvent with HandleSelectionEvent()
///   * stack RenderSelectionToolbar() below the list
///   * attach MessageActionsContextMenu to rows (event interceptor + modal)
/// =========================================================================
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.messages.messages_interactions;

// --- Shared metadata + dispatch helpers -----------------------------------
// NOTE: UI21 messages_list should later expose a `MessageRowPayload` that
// *contains* a `MessageMetadata`.  We define the metadata struct here so the
// interaction primitives have a stable ABI to code against today.

// Toolbar + menu share these cross-cutting imports:
import cc.ui.custom_select;     // SelectMode / SelectOption for export-format
import cc.ui.design.tokens;     // palette + spacing + radius
import cc.ui.trust_utils;       // RiskLevel for bulk delete

export namespace cc::ui::messages::interactions {

using namespace ftxui;
using cc::ui::design::tokens::Radius;
using cc::ui::trust_utils::RiskLevel;

// =========================================================================
// 0. Shared data types  (UI21 future MessageRowPayload members)
// =========================================================================

/// Opaque message identifier.  Mirrors TS `UUID` (string uuid today).
struct MessageId {
    std::string value;
    bool operator==(const MessageId& o) const noexcept { return value == o.value; }
    bool operator<(const MessageId& o)  const noexcept { return value < o.value; }
};

/// Author / shape of the row — used by context-menu to enable/disable items.
enum class MessageRole : std::uint8_t {
    User,
    Assistant,
    System,
    ToolUse,
    ToolResult,
    GroupedTools,
    Attachment,
};

/// A user-facing label tag.  Tags can be assigned to arbitrary rows; the
/// selection toolbar exposes a tag-chip multi-picker + "new tag" input.
struct MessageTag {
    std::string id;
    std::string label;
    std::optional<Color> color;   // chip color override
};

/// Row-level metadata used by both the selection FSM and context menu.
///
/// NOTE: UI21's MessageRowPayload should, in a follow-up commit, embed this
/// struct.  For now callers copy the relevant fields from their payload into
/// the view-model handed to the rendering helpers below.
struct MessageMetadata {
    MessageId id;
    MessageRole role = MessageRole::User;
    std::size_t line_count = 0;          // rendered line height (>20 shows ⋮)
    std::size_t char_count = 0;          // for status bar
    std::size_t est_tokens = 0;          // status bar / cost pill: chars / 4
    bool bookmarked = false;
    bool pinned = false;
    bool redacted = false;               // redacted-thinking mask
    std::vector<MessageTag> tags;        // existing chips
    std::string model_name;              // assistant rows only
    std::string role_label;              // e.g. "claude-3.7-sonnet" override
};

// =========================================================================
// 1.  Selection FSM + toolbar
// =========================================================================

/// Three multi-selection strategies, matching the TS toolbar modes.
enum class SelectionMode : std::uint8_t {
    Normal,      // click = toggle single, Shift-click = extend range (default)
    FromStart,   // click => select rows [0 .. clicked]
    ToEnd,       // click => select rows [clicked .. total-1]
};

/// Finite state machine that lives alongside UI21's row-index cursor.
///
/// State transitions are pure-ish (modelled as a function over the FSM +
/// selection vector).  Keeping the state machine external to the component
/// makes it directly testable (no ftxui Component harness required).
struct SelectionFSM {
    SelectionMode mode = SelectionMode::Normal;
    bool active = false;                 // in multi-select mode at all?
    std::optional<std::size_t> anchor;   // last single-click for Shift-range
    std::optional<std::size_t> last_visual; // v-mode cursor (j/k extends)
    bool esc_once = false;               // true after first Esc (cleared sel)
};

// -- helpers ---------------------------------------------------------------

namespace detail {
inline std::set<std::size_t> sorted_index_set(const std::vector<MessageId>& sel,
                                              const std::vector<MessageId>& order) {
    std::map<MessageId, std::size_t> idx_of;
    for (std::size_t i = 0; i < order.size(); ++i) idx_of[order[i]] = i;

    std::set<std::size_t> out;
    for (const auto& id : sel) {
        auto it = idx_of.find(id);
        if (it != idx_of.end()) out.insert(it->second);
    }
    return out;
}

inline void toggle_id(std::vector<MessageId>& sel, const MessageId& id) {
    auto it = std::find(sel.begin(), sel.end(), id);
    if (it == sel.end()) sel.push_back(id);
    else                 sel.erase(it);
}

inline void add_range(std::vector<MessageId>& sel, const std::vector<MessageId>& order,
                      std::size_t lo, std::size_t hi) {
    if (hi >= order.size()) hi = order.size() - 1;
    if (lo > hi) std::swap(lo, hi);
    for (std::size_t i = lo; i <= hi; ++i) {
        if (std::find(sel.begin(), sel.end(), order[i]) == sel.end())
            sel.push_back(order[i]);
    }
}

inline void remove_range(std::vector<MessageId>& sel, const std::vector<MessageId>& order,
                         std::size_t lo, std::size_t hi) {
    if (hi >= order.size()) hi = order.size() - 1;
    if (lo > hi) std::swap(lo, hi);
    std::set<MessageId> to_remove;
    for (std::size_t i = lo; i <= hi; ++i) to_remove.insert(order[i]);
    sel.erase(std::remove_if(sel.begin(), sel.end(),
                             [&](const MessageId& m) { return to_remove.count(m); }),
              sel.end());
}
} // namespace detail

// -- exported FSM entry point ---------------------------------------------

/// Handle one FTXUI event against a selection FSM.
///
///   `sel`       – in/out set of selected MessageIds (preserves insertion order)
///   `fsm`       – in/out selection state machine
///   `row_order` – id at each display row index (0 = top).  Caller owns.
///   `cursor_row`- the current cursor line (j/k focus) used for v-mode / `u`
///                 etc.  Pass nullopt when the caller doesn't track a cursor.
///   `mouse_row` – set when the event came from a click on a known row.
///
/// Returns true if the event was consumed (caller should not propagate).
bool HandleSelectionEvent(Event& ev,
                                 std::vector<MessageId>& sel,
                                 SelectionFSM& fsm,
                                 const std::vector<MessageId>& row_order,
                                 std::optional<std::size_t> cursor_row,
                                 std::optional<std::size_t> mouse_row = std::nullopt) {
    const std::size_t N = row_order.size();
    if (N == 0) return false;
    const std::size_t cur = cursor_row.value_or(0);

    // ---- enter multi-select mode ----------------------------------------
    if (!fsm.active) {
        if (ev == Event::Character('v')) {
            fsm.active = true;
            fsm.mode = SelectionMode::Normal;
            fsm.anchor = cur;
            fsm.last_visual = cur;
            // select the current row (Vim-like)
            sel.clear();
            sel.push_back(row_order[cur]);
            return true;
        }
        if (ev == Event::Special({17, '\0'}) /* Ctrl+Q unused; keep path */) {
            return false; // reserved
        }
        // Toolbar "# select" button fires a synthetic Character('\x16')? We
        // don't mandate a shortcut for the button; callers set fsm.active=true.
        if (ev.mouse().button == Mouse::Left && mouse_row.has_value()
            && (ev.mouse().motion == Mouse::Pressed
                ? ev.mouse().control
                : false)) {
            // Ctrl+click => enter multi-select and toggle that row
            fsm.active = true;
            fsm.anchor = *mouse_row;
            detail::toggle_id(sel, row_order[*mouse_row]);
            return true;
        }
        return false;
    }

    // ---- 2-stage Esc -----------------------------------------------------
    if (ev == Event::Escape) {
        if (!fsm.esc_once) {
            sel.clear();
            fsm.esc_once = true;
            fsm.anchor.reset();
            fsm.last_visual.reset();
            return true;
        }
        fsm.active = false;
        fsm.esc_once = false;
        fsm.mode = SelectionMode::Normal;
        return true;
    }

    // ---- strategy toggles (toolbar → synthetic) -------------------------
    // Expose as a dedicated character so the toolbar button can synthesize:
    //   Event::Character('1') => Normal
    //   Event::Character('2') => From-Start
    //   Event::Character('3') => To-End
    if (ev == Event::Character('1')) { fsm.mode = SelectionMode::Normal;    return true; }
    if (ev == Event::Character('2')) { fsm.mode = SelectionMode::FromStart; return true; }
    if (ev == Event::Character('3')) { fsm.mode = SelectionMode::ToEnd;     return true; }

    // ---- Select all / clear ---------------------------------------------
    // Ctrl+A = ASCII 0x01
    if (ev == Event::Special({1})) {
        sel.clear();
        sel.reserve(N);
        for (std::size_t i = 0; i < N; ++i) sel.push_back(row_order[i]);
        fsm.esc_once = false;
        return true;
    }
    if (ev == Event::Character('u')) {   // unselect all, stay in mode
        sel.clear();
        fsm.esc_once = false;
        return true;
    }

    // ---- mouse clicks ---------------------------------------------------
    if (ev.mouse().button == Mouse::Left
        && ev.mouse().motion == Mouse::Pressed
        && mouse_row.has_value()) {
        const auto row = *mouse_row;
        const bool shift = ev.mouse().shift;
        const bool ctrl  = ev.mouse().control;

        switch (fsm.mode) {
        case SelectionMode::Normal: {
            if (shift && fsm.anchor.has_value()) {
                // reset to the anchor set, then extend
                std::vector<MessageId> base;
                // preserve only the anchor if it was originally selected
                detail::add_range(base, row_order,
                                  std::min(*fsm.anchor, row),
                                  std::max(*fsm.anchor, row));
                sel = std::move(base);
            } else if (ctrl) {
                detail::toggle_id(sel, row_order[row]);
                fsm.anchor = row;
            } else {
                sel.clear();
                sel.push_back(row_order[row]);
                fsm.anchor = row;
            }
            break;
        }
        case SelectionMode::FromStart:
            sel.clear();
            detail::add_range(sel, row_order, 0, row);
            break;
        case SelectionMode::ToEnd:
            sel.clear();
            detail::add_range(sel, row_order, row, N - 1);
            break;
        }
        fsm.last_visual = row;
        fsm.esc_once = false;
        return true;
    }

    // ---- Vim visual-mode j/k range extend ------------------------------
    if (ev == Event::Character('j')) {
        const auto base = fsm.last_visual.value_or(fsm.anchor.value_or(cur));
        const auto next = std::min(N - 1, base + 1);
        if (fsm.anchor.has_value()) {
            sel.clear();
            detail::add_range(sel, row_order, *fsm.anchor, next);
        } else {
            detail::toggle_id(sel, row_order[next]);
        }
        fsm.last_visual = next;
        fsm.esc_once = false;
        return true;
    }
    if (ev == Event::Character('k')) {
        const auto base = fsm.last_visual.value_or(fsm.anchor.value_or(cur));
        const auto prev = (base == 0) ? 0 : base - 1;
        if (fsm.anchor.has_value()) {
            sel.clear();
            detail::add_range(sel, row_order, *fsm.anchor, prev);
        } else {
            detail::toggle_id(sel, row_order[prev]);
        }
        fsm.last_visual = prev;
        fsm.esc_once = false;
        return true;
    }

    return false;
}

// =========================================================================
// 1b.  Aggregate stats + cost pill
// =========================================================================

/// Aggregate selection statistics.
struct SelectionStats {
    std::size_t count = 0;
    std::size_t characters = 0;
    std::size_t est_tokens = 0;
    double est_cost_usd = 0.0;     // UI3 Usage: $0.003 / 1K tokens
    bool truncated_copy = false;   // combined >64K

    /// USD string, 4 digits precision, e.g. "$0.0012"
    [[nodiscard]] std::string cost_str() const {
        std::ostringstream oss;
        oss.precision(4);
        oss << std::fixed << '$' << est_cost_usd;
        return oss.str();
    }
};

/// Compute stats over the selection.  Exported so the caller can feed them
/// into the LLM-summarize callback too.
SelectionStats ComputeSelectionStats(const std::vector<MessageId>& sel,
                                            const std::vector<MessageMetadata>& meta_in_order) {
    constexpr std::size_t kCopyLimit = 64 * 1024;
    SelectionStats s;
    s.count = sel.size();
    std::map<MessageId, const MessageMetadata*> by_id;
    for (const auto& m : meta_in_order) by_id[m.id] = &m;
    for (const auto& id : sel) {
        auto it = by_id.find(id);
        if (it == by_id.end()) continue;
        s.characters += it->second->char_count;
        s.est_tokens += it->second->est_tokens;
    }
    s.truncated_copy = s.characters > kCopyLimit;
    s.est_cost_usd = static_cast<double>(s.est_tokens) * 0.003 / 1000.0;
    return s;
}

// =========================================================================
// 1c.  Callbacks the toolbar needs (no engine I/O inside the module)
// =========================================================================

enum class ExportFormat : std::uint8_t { Jsonl, Markdown, Text };
enum class SummarizeState : std::uint8_t { Idle, Running, Done, Failed };

struct SelectionCallbacks {
    /// Emitted when user hits 'c' or clicks copy-text button.  The combined
    /// string is already truncated to 64K when necessary.
    std::function<void(std::string combined_text)> on_copy;

    /// 'e' — export.  Caller writes the file (format + IDs).
    std::function<void(ExportFormat fmt, std::vector<MessageId> ids)> on_export;

    /// 't' — assign/replace tag chips on every selected row.
    std::function<void(std::vector<MessageId> ids, std::vector<MessageTag> tags)> on_tag;

    /// 'b' — toggle bookmark on every selected row.
    std::function<void(std::vector<MessageId> ids)> on_bookmark_toggle;

    /// 's' — trigger the LLM.  `on_start` kicks off the async flow; the
    /// caller must eventually call `set_summary_state(Running/Done/Failed)`.
    std::function<void(std::vector<MessageId> ids,
                       void(*set_summary_state)(void* ctx, SummarizeState),
                       void* ctx)> on_summarize_start;

    /// 'd' — delete selected rows.  RiskLevel depends on the count.
    /// Caller must show UI8 TrustDialog when the level >= Medium.
    std::function<void(std::vector<MessageId> ids, RiskLevel risk)> on_delete;

    /// 'r' — regenerate from the first selected row downwards.  Callback
    /// receives the anchor MessageId; the "irrevocable" warning lives in UI.
    std::function<void(MessageId anchor)> on_regenerate_from;

    /// 'a' / toolbar button — enter selection mode (same as toolbar).
    std::function<void()> on_enter_mode;
};

/// Compute the trust tier for a bulk delete.  Lives here so both the
/// toolbar handler and future bulk operations agree on thresholds.
RiskLevel DeleteRiskLevel(std::size_t count) {
    if (count == 0)                return RiskLevel::Low;
    if (count <= 3)                return RiskLevel::Low;
    if (count <= 10)               return RiskLevel::Medium;
    if (count <= 100)              return RiskLevel::High;
    return RiskLevel::Critical;
}

// =========================================================================
// 1d.  Rendering  (pure Element output, no mutable captures)
// =========================================================================

/// Format the mode indicator chip shown at the left of the toolbar.
[[nodiscard]] inline Element ModeChip(SelectionMode mode, bool active) {
    const char* label = "Normal";
    Color col = Color::GrayDark;
    if (active) {
        switch (mode) {
        case SelectionMode::Normal:    label = "Mode:Normal";    col = Color::SteelBlue; break;
        case SelectionMode::FromStart: label = "Mode:From-Start";col = Color::MediumPurple1; break;
        case SelectionMode::ToEnd:     label = "Mode:To-End";    col = Color::Orange1; break;
        }
    }
    return text(std::format(" [{}] ", label)) | color(col) | bold |
           borderEmpty | size(WIDTH, EQUAL, 18);
}

/// The pill-shaped cost indicator (green on Low, yellow rising with cost).
[[nodiscard]] inline Element CostPill(const SelectionStats& s) {
    Color col = Color::Green;
    if (s.est_cost_usd >= 0.05)       col = Color::Yellow;
    if (s.est_cost_usd >= 0.50)       col = Color::Orange1;
    if (s.est_cost_usd == 0.0)        col = Color::GrayDark;
    // Round to 4 digits; always show so user learns the scale is "per 1K".
    return hbox({ text(" ≈ "), text(s.cost_str()) | color(col) | bold, text(" est ") })
         | borderRounded | color(col);
}

/// Status line:  "N messages selected · K characters · M tokens (est.)"
[[nodiscard]] inline Element RenderSelectionStatus(const SelectionStats& s) {
    Elements row;
    row.push_back(text(std::format("{} message{} selected", s.count, s.count == 1 ? "" : "s"))
                  | bold | color(Color::SkyBlue1));
    row.push_back(text(" · "));
    row.push_back(text(std::format("{} chars", s.characters)) | dim);
    row.push_back(text(" · "));
    row.push_back(text(std::format("{} tokens (est.)", s.est_tokens)) | dim);
    if (s.truncated_copy) {
        row.push_back(text(" · "));
        row.push_back(text("copy truncated (max 64K)") | color(Color::Yellow) | dim);
    }
    return hbox(std::move(row));
}

/// The full Tab-style toolbar row: one button per operation.
///
/// Visual style mirrors the TS side: icon glyph | shortcut | label.  Disabled
/// items render dimmed when the selection is empty.
[[nodiscard]] inline Element RenderSelectionToolbar(const std::vector<MessageId>& sel,
                                                    const SelectionFSM& fsm,
                                                    const SelectionStats& s,
                                                    SummarizeState summary = SummarizeState::Idle) {
    const bool has = !sel.empty();

    auto btn = [has](std::string_view icon, char key, std::string_view label,
                     bool extra_disabled = false) {
        const bool on = has && !extra_disabled;
        Elements parts;
        parts.push_back(text(std::format("{} ", icon)) | color(on ? Color::Cyan : Color::GrayDark));
        parts.push_back(text(std::format("[{}]", key)) | bold
                       | color(on ? Color::White : Color::GrayDark));
        parts.push_back(text(std::format(" {} ", label))
                       | (on ? nothing : dim));
        return hbox(std::move(parts)) | (on ? nothing : strikethrough);
    };

    // (layout row)
    Elements ops;
    ops.push_back(ModeChip(fsm.mode, fsm.active));
    ops.push_back(separator());
    ops.push_back(btn("📋", 'c', "Copy"));
    ops.push_back(separator());
    ops.push_back(btn("🗂",  'e', "Export"));
    ops.push_back(separator());
    ops.push_back(btn("🏷️", 't', "Tag"));
    ops.push_back(separator());
    ops.push_back(btn("⭐", 'b', sel.size() == 1 ? "Toggle★" : "Bookmark"));
    ops.push_back(separator());

    if (summary == SummarizeState::Idle) {
        ops.push_back(btn("✨", 's', "Summarize"));
    } else if (summary == SummarizeState::Running) {
        Elements sp{text("⠋ "), text(" Summarising…") | color(Color::SteelBlue)};
        ops.push_back(hbox(std::move(sp)));
    } else if (summary == SummarizeState::Done) {
        ops.push_back(text(" ✅ summarised ") | color(Color::Green) | dim);
    } else {
        ops.push_back(text(" ⚠ summarise failed ") | color(Color::Red) | dim);
    }
    ops.push_back(separator());

    const RiskLevel dl = DeleteRiskLevel(sel.size());
    const bool del_hot = dl >= RiskLevel::Medium;
    auto del = btn("🗑", 'd', "Delete", false);
    if (del_hot) {
        del = del | color(Color::RedLight);
    }
    ops.push_back(del);
    ops.push_back(separator());

    ops.push_back(btn("↩", 'r', "Regen-from"));
    ops.push_back(separator());
    ops.push_back(hbox({
        text(" [Ctrl+A]") | bold | color(Color::CadetBlue),
        text(" Select-all ") | (has ? nothing : dim),
    }));
    ops.push_back(hbox({
        text(" [u]") | bold | color(Color::CadetBlue),
        text(" Clear ") | (has ? nothing : dim),
    }));

    // ---- footer: status + cost pill -------------------------------------
    auto cost = CostPill(s);
    auto status = RenderSelectionStatus(s);
    return window(text("Messages"),
                  vbox({
                      hflow(std::move(ops)),
                      separator(),
                      hbox({ status, filler(), std::move(cost) }),
                  }) | size(HEIGHT, GREATER_THAN, 4));
}

// =========================================================================
// 2.  Context menu (right-click / ⋮ / Enter / 'm')
// =========================================================================

/// The 10 menu item kinds, in the same order as the TS spec.
enum class MenuItemKind : std::uint8_t {
    Copy,           // submenu: plain text / JSON
    Regenerate,
    RegenModel,     // submenu: 5 common models
    Edit,
    ReplyQuote,
    Tag,            // submenu: tag chips + "new tag"
    Pin,
    ShareExport,
    Delete,
    Redact,         // admin-only toggle
};

struct MenuItem {
    MenuItemKind kind;
    std::string label;
    char shortcut_digit = '\0';    // 1-9 or 0
    std::optional<std::string> icon;
    bool enabled = true;
};

enum class CopyFormat : std::uint8_t { Text, Json };
enum class ShareFormat : std::uint8_t { JsonBlob, Link };

/// Everything the menu dispatches through — all engine-bound operations
/// flow through the caller, never through the module.
struct MessageActionsCallbacks {
    std::function<void(MessageId id, CopyFormat fmt)>         on_copy;
    std::function<void(MessageId id)>                          on_regenerate;
    std::function<void(MessageId id, std::string model)>       on_regen_model;
    std::function<void(MessageId id, std::string new_text)>    on_edit_commit;
    std::function<void(MessageId id, std::string quoted)>      on_reply_quote;
    std::function<void(MessageId id, std::vector<MessageTag>)> on_tag;
    std::function<void(MessageId id)>                          on_pin_toggle;   // returns ok/fail via bool on callback? keep void; let caller hint.
    std::function<void(MessageId id, ShareFormat)>             on_share;
    std::function<void(MessageId id, RiskLevel)>               on_delete;
    std::function<void(MessageId id)>                          on_redact_toggle;
    std::function<void(MessageId id, size_t row_visual_index)> on_pick_row_for_menu; // when menu opens
    std::function<bool(MessageId id)>                           is_admin_redact_allowed;
};

/// Submenu identifier — tracks which submenu (if any) is expanded.
enum class SubmenuOpen : std::uint8_t {
    None,
    Copy,       // Text / JSON
    RegenModel, // 5 models
    Tag,        // tag chips + new
};

/// State for the context menu (10 items + 3 submenus).
///
/// Kept as a plain struct so callers can trivially snapshot/restore it when
/// transitioning between rows.
struct MessageActionsMenuState {
    bool open = false;
    std::size_t selected_index = 0;   // into `detail::kTopLevelItems`
    SubmenuOpen submenu = SubmenuOpen::None;
    std::size_t sub_index = 0;
    std::optional<MessageId> target;  // row the menu was opened for
    std::size_t row_visual = 0;       // used for the ⋮ alignment hint

    // inline-edit state  (Edit menu item → inline TextInput substitute)
    bool editing = false;
    std::string edit_buffer;
};

// ---- static top-level menu ------------------------------------------------
namespace detail {
inline const std::array<MenuItem, 10> kTopLevelItems = {{
    { MenuItemKind::Copy,        "📋 Copy message",         '1', "📋" },
    { MenuItemKind::Regenerate,  "🔄 Regenerate",           '2', "🔄" },
    { MenuItemKind::RegenModel,  "🪄 Re-gen with model…",   '3', "🪄" },
    { MenuItemKind::Edit,        "✏️ Edit",                 '4', "✏️" },
    { MenuItemKind::ReplyQuote,  "↩ Reply / Quote",         '5', "↩" },
    { MenuItemKind::Tag,         "🏷️ Tag…",                 '6', "🏷" },
    { MenuItemKind::Pin,         "📎 Pin message",          '7', "📎" },
    { MenuItemKind::ShareExport, "📤 Share / Export",       '8', "📤" },
    { MenuItemKind::Delete,      "🗑 Delete message",        '9', "🗑" },
    { MenuItemKind::Redact,      "🔒 Redact thinking",      '0', "🔒" },
}};

inline const std::array<std::pair<std::string, CopyFormat>, 2> kCopyItems = {{
    { "Plain text",  CopyFormat::Text },
    { "JSON",        CopyFormat::Json },
}};

inline const std::array<const char*, 5> kPopularModels = {{
    "claude-3.7-sonnet",
    "claude-3.5-haiku",
    "claude-opus-4",
    "claude-3-opus",
    "gpt-5",
}};
} // namespace detail

/// Given a row's metadata, figure out which top-level menu items are enabled.
/// Mirrors the TS `applies()` guards.
[[nodiscard]] inline std::array<bool, 10> ComputeEnabledMask(const MessageMetadata& meta,
                                                             std::size_t pinned_count,
                                                             bool can_redact_admin) {
    std::array<bool, 10> enabled{};
    enabled.fill(true);

    // Regenerate only on assistant rows
    enabled[1] = (meta.role == MessageRole::Assistant);
    enabled[2] = enabled[1]; // model-switch same guard as regenerate

    // Edit only on user rows
    enabled[3] = (meta.role == MessageRole::User);

    // Pin cap: 3.  If already pinned, still allow unpin.
    enabled[6] = meta.pinned || (pinned_count < 3);

    // Redact: admin only, meaningful on assistant/thinking-ish rows
    enabled[9] = can_redact_admin;

    return enabled;
}

// =========================================================================
// 2b.  Keyboard + mouse dispatch for the context menu
// =========================================================================

/// Update `state` given one keystroke.  Returns true if the menu consumed
/// the event (caller must not propagate into the message list).
///
/// Caller is responsible for invoking the right callback for the chosen
/// item after `dispatch_result` reports `Execute`.  We don't invoke them
/// directly because the caller owns the callback object and the menu state
/// may live on the stack.
enum class MenuDispatch : std::uint8_t {
    Ignored,
    Handled,
    /// Caller should invoke the callback for (item, sub-choice).  `HandleMenuKeys`
    /// sets the choice fields on `state` before returning this value.
    Execute,
    Close,
};

/// Result + submenu choice payload combined.
struct MenuDispatchResult {
    MenuDispatch outcome = MenuDispatch::Ignored;
    MenuItemKind item = MenuItemKind::Copy;
    // discriminant based on submenu:
    std::variant<std::monostate, CopyFormat, std::string, MessageTag> extra;
};

/// Populates the enabled mask for a row, then drives the menu FSM.
MenuDispatchResult HandleMenuKeys(Event& ev,
                                         MessageActionsMenuState& state,
                                         const std::array<bool, 10>& enabled_mask) {
    using MD = MenuDispatch;

    // ---- opening triggers (not-open → open) -----------------------------
    if (!state.open) {
        if (ev == Event::Character('m') || ev == Event::Return) {
            state.open = true;
            state.selected_index = 0;
            state.submenu = SubmenuOpen::None;
            state.editing = false;
            return { MD::Handled };
        }
        return { MD::Ignored };
    }

    // ---- inline edit mode ----------------------------------------------
    if (state.editing) {
        if (ev == Event::Escape) {
            state.editing = false;
            state.edit_buffer.clear();
            return { MD::Handled };
        }
        if (ev == Event::Return) {
            state.editing = false;
            state.open = false;
            MenuDispatchResult r;
            r.outcome = MD::Execute;
            r.item = MenuItemKind::Edit;
            r.extra.emplace<std::string>(std::move(state.edit_buffer));
            state.edit_buffer.clear();
            return r;
        }
        if (ev.is_character()) {
            char ch = ev.character()[0];
            if (ch == 127 /* backspace */ || ch == '\b') {
                if (!state.edit_buffer.empty()) state.edit_buffer.pop_back();
            } else if (static_cast<unsigned char>(ch) >= 0x20) {
                state.edit_buffer.push_back(ch);
            }
            return { MD::Handled };
        }
        if (ev == Event::Backspace) {
            if (!state.edit_buffer.empty()) state.edit_buffer.pop_back();
            return { MD::Handled };
        }
        return { MD::Ignored };
    }

    // ---- close ---------------------------------------------------------
    if (ev == Event::Escape) {
        if (state.submenu != SubmenuOpen::None) {
            state.submenu = SubmenuOpen::None;
            state.sub_index = 0;
            return { MD::Handled };
        }
        state.open = false;
        return { MD::Close };
    }

    // --- submenu open → only handle its keys ----------------------------
    if (state.submenu == SubmenuOpen::Copy) {
        const std::size_t N = detail::kCopyItems.size();
        if (ev == Event::Character('j') || ev == Event::ArrowDown) {
            state.sub_index = std::min(N - 1, state.sub_index + 1); return { MD::Handled };
        }
        if (ev == Event::Character('k') || ev == Event::ArrowUp) {
            state.sub_index = (state.sub_index == 0) ? 0 : state.sub_index - 1; return { MD::Handled };
        }
        if (ev == Event::Character('h') || ev == Event::ArrowLeft) {
            state.submenu = SubmenuOpen::None; state.sub_index = 0; return { MD::Handled };
        }
        if (ev == Event::Return || ev == Event::Character('l') || ev == Event::ArrowRight) {
            MenuDispatchResult r;
            r.outcome = MD::Execute;
            r.item = MenuItemKind::Copy;
            r.extra = detail::kCopyItems[state.sub_index].second;
            state.open = false;
            state.submenu = SubmenuOpen::None;
            return r;
        }
        return { MD::Ignored };
    }

    if (state.submenu == SubmenuOpen::RegenModel) {
        const std::size_t N = detail::kPopularModels.size();
        if (ev == Event::Character('j') || ev == Event::ArrowDown) {
            state.sub_index = std::min(N - 1, state.sub_index + 1); return { MD::Handled };
        }
        if (ev == Event::Character('k') || ev == Event::ArrowUp) {
            state.sub_index = (state.sub_index == 0) ? 0 : state.sub_index - 1; return { MD::Handled };
        }
        if (ev == Event::Character('h') || ev == Event::ArrowLeft) {
            state.submenu = SubmenuOpen::None; state.sub_index = 0; return { MD::Handled };
        }
        if (ev == Event::Return || ev == Event::Character('l') || ev == Event::ArrowRight) {
            MenuDispatchResult r;
            r.outcome = MD::Execute;
            r.item = MenuItemKind::RegenModel;
            r.extra.emplace<std::string>(detail::kPopularModels[state.sub_index]);
            state.open = false;
            state.submenu = SubmenuOpen::None;
            return r;
        }
        return { MD::Ignored };
    }

    if (state.submenu == SubmenuOpen::Tag) {
        // Tag submenu: keys 1-8 toggle existing tags, 'n' new tag, Enter confirm
        if (ev == Event::Return) {
            MenuDispatchResult r;
            r.outcome = MD::Execute;
            r.item = MenuItemKind::Tag;
            // Caller combines (toggled_indices + pending_new).  We signal
            // "use caller-side tag model" via monostate + caller reads state.
            state.open = false;
            state.submenu = SubmenuOpen::None;
            return r;
        }
        if (ev == Event::Character('h') || ev == Event::ArrowLeft) {
            state.submenu = SubmenuOpen::None; return { MD::Handled };
        }
        if (ev == Event::Character('j') || ev == Event::ArrowDown) {
            state.sub_index += 1; return { MD::Handled };
        }
        if (ev == Event::Character('k') || ev == Event::ArrowUp) {
            state.sub_index = (state.sub_index == 0) ? 0 : state.sub_index - 1;
            return { MD::Handled };
        }
        // digit toggles handled by caller (they own the tag list)
        return { MD::Ignored };
    }

    // ---- top-level navigation ------------------------------------------
    const auto advance = [&](int delta) {
        std::size_t idx = state.selected_index;
        for (int tries = 0; tries < 20; ++tries) {
            int next = static_cast<int>(idx) + delta;
            if (next < 0) next = static_cast<int>(detail::kTopLevelItems.size()) - 1;
            if (next >= static_cast<int>(detail::kTopLevelItems.size())) next = 0;
            idx = static_cast<std::size_t>(next);
            if (enabled_mask[idx]) { state.selected_index = idx; return; }
        }
    };

    if (ev == Event::Character('j') || ev == Event::ArrowDown) { advance(+1); return { MD::Handled }; }
    if (ev == Event::Character('k') || ev == Event::ArrowUp)   { advance(-1); return { MD::Handled }; }

    // digit shortcuts
    if (ev.is_character() && !ev.character().empty()) {
        char c = ev.character()[0];
        std::size_t target = c == '0' ? 9 : static_cast<std::size_t>(c - '1');
        if (c >= '0' && c <= '9' && target < detail::kTopLevelItems.size() && enabled_mask[target]) {
            state.selected_index = target;
            // digits directly execute non-submenu items
            auto kind = detail::kTopLevelItems[target].kind;
            if (kind == MenuItemKind::Copy)       { state.submenu = SubmenuOpen::Copy;       state.sub_index = 0; return { MD::Handled }; }
            if (kind == MenuItemKind::RegenModel) { state.submenu = SubmenuOpen::RegenModel; state.sub_index = 0; return { MD::Handled }; }
            if (kind == MenuItemKind::Tag)        { state.submenu = SubmenuOpen::Tag;        state.sub_index = 0; return { MD::Handled }; }
            if (kind == MenuItemKind::Edit) {
                state.editing = true;
                state.edit_buffer.clear();
                return { MD::Handled };
            }
            MenuDispatchResult r;
            r.outcome = MD::Execute;
            r.item = kind;
            state.open = false;
            return r;
        }
    }

    // open submenus or execute with Enter / l / Right
    if (ev == Event::Return || ev == Event::Character('l') || ev == Event::ArrowRight) {
        const auto kind = detail::kTopLevelItems[state.selected_index].kind;
        if (kind == MenuItemKind::Copy)       { state.submenu = SubmenuOpen::Copy;       state.sub_index = 0; return { MD::Handled }; }
        if (kind == MenuItemKind::RegenModel) { state.submenu = SubmenuOpen::RegenModel; state.sub_index = 0; return { MD::Handled }; }
        if (kind == MenuItemKind::Tag)        { state.submenu = SubmenuOpen::Tag;        state.sub_index = 0; return { MD::Handled }; }
        if (kind == MenuItemKind::Edit) {
            state.editing = true;
            state.edit_buffer.clear();
            return { MD::Handled };
        }
        MenuDispatchResult r;
        r.outcome = MD::Execute;
        r.item = kind;
        state.open = false;
        return r;
    }

    return { MD::Ignored };
}

/// Handle a mouse event for the menu.  Returns true when the event consumed
/// the click (either selecting a menu row or triggering a submenu item).
bool HandleMenuMouse(Event& ev,
                            MessageActionsMenuState& state,
                            const std::array<bool, 10>& enabled_mask,
                            const Box& menu_box) {
    if (!state.open) {
        // Right click anywhere on a row → open menu for that row (caller is
        // expected to have set state.target already).  Left-click on the ⋮
        // button is also treated as "open menu".
        if (ev.mouse().button == Mouse::Right && ev.mouse().motion == Mouse::Pressed) {
            state.open = true;
            state.selected_index = 0;
            state.submenu = SubmenuOpen::None;
            return true;
        }
        return false;
    }
    if (!ev.is_mouse()) return false;
    // if click lands outside, close
    if (!menu_box.Contain(ev.mouse().x, ev.mouse().y)) {
        state.open = false;
        state.submenu = SubmenuOpen::None;
        return true;
    }
    const int rel_y = ev.mouse().y - menu_box.y_min;
    // row 0 = title, row 1 = separator, row 2.. = items
    const int item_row = rel_y - 2;
    if (item_row < 0) return true;
    std::size_t idx = static_cast<std::size_t>(item_row);
    if (idx >= detail::kTopLevelItems.size()) return true;
    if (!enabled_mask[idx]) return true;
    state.selected_index = idx;
    if (ev.mouse().motion == Mouse::Released) {
        // emulate Enter on release
        Event fake = Event::Return;
        std::array<bool, 10> mask_copy = enabled_mask;
        (void)HandleMenuKeys(fake, state, mask_copy);
    }
    return true;
}

// =========================================================================
// 2c.  Pure rendering of the menu tree
// =========================================================================

/// One top-level row, including its expand glyph when it owns a submenu.
[[nodiscard]] inline Element RenderTopLevelRow(const MenuItem& item, bool selected, bool enabled) {
    Elements parts;
    parts.push_back(text(selected ? "▶ " : "  "));

    if (item.shortcut_digit) {
        Color sc_color = selected ? Color(Color::White) : Color(Color::CadetBlue);
        parts.push_back(text(std::format("[{}] ", item.shortcut_digit))
                       | color(sc_color) | bold);
    } else {
        parts.push_back(text("    "));
    }
    if (item.icon) {
        parts.push_back(text(std::format("{} ", *item.icon)));
    }

    auto label = text(item.label);
    if (!enabled) label = label | dim | strikethrough;
    else if (selected) label = label | bold;
    parts.push_back(std::move(label));

    // submenu indicator
    if (item.kind == MenuItemKind::Copy
     || item.kind == MenuItemKind::RegenModel
     || item.kind == MenuItemKind::Tag) {
        parts.push_back(filler());
        parts.push_back(text(" ›") | (enabled ? color(Color::GrayDark) : dim));
    }

    auto row = hbox(std::move(parts));
    if (selected)  row = std::move(row) | bgcolor(Color::Grey27);
    if (!enabled)   row = std::move(row) | color(Color::GrayDark);
    return row | size(WIDTH, GREATER_THAN, 34);
}

/// Return a shared element for the submenu floating to the right of the
/// selected top-level row.  The caller packs both boxes into a dbox.
[[nodiscard]] inline Elements RenderSubmenu(SubmenuOpen sub,
                                            std::size_t sub_index,
                                            const std::vector<MessageTag>& available_tags = {}) {
    using P = std::pair<std::string, bool>;
    std::vector<P> rows;
    switch (sub) {
    case SubmenuOpen::None: return {};
    case SubmenuOpen::Copy:
        rows.emplace_back("Plain text", false);
        rows.emplace_back("JSON",       false);
        break;
    case SubmenuOpen::RegenModel:
        for (const char* m : detail::kPopularModels) rows.emplace_back(m, false);
        break;
    case SubmenuOpen::Tag: {
        for (const auto& t : available_tags) rows.emplace_back("#" + t.label, false);
        rows.emplace_back("✎ New tag…", false);
        break;
    }
    }
    Elements out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        bool sel = (i == sub_index);
        auto row = hbox({ text(sel ? "● " : "○ "),
                          text(rows[i].first) | (sel ? bold : nothing) })
                 | (sel ? bgcolor(Color::Grey27) : nothing)
                 | size(WIDTH, GREATER_THAN, 22);
        out.push_back(std::move(row));
    }
    return out;
}

/// Render the context-menu window (+ optional aligned submenu).  `menu_box`
/// reference is filled with the rendered outer window box (for mouse hits).
///
/// NOTE: Pixel-perfect alignment is not enforced; in a TTY we only guarantee
/// that the submenu visually docks to the right of the parent row.
Element RenderMessageActionsMenu(const MessageActionsMenuState& state,
                                        const std::array<bool, 10>& enabled_mask,
                                        const MessageMetadata* row_meta,
                                        const std::vector<MessageTag>& available_tags,
                                        std::string_view header = "Message actions") {
    Elements body;
    body.reserve(detail::kTopLevelItems.size());
    for (std::size_t i = 0; i < detail::kTopLevelItems.size(); ++i) {
        body.push_back(RenderTopLevelRow(detail::kTopLevelItems[i],
                                         i == state.selected_index,
                                         enabled_mask[i]));
    }

    auto main_list = vbox(std::move(body));

    // if submenu open, render a floating dbox to the right of selected row
    if (state.submenu != SubmenuOpen::None) {
        auto sub_rows = RenderSubmenu(state.submenu, state.sub_index, available_tags);
        Element sub_inner = vbox(std::move(sub_rows))
                           | color(Color(Color::SteelBlue))
                           | size(WIDTH, GREATER_THAN, 24);
        Element sub_window = window(text(""), std::move(sub_inner));

        Elements stacked;
        for (std::size_t i = 0; i < detail::kTopLevelItems.size(); ++i) {
            Element row = RenderTopLevelRow(detail::kTopLevelItems[i],
                                            i == state.selected_index,
                                            enabled_mask[i]);
            if (i == state.selected_index) {
                row = hbox({ row, text(" "), sub_window });
            }
            stacked.push_back(std::move(row));
        }
        auto w = window(text(std::string{header}), vbox(std::move(stacked)));
        // annotate the row we're acting on (role + id) at the bottom
        if (row_meta) {
            std::string role;
            switch (row_meta->role) {
                case MessageRole::User:       role = "user"; break;
                case MessageRole::Assistant:  role = "assistant"; break;
                case MessageRole::System:     role = "system"; break;
                case MessageRole::ToolUse:    role = "tool_use"; break;
                case MessageRole::ToolResult: role = "tool_result"; break;
                case MessageRole::GroupedTools: role = "grouped_tools"; break;
                case MessageRole::Attachment: role = "attachment"; break;
            }
            return vbox({ w, separator(),
                          hbox({ text("on: ") | dim, text(role) | bold,
                                  text(row_meta->pinned ? " 📌" : ""),
                                  text(row_meta->bookmarked ? " ⭐" : ""),
                                  filler(),
                                  text("j/k/↑/↓ select · ↵/l execute · h/← back · Esc close") | dim }) });
        }
        return w;
    }

    auto w = window(text(std::string{header}), main_list);
    if (row_meta) {
        std::string role = "unknown";
        switch (row_meta->role) {
            case MessageRole::User:       role = "user"; break;
            case MessageRole::Assistant:  role = "assistant"; break;
            case MessageRole::System:     role = "system"; break;
            case MessageRole::ToolUse:    role = "tool_use"; break;
            case MessageRole::ToolResult: role = "tool_result"; break;
            case MessageRole::GroupedTools: role = "grouped_tools"; break;
            case MessageRole::Attachment: role = "attachment"; break;
        }
        return vbox({ w, separator(),
                      hbox({ text("on: ") | dim, text(role) | bold,
                              text(row_meta->pinned ? " 📌" : ""),
                              text(row_meta->bookmarked ? " ⭐" : ""),
                              filler(),
                              text("1-9/0 direct · ⋮ or m / Enter open · Esc close") | dim }) });
    }
    return w;
}

/// The small "⋮" always-on badge rendered for long rows (>20 lines).
Element RenderLongRowOverflowHint(bool active) {
    auto body = text(" ⋮ ") | bold
              | color(active ? Color::Cyan : Color::GrayDark)
              | borderHeavy;
    if (active) body = body | color(Color::Cyan);
    return body;
}

// =========================================================================
// 3.  Helper: turn a MenuDispatchResult into concrete callback invocations.
//
// This keeps the mapping between the menu enum and the callbacks struct
// centralised so callers don't re-implement it.
// =========================================================================

/// Execute the given menu result against the supplied callbacks and
/// metadata.  The caller owns `meta` (row metadata), `available_tags` (for
/// Tag submenu), `pinned_count` (for the Pin guard), and `cb` (dispatch
/// table).  Returns true if any callback was invoked.
bool DispatchMenuResult(const MenuDispatchResult& r,
                               const MessageMetadata& meta,
                               const MessageActionsCallbacks& cb,
                               const std::vector<MessageTag>& /*available_tags*/,
                               std::size_t /*pinned_count*/) {
    if (r.outcome != MenuDispatch::Execute || !cb.on_copy) return false;
    // (caller ensures cb is populated)
    switch (r.item) {
    case MenuItemKind::Copy:
        if (auto* fmt = std::get_if<CopyFormat>(&r.extra)) {
            if (cb.on_copy) cb.on_copy(meta.id, *fmt);
            return true;
        }
        return false;
    case MenuItemKind::Regenerate:
        if (cb.on_regenerate) cb.on_regenerate(meta.id);
        return true;
    case MenuItemKind::RegenModel:
        if (auto* mdl = std::get_if<std::string>(&r.extra)) {
            if (cb.on_regen_model) cb.on_regen_model(meta.id, *mdl);
            return true;
        }
        return false;
    case MenuItemKind::Edit:
        if (auto* txt = std::get_if<std::string>(&r.extra)) {
            if (cb.on_edit_commit) cb.on_edit_commit(meta.id, *txt);
            return true;
        }
        return false;
    case MenuItemKind::ReplyQuote:
        // Caller decides what "quoted" means; we pass empty string and
        // let the caller build it from their payload.
        if (cb.on_reply_quote) cb.on_reply_quote(meta.id, "");
        return true;
    case MenuItemKind::Tag:
        if (cb.on_tag) cb.on_tag(meta.id, meta.tags);
        return true;
    case MenuItemKind::Pin:
        if (cb.on_pin_toggle) cb.on_pin_toggle(meta.id);
        return true;
    case MenuItemKind::ShareExport:
        if (cb.on_share) cb.on_share(meta.id, ShareFormat::JsonBlob);
        return true;
    case MenuItemKind::Delete:
        if (cb.on_delete) cb.on_delete(meta.id, DeleteRiskLevel(1));
        return true;
    case MenuItemKind::Redact:
        if (cb.on_redact_toggle) cb.on_redact_toggle(meta.id);
        return true;
    }
    return false;
}

} // namespace cc::ui::messages::interactions
