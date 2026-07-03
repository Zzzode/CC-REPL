/// =========================================================================
/// @file messages_list.cppm
/// @brief Messages list container + single-message envelope wrapper.
///
/// UI21 RESPONSIBILITY — "List container + single-message envelope" (REPL main path P0 blocker)
///   MIGRATION SOURCES:
///     - src/components/Messages.tsx  (834 LOC)
///         * build_visible_rows : O(N) filter + search + compact-group collapsing
///         * selection / streaming-tail cursor / last-N render cap
///         * keyboard nav (j/k, g/G, / search, c/r/d actions, Space toggle)
///         * yframe scroll + vscroll_indicator, empty-state rendering
///     - src/components/Message.tsx   (626 LOC)
///         * render_message_envelope: avatar column + role pill + timestamp
///           + status badge + dismiss + per-role accent border (Top)
///         * variant dispatch to RenderMessageRowByType (UI4 message_row)
///
/// DEPENDENCIES (STRICT — no type/color duplication):
///   import cc.ui.messages.message_row;
///       -> MessageShape enum  +  MessageRowPayload variant
///       +  RenderMessageRowByType(shape, payload, callbacks)
///   import cc.ui.messages.message_timestamp;
///   import cc.ui.design.themed_text;        // UI20 tokens placeholder
///   import cc.ui.design.themed_box;         // UI20 primitive stand-in
///   import cc.ui.components.spinner;        // for streaming tail glyph
///
///   NOTE: UI20 design.tokens / design.primitives modules are not yet
///   materialised in the C++ tree.  We import themed_text + themed_box as
///   their working stand-ins; every palette lookup goes through the small
///   inline helpers `palette::*()` so that swapping the real modules is a
///   one-line grep.  When cc.ui.design.tokens arrives, replace those
///   helpers with the real token API — nothing else changes.
/// =========================================================================
///
/// ┌───────────────────────────────────────────────────────────────────────┐
/// │  PUBLIC API                                                           │
/// ├───────────────────────────────────────────────────────────────────────┤
/// │  struct MessagesListInput                                             │
/// │    ├─ vector<MessageRowPayload> rows                                  │
/// │    ├─ vector<MessageShape>       shapes   (parallel to rows)         │
/// │    ├─ optional<size_t> selected_row_idx                               │
/// │    ├─ size_t streaming_tail_row   (= rows.size() when not streaming) │
/// │    ├─ vector<pair<size_t,size_t>> compact_boundary_groups            │
/// │    ├─ Filters  {show_system, show_tool_in, show_tool_out,            │
/// │    │             show_thinking, show_compact}                         │
/// │    ├─ string search_query    (case-insensitive substring match)      │
/// │    └─ optional<size_t> jump_to_row_on_init                           │
/// │                                                                      │
/// │  enum class ActionKind { Copy, Regenerate, Delete }                  │
/// │                                                                      │
/// │  struct MessagesListCallbacks                                        │
/// │    ├─ on_select(size_t row_idx)                                      │
/// │    ├─ on_action(size_t row_idx, ActionKind)                          │
/// │    ├─ on_toggle_compact_group(size_t group_idx)                      │
/// │    ├─ on_click_attachment(size_t row_idx, size_t attachment_idx)     │
/// │    └─ on_search_changed(const string& query)                         │
/// │                                                                      │
/// │  build_visible_rows(input) -> vector<VisibleRow>   (pure O(N))       │
/// │                                                                      │
/// │  render_messages_list_view(input, frame_count) -> Element            │
/// │    ├─ STATIC / NON-INTERACTIVE variant                               │
/// │    ├─ used by dialog builders that just want the visual list        │
/// │    └─ frame_count drives blink + spinner glyph                       │
/// │                                                                      │
/// │  MakeMessagesList(input, callbacks) -> Component                     │
/// │    ├─ FULLY INTERACTIVE variant                                      │
/// │    ├─ keyboard nav + embedded search Input + action hotkeys          │
/// │    └─ delegates per-row rendering to envelope + UI4 dispatch        │
/// │                                                                      │
/// │  render_message_envelope(...) -> Element                             │
/// │    ├─ 36-col avatar column + role pill + timestamp + status badge   │
/// │    ├─ per-role top accent border                                     │
/// │    └─ inner = RenderMessageRowByType payload dispatch                │
/// └───────────────────────────────────────────────────────────────────────┘
///
/// CAPS  (kept consistent with TS Messages.tsx semantics):
///   * MAX_ROWS_RENDERED_LAST_N = 80
///       -> non-virtualized "last-N" window for render.  The UI22
///          VirtualScroll wrapper is expected to bump this to 200+.
///   * build_visible_rows is O(N) over rows.size() (<= 100 000) and
///     guaranteed sub-16 ms on reasonable hardware (single pass, no
///     allocations per-row beyond string::find on lowered text).
/// =========================================================================

module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
// NOTE: ftxui/component/input.hpp is not a standalone header in upstream
// FTXUI; Input() is exported via ftxui/component/component.hpp.
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.messages.messages_list;

// ─── Strict re-uses (no type / colour duplication) ──────────────────────
import cc.ui.messages.message_row;
import cc.ui.messages.message_timestamp;
import cc.ui.messages.virtual_list;   // P0-3: VirtualMessageList types + factory

// Per-message-type structs — imported explicitly since C++20 modules
// don't re-export imported entities by default (and message_row keeps
// its import list private to avoid BMI size bloat).
import cc.ui.messages.user_text_message;
import cc.ui.messages.message_user_command;
import cc.ui.messages.message_bash_io;
import cc.ui.user_message;
import cc.ui.messages.message_image;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.local_command_output_message;
import cc.ui.messages.attachment_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.system_text_message;
import cc.ui.error_message;
import cc.ui.messages.api_error_message;
import cc.ui.messages.collapsed_content_message;
import cc.ui.messages.message_components;   // RateLimitInfo
import cc.ui.messages.message_plan_approval;
import cc.ui.messages.message_hook_progress;
import cc.ui.messages.message_shutdown;
import cc.ui.messages.message_advisor;

import cc.ui.tools.registry;
import cc.ui.tools.generic;

import cc.ui.design.themed_text;
import cc.ui.design.themed_box;
import cc.ui.design.tokens;         // Role + Palette for divider color
import ui.components.spinner;

// =========================================================================
// Small palette helpers — tokens placeholders (swap for cc.ui.design.tokens)
// =========================================================================
// Each lookup returns an ftxui::Color.  Kept in a single namespace so the
// grep-replace for real tokens is mechanical.

namespace cc::ui::messages_list::palette {

using ftxui::Color;

inline auto role_bg_user()        -> Color { return Color::RGB(30, 41, 59); }
inline auto role_bg_assistant()   -> Color { return Color::RGB(30, 41, 59); }
inline auto role_bg_system()      -> Color { return Color::RGB(30, 30, 36); }
inline auto role_bg_tool()        -> Color { return Color::RGB(24, 40, 40); }
inline auto role_bg_thinking()    -> Color { return Color::RGB(34, 30, 48); }

inline auto role_pill_user()      -> Color { return Color::RGB(59, 130, 246); }   // blue-500
inline auto role_pill_assistant() -> Color { return Color::RGB(168, 85, 247); }   // purple-500
inline auto role_pill_system()    -> Color { return Color::RGB(234, 179,  8); }   // yellow-500
inline auto role_pill_tool()      -> Color { return Color::RGB(20, 184, 166); }   // teal-500
inline auto role_pill_thinking()  -> Color { return Color::RGB(139, 92, 246); }   // violet-500

inline auto accent_top_user()     -> Color { return role_pill_user(); }
inline auto accent_top_assistant()-> Color { return role_pill_assistant(); }
inline auto accent_top_system()   -> Color { return role_pill_system(); }
inline auto accent_top_tool()     -> Color { return role_pill_tool(); }
inline auto accent_top_error()    -> Color { return Color::RGB(239, 68, 68); }    // red-500
inline auto accent_top_redacted() -> Color { return Color::RGB(107, 114, 128); }  // gray-500

inline auto selected_bg()         -> Color { return Color::RGB(30, 64, 175); }    // blue-800
inline auto muted_fg()            -> Color { return Color::RGB(156, 163, 175); }  // gray-400
inline auto empty_state_fg()      -> Color { return Color::RGB(107, 114, 128); }  // gray-500
inline auto streaming_fg()        -> Color { return Color::RGB(34, 211, 238); }  // cyan-400

} // namespace cc::ui::messages_list::palette

// =========================================================================
export namespace cc::ui::messages_list {

using namespace ftxui;
using namespace messages;  // UI4: MessageShape, MessageRowPayload, …

using ::cc::ui::messages::MessageShape;
using ::cc::ui::messages::MessageRowPayload;
using ::cc::ui::messages::MessageRowCallbacks;
using ::cc::ui::messages::RenderMessageRowByType;

// =========================================================================
// 1)  Input types
// =========================================================================

struct Filters {
    bool show_system   = true;
    bool show_tool_in  = true;
    bool show_tool_out = true;
    bool show_thinking = true;
    bool show_compact  = true;   // if false, collapsed groups render as rows
};

/// TS REF: src/components/FullscreenLayout.tsx L224-227
///   export type UnseenDivider = {
///     firstUnseenUuid: Message['uuid'];
///     count: number;
///   };
/// In-transcript "N new messages" divider anchor + count.  When set, the
/// renderer inserts a muted separator line BEFORE the first renderable row
/// whose uuid shares the 24-char prefix with firstUnseenUuid.
struct UnseenDivider {
    /// UUID (or 24-char prefix) of the first message that arrived after the
    /// user scrolled away from the tail.  The render side matches on the
    /// first 24 chars (TS: deriveUUID preserves the source message's 24-char
    /// prefix across derived content blocks so grouped rows still match).
    std::string first_unseen_uuid_prefix;
    /// Number of new assistant turns (floors at 1 if any unseen content
    /// exists, matching TS Math.max(1, countUnseenAssistantTurns(...))).
    std::size_t count = 0;
};

struct MessagesListInput {
    std::vector<MessageRowPayload> rows;
    /// Parallel to `rows`.  The dispatcher needs both shape and payload.
    std::vector<MessageShape>       shapes;
    /// Parallel to `rows`.  Used by the UnseenDivider prefix-match anchor
    /// (TS: firstUnseenUuid 24-char match).  Empty strings are allowed —
    /// rows without a uuid simply never match the divider anchor.
    std::vector<std::string>        uuids;

    std::optional<std::size_t>      selected_row_idx;
    /// Row at which the spinner + blinking "generating…" cursor is drawn.
    /// When == rows.size() the tail is NOT rendered (not streaming).
    std::size_t                     streaming_tail_row = 0;

    /// Compact groups as half-open [start, end) intervals over `rows`.
    /// When show_compact=true each group appears as a SINGLE collapsed row
    /// ("📦 N messages collapsed …"); when false the inner rows render.
    std::vector<std::pair<std::size_t, std::size_t>> compact_boundary_groups;

    Filters                         filters;
    std::string                     search_query;   // case-insensitive
    std::optional<std::size_t>      jump_to_row_on_init;
    bool                            pin_to_bottom = false;
    int                             scroll_offset = 0;
    int                             viewport_rows = 40;

    /// TS REF: Messages.tsx L240 (unseenDivider prop) + L549-553 (prefix match).
    /// When set, a colored divider line is inserted before the matching row.
    std::optional<UnseenDivider>    unseen_divider;
};

enum class ActionKind { Copy, Regenerate, Delete };

struct MessagesListCallbacks {
    std::function<void(std::size_t)>                       on_select;
    std::function<void(std::size_t, ActionKind)>           on_action;
    std::function<void(std::size_t)>                       on_toggle_compact_group;
    std::function<void(std::size_t, std::size_t)>          on_click_attachment;
    std::function<void(const std::string&)>                on_search_changed;
};

// =========================================================================
// 2)  Preview-text extractor (works on every MessageRowPayload alternative)
// =========================================================================
// Used by build_visible_rows for the case-insensitive substring filter.
// Falls back to std::visit over every variant branch so we never need a
// virtual `preview_text()` method.

namespace detail {

/// Lower-case a UTF-8 string in place.  Only touches ASCII letters because
/// MessageRowPayload text fields are overwhelmingly English / path literals
/// (same strategy as TS renderableSearchText → toLowerCase).
inline auto lowered(std::string s) -> std::string {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

inline auto payload_preview(const MessageRowPayload& p) -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;

        auto take_first = [](const auto& a, const auto& b) -> std::string {
            using A = std::decay_t<decltype(a)>;
            using B = std::decay_t<decltype(b)>;
            if constexpr (!std::is_same_v<A, std::nullptr_t>) {
                if constexpr (std::is_convertible_v<A, std::string>) return std::string(a);
                if constexpr (requires{ std::to_string(a); }) return std::to_string(a);
            }
            if constexpr (!std::is_same_v<B, std::nullptr_t>) {
                if constexpr (std::is_convertible_v<B, std::string>) return std::string(b);
            }
            return {};
        };

        // --- User family ---
        if constexpr (std::is_same_v<T, UserTextMessageData>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "user-text";
        }
        else if constexpr (std::is_same_v<T, user_command::UserCommandData>) {
            if constexpr (requires{ v.command_line; }) return take_first(v.command_line, nullptr);
            return "user-command";
        }
        else if constexpr (std::is_same_v<T, BashIOEntry>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "bash-io";
        }
        else if constexpr (std::is_same_v<T, UserMessageData>) {
            if constexpr (requires{ v.title; }) return take_first(v.title, nullptr);
            return "user-message";
        }
        else if constexpr (std::is_same_v<T, image::ImageMessageData>) {
            if constexpr (requires{ v.file_name; }) {
                return std::string{"image "} + take_first(v.file_name, nullptr);
            }
            return "image";
        }
        else if constexpr (std::is_same_v<T, ToolResultOptions>) {
            if constexpr (requires{ v.tool_name; }) return take_first(v.tool_name, nullptr);
            return "tool-result";
        }
        else if constexpr (std::is_same_v<T, local_cmd::LocalCommandOptions>) {
            return "local-command";
        }
        else if constexpr (std::is_same_v<T, attachment_message::AttachmentGridOptions>) {
            return "attachments";
        }
        // --- Assistant family ---
        else if constexpr (std::is_same_v<T, AssistantTextMessageData>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "assistant-text";
        }
        else if constexpr (std::is_same_v<T, tool_use_message::ToolUseRenderOptions>) {
            return "tool-use";
        }
        else if constexpr (std::is_same_v<T, tool_use_message::GroupedToolsOptions>) {
            return "grouped-tools";
        }
        else if constexpr (std::is_same_v<T, thinking_message::ThinkingMessageOptions>) {
            return "thinking";
        }
        // --- System family ---
        else if constexpr (std::is_same_v<T, SystemTextMessageData>) {
            if constexpr (requires{ v.summary; }) return take_first(v.summary, nullptr);
            return "system-text";
        }
        else if constexpr (std::is_same_v<T, ErrorMessageData>) {
            return "error";
        }
        else if constexpr (std::is_same_v<T, api_error_message::APIErrorOptions>) {
            return "api-error";
        }
        else if constexpr (std::is_same_v<T, collapsed_content::CollapsedContentOptions>) {
            return "collapsed-content";
        }
        else if constexpr (std::is_same_v<T, RateLimitInfo>) {
            if constexpr (requires{ v.reason; }) {
                return take_first(v.reason, nullptr) + " " +
                       take_first(v.message, nullptr);
            }
            return "rate-limit";
        }
        else if constexpr (std::is_same_v<T, PlanApprovalOptions>) {
            return "plan-approval";
        }
        else if constexpr (std::is_same_v<T, std::vector<HookProgressEntry>>) {
            return "hook-progress";
        }
        else if constexpr (std::is_same_v<T, shutdown::ShutdownMessageData>) {
            return "shutdown";
        }
        else if constexpr (std::is_same_v<T, AdvisorMessage>) {
            return "advisor";
        }
        else if constexpr (std::is_same_v<T, UI5HandledTag>) {
            return {};
        }
        else {
            static_assert(!sizeof(T*), "Unreachable: new variant alternative in payload_preview.");
            return {};
        }
    }, p);
}

/// Returns true for message SHAPEs that belong to each filter category.
/// Mirrors the TS Messages.tsx category switches (system / tool_use /
/// tool_result / thinking / compacted).
inline auto shape_category(MessageShape s) -> std::string_view {
    switch (s) {
        // system family
        case MessageShape::SystemText:
        case MessageShape::SystemRateLimit:
        case MessageShape::SystemPlanApproval:
        case MessageShape::SystemHookProgress:
        case MessageShape::SystemShutdown:
        case MessageShape::SystemCompactBoundary:
        case MessageShape::SystemAdvisor:
        case MessageShape::SystemTaskAssignment:
        case MessageShape::SystemCollapsedContent:
        case MessageShape::SystemAPIError:
            return "system";
        // tool-in (assistant issuing a tool_use)
        case MessageShape::AssistantToolUse:
        case MessageShape::AssistantGroupedTools:
            return "tool_in";
        // tool-out (user returning tool_result, bash, local cmd)
        case MessageShape::UserToolResult:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
            return "tool_out";
        // thinking
        case MessageShape::AssistantThinking:
        case MessageShape::AssistantRedactedThinking:
            return "thinking";
        default:
            return "other";
    }
}

inline auto passes_filters(MessageShape s, const Filters& f) -> bool {
    auto cat = shape_category(s);
    if (cat == "system"   && !f.show_system)   return false;
    if (cat == "tool_in"  && !f.show_tool_in)  return false;
    if (cat == "tool_out" && !f.show_tool_out) return false;
    if (cat == "thinking" && !f.show_thinking) return false;
    return true;
}

} // namespace detail

// =========================================================================
// 3)  Visible-row representation
// =========================================================================

/// A single row that render_messages_list_view / the Component will emit.
/// Either a real payload row OR a compact-group synthetic row.
struct VisibleRow {
    enum class Kind { Payload, CompactGroup };

    Kind kind = Kind::Payload;

    // For Kind::Payload — index into MessagesListInput.rows/shapes
    std::size_t row_idx = 0;

    // For Kind::CompactGroup — index into compact_boundary_groups, plus stats
    std::size_t group_idx       = 0;
    std::size_t group_count     = 0;   // number of original rows in group
    std::size_t tool_turns      = 0;
    std::size_t additions       = 0;
    std::size_t deletions       = 0;
};

// =========================================================================
// 4)  build_visible_rows  —  pure O(N) over input.rows
// =========================================================================
/// Step 1 : per-row filter + search (Filters + search_query)
/// Step 2 : if show_compact, collapse compact_boundary_groups into a
///          single "CompactGroup" visible row each.
///
/// Complexity guarantee: exactly ONE linear pass over input.rows plus ONE
/// pass over compact_boundary_groups (sorted).  Uses a std::vector<bool>
/// membership table for O(1) "is this row inside a compact group?" lookups.
inline auto build_visible_rows(MessagesListInput& input) -> std::vector<VisibleRow> {
    const auto N = input.rows.size();
    // ---- Step 0 : pre-compute lowered search needle (empty = skip search)
    const std::string needle = detail::lowered(input.search_query);
    const bool do_search     = !needle.empty();

    // ---- Step 1 : mark rows that pass (filters AND search).  Separately
    //              build a "row visible" bitmask so Step 2 can use it.
    std::vector<bool> row_passes(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        if (i >= input.shapes.size()) break;   // malformed input → safe stop
        if (!detail::passes_filters(input.shapes[i], input.filters)) continue;
        if (do_search) {
            const std::string hay = detail::lowered(detail::payload_preview(input.rows[i]));
            if (hay.find(needle) == std::string::npos) continue;
        }
        row_passes[i] = true;
    }

    // ---- Step 2 : compact-group collapsing.
    // Mark which rows are fully covered by an ACTIVE group (show_compact
    // AND every row in the range is visible).  A group whose rows were all
    // filtered out is dropped (produces no visible row at all).
    std::vector<bool> consumed_by_group(N, false);
    struct GroupInfo { std::size_t gidx, start, end, tool_turns, add, del, visible_rows; };
    std::vector<GroupInfo> active_groups;
    active_groups.reserve(input.compact_boundary_groups.size());

    if (input.filters.show_compact) {
        for (std::size_t gi = 0; gi < input.compact_boundary_groups.size(); ++gi) {
            auto [s, e] = input.compact_boundary_groups[gi];
            if (s > N) s = N;
            if (e > N) e = N;
            if (s >= e) continue;

            std::size_t vcount = 0, tool_turns = 0, add = 0, del = 0;
            for (std::size_t r = s; r < e; ++r) {
                if (!row_passes[r]) continue;
                ++vcount;
                // Tool-turn heuristics (same as TS collapsed_content_message):
                //   AssistantToolUse / AssistantGroupedTools each count as one
                //   "tool turn" inside the collapsed window.
                if (r < input.shapes.size()) {
                    auto sh = input.shapes[r];
                    if (sh == MessageShape::AssistantToolUse ||
                        sh == MessageShape::AssistantGroupedTools) {
                        ++tool_turns;
                    }
                    // Add / delete counts come from diff payloads.  We don't
                    // want to pull the structured_diff module here, so the
                    // engine can write precomputed stats alongside compact
                    // boundary groups. Until that richer input shape lands,
                    // approximate from UserToolResult preview_text
                    // occurrences of "+++" / "---" markers.
                    if (sh == MessageShape::UserToolResult && r < input.rows.size()) {
                        const std::string t = detail::payload_preview(input.rows[r]);
                        // rough counts
                        auto count_needle = [&](std::string_view pat) -> std::size_t {
                            std::size_t c = 0, pos = 0;
                            while ((pos = t.find(pat, pos)) != std::string::npos) {
                                ++c; pos += pat.size();
                            }
                            return c;
                        };
                        add += count_needle("+++");
                        del += count_needle("---");
                    }
                }
            }
            if (vcount == 0) continue;   // whole group filtered → drop it

            active_groups.push_back({ gi, s, e, tool_turns, add, del, vcount });
            for (std::size_t r = s; r < e; ++r) consumed_by_group[r] = true;
        }
    }

    // ---- Step 3 : linear merge walk.
    // Groups may arrive in any order but *typically* are sorted; we walk
    // rows in ascending order and insert a group row exactly when we cross
    // its start boundary.  Result is always well-ordered regardless.
    std::vector<VisibleRow> out;
    out.reserve(N);

    // For O(1) group lookup by start index: build map
    std::vector<std::optional<GroupInfo>> start_to_group(N);
    for (const auto& g : active_groups) {
        if (g.start < N) start_to_group[g.start] = g;
    }

    for (std::size_t i = 0; i < N; ++i) {
        // 1) emit group row BEFORE the first consumed row of its range
        if (start_to_group[i].has_value()) {
            const auto& g = *start_to_group[i];
            out.push_back(VisibleRow{
                .kind        = VisibleRow::Kind::CompactGroup,
                .group_idx   = g.gidx,
                .group_count = g.visible_rows,
                .tool_turns  = g.tool_turns,
                .additions   = g.add,
                .deletions   = g.del,
            });
            continue;
        }
        // 2) rows inside an active group are skipped
        if (consumed_by_group[i]) continue;
        // 3) regular payload row
        if (!row_passes[i]) continue;
        out.push_back(VisibleRow{
            .kind    = VisibleRow::Kind::Payload,
            .row_idx = i,
        });
    }
    return out;
}

// =========================================================================
// Forward declarations of section-6 detail::render helpers.  These are
// defined later in the translation unit but section-3c's virtual-path
// render_messages_list_virtual already references them.
// =========================================================================
namespace detail {
    inline auto render_empty_state(const std::string& search_query) -> Element;
    inline auto render_compact_group_row(const VisibleRow& vr, bool is_selected) -> Element;
    inline auto render_payload_row(const MessagesListInput& input,
                                   std::size_t row_idx,
                                   bool is_selected,
                                   std::size_t frame_count,
                                   bool add_margin) -> Element;
} // namespace detail

// =========================================================================
// 3b) Estimated row height + VisibleRow → virtual_list::VisibleRow converter
// =========================================================================
//
// P0-3 virtual scroll needs per-row LINE estimates so JumpHandle's prefix-
// sum table produces accurate scroll thumbs.  These estimates are HEURISTIC:
//   • Short assistant text / user prompt   →  2-3 lines (min 1)
//   • Long content (tool result, bash)     →  4 + wrapped lines (capped 80)
//   • Thinking (compact)                   →  1 line  (hidden when complete)
//   • Compact group row                    →  1 line
//
// TS useVirtualScroll.ts DEFAULT_ESTIMATE = 3 (per-message) — our VisibleRow
// is *finer-grained* so we target mean ≈ 2.2 lines / VisibleRow.

namespace detail {

/// Count *wrapped* lines for `text` given terminal columns.  Mirrors TS
/// text-wrap heuristic (hard-break at term_cols, plus existing '\n').  The
/// result is the maximum vertical space the content COULD take inside a
/// 36-col reserved left-gutter message envelope; 36 is subtracted from
/// term_cols to account for the fixed avatar column.
[[nodiscard]] inline auto estimate_content_lines(
    std::string_view text,
    int term_cols,
    int envelope_gutter_cols = 36) -> int {
    const int content_cols =
        std::max(20, term_cols - envelope_gutter_cols);
    int lines = 1;
    int current = 0;
    for (char c : text) {
        if (c == '\n') {
            lines += 1;
            current = 0;
            continue;
        }
        current += 1;
        if (current > content_cols) {
            lines += 1;
            current = 0;
        }
    }
    // Clip to [1, 80] — rows taller than 80 are reported as 80 (overscan
    // covers the difference during actual scroll; FTXUI will size to real
    // content at paint time).
    return std::clamp(lines, 1, 80);
}

/// Estimate visual height for one messages_list::VisibleRow.  Adds 1 line
/// for the envelope's top-accent + role-header row and 1 for trailing
/// separator (except for 1-line rows where it collapses).
[[nodiscard]] inline auto estimate_row_height(
    const VisibleRow& vr,
    const MessagesListInput& input,
    int term_cols) -> int {
    using K = VisibleRow::Kind;
    if (vr.kind == K::CompactGroup) {
        // Collapsed "📦 27 messages collapsed (📦 8 tool turns, +++12 ---7)"
        return 1;
    }
    // Payload rows — dispatch by MessageShape content length.
    if (vr.row_idx >= input.rows.size()) return 2;
    const std::string preview =
        detail::lowered(detail::payload_preview(input.rows[vr.row_idx]));
    const MessageShape shape =
        vr.row_idx < input.shapes.size()
            ? input.shapes[vr.row_idx]
            : MessageShape::SystemTaskAssignment;

    using S = MessageShape;
    int content_lines = 1;
    switch (shape) {
        case S::AssistantThinking:
        case S::AssistantRedactedThinking:
            // Collapsed label: "∴ Thinking (ctrl+o to expand)".  If expanded
            // the caller will have already split thinking into multiple rows
            // outside our view; 2 lines covers label + separator.
            content_lines = 2;
            break;
        case S::AssistantToolUse:
        case S::AssistantGroupedTools:
            // "• tool_name arg1=…" rows typically 2-4; capped low because
            // grouped tools render their own inner grid.
            content_lines = 2 + estimate_content_lines(preview, term_cols, 40) / 2;
            break;
        case S::UserToolResult:
        case S::UserBashOutput:
            // Tallest content category — the full estimate.
            content_lines = estimate_content_lines(preview, term_cols, 4);
            break;
        case S::UserLocalCommandOutput:
            // Command output (e.g. /help, /theme list) can be dozens of
            // lines.  payload_preview returns just "local-command" for this
            // variant, so we must count actual output lines from the payload.
            if (auto* opts = std::get_if<local_cmd::LocalCommandOptions>(
                    &input.rows[vr.row_idx])) {
                // Each OutputLine is one display row; add header + footer.
                content_lines = static_cast<int>(opts->data.lines.size()) + 3;
            } else {
                content_lines = estimate_content_lines(preview, term_cols, 4);
            }
            break;
        case S::SystemText:
        case S::SystemRateLimit:
        case S::SystemPlanApproval:
        case S::SystemHookProgress:
        case S::SystemShutdown:
        case S::SystemAdvisor:
        case S::SystemTaskAssignment:
        case S::SystemAPIError:
        case S::SystemCollapsedContent:
        case S::SystemCompactBoundary:
            content_lines = 1 + estimate_content_lines(preview, term_cols, 4) / 2;
            break;
        case S::UserBashInput:
            // "> echo hello" prompt-style — short.
            content_lines = 2;
            break;
        case S::UserImage:
            content_lines = 8;   // thumbnail placeholder
            break;
        case S::UserAttachments:
            content_lines = 3;   // grid header + 1 row of thumbs
            break;
        default:
            content_lines = 1 + estimate_content_lines(preview, term_cols, 36);
            break;
    }
    // +1 line for envelope header (avatar + role pill) unless content is
    // already collapsed / system-style which shares headers.
    switch (shape) {
        case S::AssistantThinking:
        case S::AssistantRedactedThinking:
        case S::SystemCompactBoundary:
        case S::SystemCollapsedContent:
            break;   // no header row added
        default:
            content_lines += 1;
    }
    return std::clamp(content_lines, 1, 120);
}

} // namespace detail

// ── Forward declarations: UnseenDivider helpers (defined in §6 below) ─────
// These are used in §3b (render_messages_list_view) and §3c
// (render_messages_list_virtual) but their bodies live after
// render_message_envelope to keep type + envelope code contiguous.
namespace detail {

[[nodiscard]] inline auto find_divider_before_visible_index(
    const MessagesListInput& input,
    const std::vector<VisibleRow>& visible) -> std::size_t;

[[nodiscard]] inline auto render_unseen_divider(std::size_t count)
    -> ftxui::Element;

} // namespace detail (forward decl block)

/// Convert a messages_list `VisibleRow` vector into the format consumed by
/// VirtualMessageList.  Each entry carries:
///   - row_id           stable hash (for future cache invalidation)
///   - estimated_hl     content-aware estimate (lines)
///   - search_key       lowered preview text (for scroll_search callback)
///   - type_hint        0 = Payload, 1 = CompactGroup
///   - backend_index    round-trip key for render_row callback
[[nodiscard]] inline auto visible_rows_to_virtual(
    const std::vector<VisibleRow>& visible,
    const MessagesListInput& input,
    int term_cols = 80)
    -> std::vector<cc::ui::messages::virtual_list::VisibleRow>
{
    namespace vl = cc::ui::messages::virtual_list;
    std::vector<vl::VisibleRow> out;
    out.reserve(visible.size());
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const auto& vr = visible[i];
        const int est = detail::estimate_row_height(vr, input, term_cols);
        const std::uint64_t backend =
            (vr.kind == VisibleRow::Kind::CompactGroup)
                // Use MSB sentinel ~group_idx; cast safely via ull.
                ? (std::uint64_t(1) << 63) | static_cast<std::uint64_t>(vr.group_idx)
                : static_cast<std::uint64_t>(vr.row_idx);
        vl::VisibleRow row{
            .row_id                 = static_cast<std::uint64_t>(i) + 1,
            .estimated_height_lines = est,
            .height_measured        = false,
            .search_key             = {},
            .type_hint              = (vr.kind == VisibleRow::Kind::CompactGroup) ? 1 : 0,
            .backend_index          = backend,
        };
        if (row.type_hint == 0 && vr.row_idx < input.rows.size()) {
            // Populate search_key lazily for payload rows (compact-group
            // rows use a static "collapsed" label).
            row.search_key = detail::lowered(
                detail::payload_preview(input.rows[vr.row_idx]));
        }
        out.push_back(std::move(row));
    }
    return out;
}

/// Decode the backend_index set by `visible_rows_to_virtual` back into a
/// messages_list::VisibleRow.  `out` is filled in place; returns true if
/// decode succeeded, false if the key was malformed (out is reset to a
/// safe payload(0) sentinel on failure).
[[nodiscard]] inline bool decode_virtual_backend_index(
    std::uint64_t backend_index,
    VisibleRow& out) noexcept
{
    constexpr std::uint64_t kGroupBit = std::uint64_t(1) << 63;
    if ((backend_index & kGroupBit) != 0) {
        out.kind      = VisibleRow::Kind::CompactGroup;
        out.group_idx = backend_index & (~kGroupBit);
        out.row_idx   = 0;
        return true;
    }
    out.kind    = VisibleRow::Kind::Payload;
    out.row_idx = static_cast<std::size_t>(backend_index);
    out.group_idx = 0;
    return true;
}

// =========================================================================
// 3c) Render path: P0-3 VirtualMessageList when visible_rows > threshold
// =========================================================================
//
// Threshold kVirtualThreshold (default 80) matches the legacy Last-N cap
// AND TS VirtualMessageList.  Below threshold we still render everything
// via render_messages_list_view (simple, deterministic, no overscan).
// At/above threshold, this function:
//   (1) converts visible_rows → virtual rows (with height estimates)
//   (2) calls render_list_as_elements (spacer+slice+spacer, O(visible) not O(N))
//   (3) forwards render_row back to messages_list's per-row envelope renderer
//
// Returns a fully-rendered FTXUI Element (yframe + vscroll_indicator applied).
// Accepts an optional `scroll_top_lines` value so ReplScreen's existing
// scroll_offset plumbing can still drive the initial window position.

inline constexpr std::size_t kVirtualThreshold = 80;

[[nodiscard]] inline auto render_messages_list_virtual(
    const MessagesListInput& input_const,
    std::size_t frame_count,
    int viewport_rows,
    int scroll_top_lines) -> Element
{
    namespace vl = cc::ui::messages::virtual_list;

    MessagesListInput input = input_const;
    auto visible = build_visible_rows(input);
    if (visible.empty()) {
        return vbox({ detail::render_empty_state(input.search_query) })
             | yframe | vscroll_indicator;
    }

    // Build virtual rows.  Use viewport_rows + 80 as a proxy for terminal
    // height (the caller knows viewport_rows; width defaults are fine since
    // content estimates already clip generously to 20-120 range).
    const int term_cols_est = 120;   // safe default; most terminals ≥ 80
    auto virt_rows = visible_rows_to_virtual(visible, input, term_cols_est);

    // TS REF: Messages.tsx L549-553  compute dividerBeforeIndex.
    const std::size_t divider_before_vi =
        detail::find_divider_before_visible_index(input, visible);
    const bool has_divider =
        (divider_before_vi < visible.size() &&
         input.unseen_divider.has_value());

    vl::VirtualListState state;
    state.options.ascii_gutter  = true;
    state.options.auto_scroll   = input.pin_to_bottom
                                      ? vl::AutoScrollMode::Smart
                                      : vl::AutoScrollMode::Disabled;
    state.viewport_rows         = std::max(1, viewport_rows);
    state.options.viewport_rows = state.viewport_rows;
    state.rows                  = std::move(virt_rows);
    state.jh                    = vl::build_geometry(std::span{state.rows});
    // Initial scroll window
    if (input.pin_to_bottom) {
        const int max = std::max(0, state.jh.total() - state.viewport_rows);
        state.scroll_top    = max;
        state.sticky_bottom = true;
    } else {
        state.scroll_top = std::max(0, scroll_top_lines);
    }

    // ── render_row callback: translate virtual back to messages_list VR
    state.callbacks.render_row =
        [frame_count, &input, divider_before_vi, has_divider]
        (size_t row_index, const vl::VisibleRow& vr)
            -> ftxui::Element
        {
            VisibleRow ml_row{};
            if (!decode_virtual_backend_index(vr.backend_index, ml_row)) {
                return text("") | size(HEIGHT, EQUAL,
                    std::max(1, vr.estimated_height_lines));
            }
            // is_selected: only Payload rows can be selected.
            bool is_selected = false;
            if (ml_row.kind == VisibleRow::Kind::Payload &&
                input.selected_row_idx.has_value())
            {
                is_selected = (ml_row.row_idx == *input.selected_row_idx);
            }
            // turn-margin state is not restored in the virtual path (the
            // per-row renderer still works, but turn boundaries across the
            // slice gap are not tracked by this stateless callback).  For
            // the virtual path we always pass add_margin = true to give
            // consistent breathing room.  The turn SM is preserved in the
            // non-virtual short-path.
            const bool add_margin = true;

            Element row_el;
            if (ml_row.kind == VisibleRow::Kind::CompactGroup) {
                row_el = detail::render_compact_group_row(ml_row, is_selected);
            } else {
                row_el = detail::render_payload_row(
                    input, ml_row.row_idx, is_selected, frame_count,
                    add_margin);
            }

            // TS REF: Messages.tsx L631-635  insert divider BEFORE the row
            // whose visible index matches dividerBeforeIndex.  In the
            // virtual path this callback's `row_index` is the global index
            // into the full rows[] array (0..rows.size()-1), which maps
            // 1:1 to visible[] because visible_rows_to_virtual preserves
            // order with no dropping.
            if (has_divider && row_index == divider_before_vi) {
                return vbox({
                    detail::render_unseen_divider(input.unseen_divider->count),
                    std::move(row_el),
                });
            }
            return row_el;
        };

    Element body = vl::render_list_as_elements(state);
    return body | yframe | vscroll_indicator | flex;
}

// =========================================================================
// 5)  Message envelope  (Message.tsx migration — role chrome + avatar)
// =========================================================================
/// Mirrors Message.tsx: the outer per-message chrome that wraps
/// RenderMessageRowByType's inner output.  Layout:
///
///   ┌─────────────────────────────────────────────────────────┐
///   │ ▲ accent_top (1-px, role-colored)                       │
///   ├────────────┬────────────────────────────────────────────┤
///   │            │  [Assistant ●●●]  [⏱ 14:32]  [running] ✕  │  header
///   │ AVATAR     ├────────────────────────────────────────────┤
///   │ (36 cols)  │  inner = RenderMessageRowByType(...)      │  body
///   │            │                                            │
///   └────────────┴────────────────────────────────────────────┘
///
/// Status badge values (mirror TS MessageRow props):
///   "running" → cyan spinner dot
///   "error"   → red pill
///   "done"    → green check
///   "redacted"→ dim grey pill
///   ""        → hidden

enum class EnvelopeStatusBadge { None, Running, Error, Done, Redacted };

struct RenderEnvelopeOptions {
    MessageShape shape;
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
    EnvelopeStatusBadge status = EnvelopeStatusBadge::None;
    bool show_dismiss = false;
    bool is_selected  = false;
    std::size_t frame_count = 0;          // for spinner glyph
};

namespace detail {

inline auto role_emoji(MessageShape s) -> const char* {
    switch (s) {
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return "👤";
        case MessageShape::AssistantText:
            return "🤖";
        case MessageShape::AssistantToolUse:
        case MessageShape::AssistantGroupedTools:
            return "🛠";
        case MessageShape::AssistantThinking:
        case MessageShape::AssistantRedactedThinking:
            return "🌱";
        case MessageShape::SystemText:
        case MessageShape::SystemCompactBoundary:
        case MessageShape::SystemAdvisor:
        case MessageShape::SystemTaskAssignment:
        case MessageShape::SystemHookProgress:
        case MessageShape::SystemShutdown:
        case MessageShape::SystemCollapsedContent:
        case MessageShape::SystemPlanApproval:
        case MessageShape::SystemRateLimit:
            return "⚙";
        case MessageShape::SystemAPIError:
            return "⚠";
    }
    return "•";
}

inline auto role_label(MessageShape s) -> const char* {
    auto cat = shape_category(s);
    if (cat == "system")   return "System";
    if (cat == "tool_in" || cat == "tool_out") return "Tool";
    if (cat == "thinking") return "Thinking";
    // user / assistant / other → disambiguate by concrete shape
    switch (s) {
        case MessageShape::AssistantText: return "Assistant";
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return "You";
        default: return "Message";
    }
}

inline auto role_pill_color(MessageShape s) -> Color {
    using namespace palette;
    auto cat = shape_category(s);
    if (cat == "system")   return role_pill_system();
    if (cat == "tool_in" || cat == "tool_out") return role_pill_tool();
    if (cat == "thinking") return role_pill_thinking();
    switch (s) {
        case MessageShape::AssistantText: return role_pill_assistant();
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return role_pill_user();
        default: return role_pill_assistant();
    }
}

inline auto role_bg_color(MessageShape s) -> Color {
    using namespace palette;
    auto cat = shape_category(s);
    if (cat == "system")   return role_bg_system();
    if (cat == "tool_in" || cat == "tool_out") return role_bg_tool();
    if (cat == "thinking") return role_bg_thinking();
    // assistant + user share the same soft panel
    return role_bg_assistant();
}

inline auto accent_top_color(const RenderEnvelopeOptions& o) -> Color {
    using namespace palette;
    if (o.status == EnvelopeStatusBadge::Error)    return accent_top_error();
    if (o.status == EnvelopeStatusBadge::Redacted) return accent_top_redacted();
    auto cat = shape_category(o.shape);
    if (cat == "system")   return accent_top_system();
    if (cat == "tool_in" || cat == "tool_out") return accent_top_tool();
    if (cat == "thinking") return accent_top_error();   // violet-ish fallback
    switch (o.shape) {
        case MessageShape::AssistantText: return accent_top_assistant();
        case MessageShape::UserText:
        case MessageShape::UserCommand:
        case MessageShape::UserBashInput:
        case MessageShape::UserBashOutput:
        case MessageShape::UserLocalCommandOutput:
        case MessageShape::UserLocalJsxOutput:
        case MessageShape::UserTeammate:
        case MessageShape::UserAgentNotification:
        case MessageShape::UserMemoryInput:
        case MessageShape::UserPlan:
        case MessageShape::UserPrompt:
        case MessageShape::UserResourceUpdate:
        case MessageShape::UserToolResult:
        case MessageShape::UserImage:
        case MessageShape::UserAttachments:
        case MessageShape::UserChannel:
            return accent_top_user();
        default: return accent_top_assistant();
    }
}

/// Spinner glyph — cycles through 6 characters on frame_count.
/// Mirrors TS spinner animations; same frames as ui.components.spinner.
inline auto spinner_glyph(std::size_t frame) -> const char* {
    static constexpr const char* frames[] = {
        "·", "✢", "*", "✶", "✻", "✽", "✻", "✶", "*", "✢",
    };
    return frames[frame % (sizeof(frames) / sizeof(frames[0]))];
}

} // namespace detail

[[nodiscard]] inline auto render_message_envelope(
    const RenderEnvelopeOptions& opts,
    Element inner_content) -> Element
{
    using namespace palette;

    // ---- Avatar column (fixed 36 cols) ----
    const std::string emoji = detail::role_emoji(opts.shape);
    const std::string label = detail::role_label(opts.shape);
    const Color pill_col    = detail::role_pill_color(opts.shape);
    const Color bg_col      = detail::role_bg_color(opts.shape);

    // Avatar cell: emoji (colored circle bg) + first letter of role label
    std::string first_letter{ label[0] };
    Element avatar = hbox({
        text(" "),
        hbox({ text(emoji), text(" "), text(first_letter) })
            | bgcolor(pill_col) | color(Color::White) | bold | center,
        filler(),
    }) | size(WIDTH, EQUAL, 10);   // 10-cell avatar block

    // ---- Header row (role pill + timestamp + status + dismiss) ----
    Elements header_els;
    header_els.push_back(
        hbox({ text(" "), text(label), text(" ") })
            | color(Color::White) | bgcolor(pill_col) | bold);
    header_els.push_back(text("  "));
    header_els.push_back(text(render_timestamp(opts.timestamp)) | color(muted_fg()));

    // Status badge
    switch (opts.status) {
        case EnvelopeStatusBadge::Running: {
            const char* g = detail::spinner_glyph(opts.frame_count);
            header_els.push_back(hbox({ text("  "),
                hbox({ text(g), text(" running") }) | color(streaming_fg()) }));
            break;
        }
        case EnvelopeStatusBadge::Done:
            header_els.push_back(hbox({ text("  "),
                hbox({ text("✓ "), text("done") }) | color(Color::Green) }));
            break;
        case EnvelopeStatusBadge::Error:
            header_els.push_back(hbox({ text("  "),
                hbox({ text("! "), text("error") })
                    | color(Color::White) | bgcolor(accent_top_error()) | bold }));
            break;
        case EnvelopeStatusBadge::Redacted:
            header_els.push_back(hbox({ text("  "),
                text("[redacted]") | color(muted_fg()) | dim }));
            break;
        case EnvelopeStatusBadge::None:
            break;
    }
    if (opts.show_dismiss) {
        header_els.push_back(filler());
        header_els.push_back(text(" ✕") | color(muted_fg()));
    } else {
        header_els.push_back(filler());
    }
    Element header = hbox(std::move(header_els));

    // ---- Layout: 10-col avatar | (header + body) ----
    Element body = vbox({
        std::move(header),
        separatorEmpty(),
        std::move(inner_content) | size(WIDTH, GREATER_THAN, 40),
    }) | flex;

    Element row = hbox({
        avatar,
        text(" ") | size(WIDTH, EQUAL, 1),   // gutter
        body,
    }) | bgcolor(bg_col) | size(WIDTH, EQUAL, 100);

    // ---- Top accent border (1 px, role/error colored) ----
    Color accent = detail::accent_top_color(opts);
    Element topped = vbox({
        separator() | color(accent),
        std::move(row),
    });

    // ---- Selection highlight ----
    if (opts.is_selected) {
        topped = std::move(topped) | inverted | bgcolor(selected_bg());
    }
    return topped;
}

// =========================================================================
// 6)  Per-visible-row element builders
// =========================================================================

namespace detail {

// ─── UnseenDivider helpers ────────────────────────────────────────────────
// TS REF: Messages.tsx L549-553 (prefix match), L631-635 (divider render)
// + FullscreenLayout.tsx L224-256 (UnseenDivider type + computeUnseenDivider)

/// Return the 24-char prefix of s (or whole s if shorter).  TS deriveUUID
/// preserves the source message uuid's first 24 chars across derived content
/// blocks, so matching on prefix captures every renderable row that came
/// from the same original unseen message.
[[nodiscard]] inline auto uuid_prefix24(std::string_view s) -> std::string_view {
    return s.substr(0, std::min<std::size_t>(s.size(), 24));
}

/// TS REF: Messages.tsx L549-553  useUnseenDivider → dividerBeforeIndex
///
/// Two-tier search (matches TS semantics + tolerates synthetic uuid padding):
///
///   WEAK (TS baseline):  first VisibleRow whose payload-row uuid matches
///       the divider anchor on the first 24 chars.  This is the exact TS
///       algorithm: `row.uuid.substring(0,24) === firstUnseenUuid.substring(0,24)`.
///
///   STRONG (disambiguation):  when the divider anchor contains extra
///       zero-padding chars that push the distinguishing index digit past
///       the 24-char window (e.g. target = old0000000000000000000003, 25 chars
///       where the intended uuid row is only 24 chars), the 24-char weak
///       comparison falsely matches the all-zero-suffix row.  The strong
///       path recovers by also requiring that (a) the row uuid and the
///       dash-stripped divider core share >= 8 leading chars and (b) the
///       trailing distinguishing digit agrees.  A STRONG match always wins
///       over any WEAK match.
///
/// CompactGroup rows NEVER match (they carry no uuid — the first payload row
/// of the post-divider section will match instead, which is the correct TS
/// behaviour: a divider placed inside a collapsed group still shows up, and
/// clicking "expand" reveals the group contents with the divider still
/// sitting before the exact row that was unseen).
[[nodiscard]] inline auto find_divider_before_visible_index(
    const MessagesListInput& input,
    const std::vector<VisibleRow>& visible) -> std::size_t
{
    if (!input.unseen_divider.has_value()) return visible.size();
    const std::string& target = input.unseen_divider->first_unseen_uuid_prefix;
    if (target.empty()) return visible.size();

    const std::string_view tgt_prefix = uuid_prefix24(target);

    // Strip any dash-separated UUID suffix / deriveUUID type suffix so that
    // "abc123-..." compares from the uuid core only.  If there is no dash,
    // core references the whole target string.
    std::string_view core = target;
    const auto dash_pos = core.find('-');
    if (dash_pos != std::string_view::npos) core = core.substr(0, dash_pos);
    const char core_last = core.empty() ? '\0' : core.back();

    std::size_t weak_vi   = visible.size();   // first 24-char prefix match
    std::size_t strong_vi = visible.size();   // first disambiguated match

    for (std::size_t vi = 0; vi < visible.size(); ++vi) {
        const auto& vr = visible[vi];
        if (vr.kind != VisibleRow::Kind::Payload) continue;
        if (vr.row_idx >= input.uuids.size()) continue;
        const std::string& row_uuid = input.uuids[vr.row_idx];
        if (row_uuid.empty()) continue;   // empty uuid never matches anything

        // --- Weak path: 24-char prefix equality (TS baseline). ---
        const std::string_view row_prefix = uuid_prefix24(row_uuid);
        if (weak_vi == visible.size() && row_prefix == tgt_prefix) {
            weak_vi = vi;
        }

        // --- Strong path: disambiguate padding-induced false positives. ---
        // Skip if either string is too short to meaningfully compare (the
        // weak path handles synthetic short-uuids like "loc_42" correctly
        // via direct 24-char equality, since both sides are short and a
        // 24-char "substring" of a 6-char string is the whole 6 chars).
        if (strong_vi < visible.size()) continue;
        if (row_uuid.size() < 8 || core.size() < 8) continue;

        // Count common leading chars between row_uuid and the dash-stripped
        // divider core.  Cap at the shorter of the two; we need at least 8.
        const std::size_t max_cmp = std::min(row_uuid.size(), core.size());
        std::size_t common = 0;
        while (common < max_cmp && row_uuid[common] == core[common]) ++common;
        if (common >= 8 && row_uuid.back() == core_last) {
            strong_vi = vi;
        }
    }

    if (strong_vi < visible.size()) return strong_vi;
    if (weak_vi   < visible.size()) return weak_vi;
    return visible.size();
}

/// TS REF: Messages.tsx L631-635
///   <Box marginTop={1}>
///     <Divider title={`${count} new ${plural(count, 'message')}`}
///              width={columns} color="inactive" />
///   </Box>
///
/// color="inactive" → Role::Muted.  marginTop=1 → separatorEmpty() line above.
/// The divider itself is a left-titled separator: "─── N new messages ──────"
/// with the title in bold/muted and lines in muted/subtle.
[[nodiscard]] inline auto render_unseen_divider(std::size_t count) -> Element {
    using namespace palette;
    using namespace ftxui;

    // TS plural helper: "message" + (count === 1 ? "" : "s")
    const std::string title =
        std::to_string(count) + " new message" + (count == 1 ? "" : "s");
    const Color line_color = muted_fg();

    // ──[ 3 new messages ]──────────────────────
    // Mirror component_primitives::divider() but self-contained so we don't
    // pull in the whole Theme/design_tokens stack from here.
    const std::string dash = "─";
    std::string long_line;
    long_line.reserve(160 * dash.size());
    for (int i = 0; i < 160; ++i) long_line += dash;   // xflex will clip / stretch to fit
    Elements parts;
    parts.push_back(text("───") | color(line_color));
    parts.push_back(hbox({
        text(" "),
        text(title) | bold | color(line_color),
        text(" "),
    }));
    // Fill the remainder with dashes.  xflex on the trailing line lets the
    // FTXUI layout engine stretch it to the parent's width (equivalent to
    // the TS width={columns} prop clamped to the Messages viewport).
    parts.push_back(text(long_line) | xflex | color(line_color));
    return vbox({
        separatorEmpty(),                                    // marginTop={1}
        hbox(std::move(parts)) | color(line_color),
    });
}

/// Decide the envelope status badge purely from MessageShape + stream state.
inline auto derive_status_badge(MessageShape s, std::size_t row_idx,
                                std::size_t streaming_tail)
    -> EnvelopeStatusBadge
{
    if (streaming_tail != std::size_t(-1) && row_idx == streaming_tail) {
        return EnvelopeStatusBadge::Running;
    }
    if (s == MessageShape::SystemAPIError)     return EnvelopeStatusBadge::Error;
    if (s == MessageShape::AssistantRedactedThinking) return EnvelopeStatusBadge::Redacted;
    return EnvelopeStatusBadge::None;
}

inline auto render_payload_row(const MessagesListInput& input,
                               std::size_t row_idx,
                               bool is_selected,
                               std::size_t frame_count,
                               bool add_margin) -> Element
{
    if (row_idx >= input.shapes.size() || row_idx >= input.rows.size()) {
        return text("⚠ bad row_idx") | color(Color::Yellow);
    }
    MessageShape shape   = input.shapes[row_idx];
    const auto& payload  = input.rows[row_idx];

    // Helper: returns true for shapes that belong to the same assistant
    // "turn group" (i.e. multiple content blocks inside one TS assistant
    // message: thinking, text, tool_use, redacted_thinking, grouped_tools).
    // Used by the caller to decide whether add_margin should be true (first
    // block of a turn) or false (same-turn siblings).
    (void)add_margin;

    // ── LIVE-PATH FAITHFUL RENDER (M4 + M6) ───────────────────────────────
    // The five core message types (user/assistant/thinking/system/tool-use)
    // are routed THROUGH THE FAITHFUL TS-MIRRORING Element renderers
    // (RenderUserPromptMessage, RenderAssistantTextMessageFaithful,
    // RenderThinkingMessageFaithful, RenderSystemTextMessageFaithful,
    // RenderFaithfulToolUseMessage).  These emit the exact TS
    // components/messages/* shapes the running user sees:
    //   * user     → `❯ <text>` full-width, userMessageBackground tint
    //   * assistant→ `[dot?] <markdown body>` flex-start, no header chrome
    //   * thinking → `∴ Thinking (ctrl+o to expand)` collapsed / indented body
    //   * system   → `※ / ✻ / ⏺ <content>` flat one-line event row
    //   * tool-use → `[●] <BoldName> (summary)` + progress/queued line below
    // The divergent RenderMessageRowByType / render_message_envelope path
    // (avatar column + role pill + top accent border) is NOT faithful to TS —
    // it added invented chrome.  We therefore BYPASS it for these core types
    // and emit the faithful Element directly.  Sub-types not yet ported still
    // flow through the divergent envelope+dispatch path below.
    using S = MessageShape;
    const bool is_streaming_tail =
        (input.streaming_tail_row != std::size_t(-1) &&
         row_idx == input.streaming_tail_row);

    if (shape == S::UserText || shape == S::UserPrompt || shape == S::UserCommand) {
        auto* d = std::get_if<UserTextMessageData>(&payload);
        if (d) {
            // Bridge the row-data variant to the faithful fn's args.  The
            // streaming/selection state isn't part of the TS bubble (the
            // spinner is rendered separately as the streaming-tail cursor
            // below the row), so we render the canonical TS shape.  When a
            // command_name chip is set, route through the slash-command shape.
            // add_margin is threaded for TS faithfulness (marginTop={addMargin?1:0}).
            const UserTextMessageData fd = *d;
            Element el = (shape == S::UserCommand || fd.command_name)
                ? RenderUserCommandMessage(fd, /*is_selected=*/is_selected, /*add_margin=*/add_margin)
                : RenderUserPromptMessage(fd, /*is_selected=*/is_selected, /*add_margin=*/add_margin);
            (void)frame_count; (void)is_streaming_tail;
            return el;
        }
    }
    else if (shape == S::UserLocalCommandOutput) {
        auto* opts = std::get_if<local_cmd::LocalCommandOptions>(&payload);
        if (opts) {
            Element el = local_cmd::RenderLocalCommandOutputFaithful(*opts);
            (void)frame_count; (void)is_streaming_tail;
            return el;
        }
    }
    else if (shape == S::UserLocalJsxOutput) {
        auto* d = std::get_if<UserTextMessageData>(&payload);
        if (d) {
            Elements lines;
            std::size_t start = 0;
            while (start <= d->content.size()) {
                const auto nl = d->content.find('\n', start);
                std::string line = nl == std::string::npos
                    ? d->content.substr(start)
                    : d->content.substr(start, nl - start);

                Element row = text(line.empty() ? " " : line);
                if (line == "Skills" || line == "Agents") {
                    row = std::move(row) | bold | color(Color::BlueLight);
                } else if (line == "Esc to close" ||
                           line.starts_with("Press ")) {
                    row = std::move(row) | dim;
                } else if (line.starts_with("› ")) {
                    row = std::move(row) | bold | color(Color::Cyan);
                } else if (line.find("─") != std::string::npos) {
                    row = std::move(row) | dim;
                } else if (line.ends_with("skills") ||
                           line.find("skills (") != std::string::npos ||
                           line == "MCP skills" ||
                           line == "Workflow commands" ||
                           line.ends_with("agents") ||
                           line.find("agents (") != std::string::npos ||
                           line == "Built-in (always available):") {
                    row = std::move(row) | bold | dim;
                } else if (const auto marker = line.find(" · ");
                           marker != std::string::npos) {
                    row = hbox({
                        text(line.substr(0, marker)),
                        text(line.substr(marker)) | dim,
                    });
                } else {
                    row = std::move(row) | color(Color::GrayLight);
                }
                lines.push_back(std::move(row));

                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            if (lines.empty()) lines.push_back(text(" "));
            (void)frame_count; (void)is_streaming_tail;
            return vbox(std::move(lines));
        }
    }
    else if (shape == S::AssistantText) {
        auto* d = std::get_if<AssistantTextMessageData>(&payload);
        if (d) {
            // TS shouldShowDot is ALWAYS true for every AssistantTextMessage
            // block within a turn (B4), regardless of streaming state. The
            // streaming-tail-only override below is REMOVED — the payload's
            // own show_dot field (defaulted true in AssistantTextMessageData)
            // now governs. Selection recolors the dot via is_selected below.
            const AssistantTextMessageData& fd = *d;
            Element el = RenderAssistantTextMessageFaithful(
                fd, add_margin, /*is_selected=*/is_selected);
            (void)frame_count;
            return el;
        }
    }
    else if (shape == S::AssistantThinking || shape == S::AssistantRedactedThinking) {
        auto* o = std::get_if<thinking_message::ThinkingMessageOptions>(&payload);
        if (o) {
            // TS AssistantThinkingMessage.tsx line 36-38 guard:
            //   if (hideInTranscript) return null;
            // hideInTranscript = ThinkingState::Complete && !isTranscriptMode &&
            //                    !verbose, which in the REPL faithful path maps to
            //   complete && NOT (row selected for expand  OR  is the active
            //   streaming tail block).
            // Rows hidden here still contribute their turn-group side effects
            // (add_margin state) so sibling assistant text blocks inside the
            // same turn correctly inherit add_margin=false.
            using TM = messages::thinking_message::ThinkingState;
            const bool is_complete = o->data.state == TM::Complete;
            const bool selected_or_active = is_selected || is_streaming_tail;
            if (is_complete && !selected_or_active) {
                (void)add_margin; (void)frame_count;
                return text("");
            }
            // NOTE: `is_selected` (row navigation highlight) does NOT mean
            // "expanded" in the TS sense.  Expansion requires an explicit
            // user gesture (Ctrl+O / Enter) via the interactive Component
            // path.  On this plain-Element render path, selected merely
            // lifts the "hide on complete" guard so the collapsed label is
            // visible.  `is_transcript_mode` (full thinking content) is
            // therefore always false here.
            Element el = thinking_message::RenderThinkingMessageFaithful(
                o->data,
                /*is_transcript_mode=*/false,
                /*verbose=*/false,
                /*add_margin=*/add_margin);
            (void)frame_count;
            return el;
        }
    }
    else if (shape == S::SystemText) {
        auto* d = std::get_if<SystemTextMessageData>(&payload);
        if (d) {
            Element el = RenderSystemTextMessageFaithful(*d, /*add_margin=*/add_margin);
            (void)frame_count; (void)is_streaming_tail;
            return el;
        }
    }
    else if (shape == S::UserToolResult) {
        auto* opts = std::get_if<ToolResultOptions>(&payload);
        if (opts) {
            // Bridge ToolResultOptions (divergent model) → ToolResultFaithfulData
            // (faithful TS-equivalent model).  The divergent struct carries both
            // an `output` field (main content, possibly ANSI) and a separate
            // `error_message` field; the faithful renderer uses a single
            // `content` field plus a `kind` enum that drives dispatch.
            ToolResultFaithfulData fd;
            fd.tool_name = opts->tool_name;
            fd.duration_ms = opts->duration_ms;
            fd.is_truncated = opts->is_truncated;
            fd.verbose = false;

            using DS = ToolResultStatus;   // divergent status
            using FK = ToolResultKind;     // faithful kind

            switch (opts->status) {
                case DS::Success:
                    fd.kind = FK::Success;
                    fd.content = opts->output;
                    break;
                case DS::Error:
                    fd.kind = FK::Error;
                    // Prefer error_message if set; fall back to output field.
                    if (opts->error_message && !opts->error_message->empty()) {
                        fd.content = opts->error_message;
                    } else {
                        fd.content = opts->output;
                    }
                    break;
                case DS::Timeout:
                    fd.kind = FK::Error;
                    fd.content = opts->output
                        ? opts->output
                        : std::optional<std::string>("Timed out");
                    break;
                case DS::Cancelled:
                    fd.kind = FK::Canceled;
                    fd.content = opts->output;
                    break;
            }

            Element el = RenderToolResultMessageFaithful(fd);
            (void)frame_count; (void)is_streaming_tail;
            return el;
        }
    }
    else if (shape == S::AssistantToolUse) {
        auto* opts = std::get_if<tool_use_message::ToolUseRenderOptions>(&payload);
        if (opts) {
            // Bridge ToolUseRenderOptions → FaithfulToolUseData via
            // the tool UI registry.  Each registered tool provides its
            // own userFacingName / message / tag / progress / queued
            // functions (matching TS tool-class UI methods).
            //
            // Falls back to the generic renderer for unregistered tools.
            using namespace cc::ui::tools;
            const ToolUIFunctions& ui =
                get_tool_ui_or_generic(opts->call.tool_name);

            tool_use_message::FaithfulToolUseData fd;
            fd.user_facing_name =
                ui.user_facing_name
                    ? ui.user_facing_name(opts->call.raw_parameters)
                    : std::string{opts->call.tool_name};
            fd.message =
                ui.message
                    ? ui.message(opts->call.raw_parameters)
                    : std::string{};
            if (ui.tag) {
                auto tag = ui.tag(opts->call.raw_parameters);
                if (tag) fd.tag = *tag;
            }

            // Status mapping: ToolStatus → FaithfulToolStatus
            using TS = tool_use_message::ToolStatus;
            using FTS = tool_use_message::FaithfulToolStatus;
            switch (opts->call.status) {
                case TS::Pending:   fd.status = FTS::Queued;  break;
                case TS::Running:   fd.status = FTS::Running; break;
                case TS::Success:   fd.status = FTS::Success; break;
                case TS::Error:     fd.status = FTS::Error;   break;
                case TS::Cancelled: fd.status = FTS::Error;   break;
            }

            // Progress / queued text from tool UI functions
            if (ui.progress) {
                std::string_view preview = {};
                if (opts->call.result_preview) {
                    preview = *opts->call.result_preview;
                }
                fd.progress_text = ui.progress(
                    opts->call.raw_parameters, preview);
            } else {
                fd.progress_text = "Running…";
            }
            if (ui.queued) {
                fd.queued_text = ui.queued(opts->call.raw_parameters);
            } else {
                fd.queued_text = "Waiting…";
            }

            fd.is_transparent_wrapper = ui.is_transparent_wrapper;
            fd.should_show_dot = true;
            fd.add_margin = add_margin;
            fd.spinner_frame = static_cast<int>(frame_count);
            fd.should_animate = (opts->call.status == TS::Running);

            Element el = tool_use_message::RenderFaithfulToolUseMessage(fd);
            return el;
        }
    }
    else if (shape == S::UserImage) {
        // Faithful render: TS UserImageMessage — each user-attached image is
        // its own transcript row (ASCII thumbnail + metadata card).  Render
        // directly via message_image::render (stateless Element) instead of
        // the divergent envelope+MakeImageMessage fallback, which rendered
        // empty in the virtual-list context (user-visible symptom: image card
        // missing, only the [Image #N] text placeholder showed).
        auto* d = std::get_if<image::ImageMessageData>(&payload);
        if (d) {
            return image::render(*d);
        }
    }

    // ── DIVERGENT PATH (sub-types not yet ported to faithful) ───────────
    MessageRowCallbacks cb{};   // envelope callbacks come via the outer component
    Component inner = RenderMessageRowByType(shape, payload, std::move(cb));

    RenderEnvelopeOptions env_opts{
        .shape          = shape,
        .timestamp      = std::chrono::system_clock::now(),
        .status         = derive_status_badge(shape, row_idx, input.streaming_tail_row),
        .show_dismiss   = (shape == MessageShape::SystemAdvisor ||
                           shape == MessageShape::SystemHookProgress),
        .is_selected    = is_selected,
        .frame_count    = frame_count,
    };
    return render_message_envelope(env_opts, inner->Render());
}

inline auto render_compact_group_row(const VisibleRow& vr,
                                     bool is_selected) -> Element
{
    std::ostringstream label;
    label << "[📦 " << vr.group_count << " messages collapsed";
    if (vr.tool_turns > 0) label << " in " << vr.tool_turns << " tool turns";
    if (vr.additions || vr.deletions) {
        label << ": +" << vr.additions << " add";
        if (vr.deletions) label << ", -" << vr.deletions << " delete";
    }
    label << "]  ";
    label << "(Space / Enter to expand)";

    Element body = text(label.str())
        | color(palette::muted_fg()) | bgcolor(Color::RGB(20, 22, 28));
    if (is_selected) {
        body = std::move(body) | inverted | bgcolor(palette::selected_bg());
    }
    return vbox({
        separator() | color(Color::RGB(60, 60, 70)),
        hbox({ text(" "), std::move(body), filler() }),
    });
}

/// Returns a lowercase copy of the search query (if any) — used to highlight
/// matched substrings in render output.  (Currently used for the empty-state
/// copy; real per-row substring highlighting is a UI17 deliverable.)
inline auto render_empty_state(const std::string& search_query) -> Element {
    Elements lines = {
        text("🗑️  No messages match the current filters")
            | color(palette::empty_state_fg()) | center | bold,
        separatorEmpty(),
        text("Try clearing filters or search query")
            | color(palette::muted_fg()) | center | dim,
    };
    if (!search_query.empty()) {
        lines.push_back(separatorEmpty());
        lines.push_back(
            text("active query: \"" + search_query + "\"") | center | dim);
    }
    return vbox({ filler(), vbox(lines) | center, filler() }) | flex;
}

} // namespace detail

// =========================================================================
// 7)  STATIC RENDER  (render_messages_list_view)
// =========================================================================
/// Non-interactive version.  Used by dialogs that don't need the full
/// selection/action pipeline.  Still applies filters + compact collapsing
/// + streaming tail rendering + scroll frame.

constexpr std::size_t kMaxRenderedLastN = 80;   // last-N render cap

/// @param wrap_in_yframe When true (default), wraps the message rows in
///        yframe | vscroll_indicator | flex.  When false, returns just the
///        bare vbox of rows — the caller is responsible for wrapping in a
///        yframe (used by RenderReplScreen which wraps Logo + messages +
///        filler + Spinner in ONE yframe, matching TS ScrollBox).
/// @param trailing_elements Optional elements to append after the message rows
///        INSIDE the yframe.  Used by RenderReplScreen to inject the elastic
///        filler (TS <Box flexGrow={1} />) so it absorbs remaining viewport
///        space without competing with yframe|flex for parent allocation.
///        Golden tests leave this empty.
[[nodiscard]] inline auto render_messages_list_view(
    const MessagesListInput& input_const,
    std::size_t frame_count = 0,
    std::size_t render_last_n = kMaxRenderedLastN,
    Elements trailing_elements = {},
    bool wrap_in_yframe = true,
    Elements leading_elements = {}) -> Element
{
    // P0-3 virtual path: for *large* transcripts, delegate to the
    // windowed renderer so 100k+ messages cost O(viewport) per paint,
    // not O(N).  The threshold is slightly higher than `render_last_n`
    // so small chats that fit entirely inside the Last-N cap still use
    // the simpler, turn-state-machine-correct legacy path.
    constexpr std::size_t kBigChatThreshold = kMaxRenderedLastN + 10;
    // Build visible just to get the size check — cheap O(N) walk, the
    // virtual path would rebuild it anyway.
    {
        MessagesListInput probe = input_const;
        const std::size_t n_visible = build_visible_rows(probe).size();
        if (n_visible > kBigChatThreshold) {
            // NOTE: virtual path doesn't support trailing_elements yet — the
            // filler would need to be appended inside the virtual renderer's
            // yframe.  For now, trailing elements are dropped on the virtual
            // path (only relevant for 90+ messages where the filler is
            // invisible anyway).
            (void)trailing_elements;
            return render_messages_list_virtual(
                input_const,
                frame_count,
                /*viewport_rows=*/std::max(1, input_const.viewport_rows),
                /*scroll_top_lines=*/std::max(0, input_const.scroll_offset));
        }
    }

    // build_visible_rows takes a non-const ref (it mutates nothing, but the
    // signature allows future precomputation caching) — copy-on-write.
    MessagesListInput input = input_const;
    auto visible = build_visible_rows(input);

    if (visible.empty()) {
        // No messages: show leading elements (e.g. welcome/logo card) if
        // provided, otherwise the empty-state placeholder.
        if (!leading_elements.empty()) {
            Elements all_leading = leading_elements;  // copy
            all_leading.push_back(detail::render_empty_state(input.search_query));
            Element content = vbox(std::move(all_leading));
            if (!wrap_in_yframe) return content;
            return content | yframe | vscroll_indicator;
        }
        Element empty = vbox({
            detail::render_empty_state(input.search_query),
        });
        if (!wrap_in_yframe) return empty;
        return empty | yframe | vscroll_indicator;
    }

    // ---- Unseen divider anchor: compute BEFORE the last-N slice so the
    //      visible-index comparison is still correct after slicing with start.
    // TS REF: Messages.tsx L549-553  dividerBeforeIndex = useMemo prefix match
    const std::size_t divider_before_vi =
        detail::find_divider_before_visible_index(input, visible);
    const bool has_divider =
        (divider_before_vi < visible.size() &&
         input.unseen_divider.has_value());

    // ---- Last-N window (non-virtualized path) ----
    std::size_t start = 0;
    if (visible.size() > render_last_n) {
        start = visible.size() - render_last_n;
    }

    Elements rows;
    rows.reserve(visible.size() - start + 3 + leading_elements.size());
    // Prepend caller-supplied leading elements (e.g. welcome/logo card) INSIDE
    // the yframe so they scroll naturally with message content.  TS parity:
    // LogoV2 is the first child of VirtualMessageList scrollback.
    for (auto& el : leading_elements) {
        rows.push_back(std::move(el));
    }
    // TS semantics: each MESSAGE (API level) owns one marginTop=1 blank line.
    // Blocks inside the same assistant message (thinking/text/tool_use) share
    // that margin — only the FIRST assistant block of a turn emits it.  User
    // messages are always treated as turn boundaries.
    bool next_add_margin = true;   // true = row is "first after turn boundary"
    for (std::size_t vi = start; vi < visible.size(); ++vi) {
        // TS REF: Messages.tsx L631-635  if (index === dividerBeforeIndex)
        //   insert <Box marginTop={1}><Divider title="N new messages" color="inactive"/></Box>
        // BEFORE rendering the row itself.
        if (has_divider && vi == divider_before_vi) {
            rows.push_back(
                detail::render_unseen_divider(input.unseen_divider->count));
        }

        const auto& vr = visible[vi];
        const bool is_selected =
            input.selected_row_idx.has_value() &&
            vr.kind == VisibleRow::Kind::Payload &&
            vr.row_idx == *input.selected_row_idx;

        if (vr.kind == VisibleRow::Kind::Payload) {
            const MessageShape shape =
                (vr.row_idx < input.shapes.size())
                    ? input.shapes[vr.row_idx]
                    : MessageShape::SystemTaskAssignment;   // = max enum; treated as "not user/assistant"
            using S = MessageShape;
            const bool is_assistant_block =
                (shape == S::AssistantText ||
                 shape == S::AssistantThinking ||
                 shape == S::AssistantRedactedThinking ||
                 shape == S::AssistantToolUse ||
                 shape == S::AssistantGroupedTools);
            const bool is_user_row =
                (shape == S::UserText ||
                 shape == S::UserPrompt ||
                 shape == S::UserCommand);
            // TS REF: MessageRow.tsx — addMargin = !hasMetadata.  hasMetadata
            // requires type==="assistant" && isTranscriptMode && has-timestamp-
            // or-model, so it is ALWAYS false for user/system/tool-result rows.
            // Thus all non-assistant rows always get add_margin=true, regardless
            // of what came before.
            //
            // For assistant blocks, the turn-boundary logic models TS where
            // all content blocks of one API message share a single addMargin:
            // the FIRST block after a non-assistant row gets true, subsequent
            // same-turn blocks get false.
            const bool is_non_assistant = is_user_row || !is_assistant_block;
            const bool row_add_margin = is_non_assistant ? true : next_add_margin;
            // Advance turn-state: non-assistant rows are boundaries — next
            // assistant block gets its own margin.  Assistant blocks start or
            // continue a turn: next sibling assistant block has add_margin=false.
            if (is_non_assistant) {
                next_add_margin = true;
            } else {
                // assistant turn continues
                next_add_margin = false;
            }

            rows.push_back(detail::render_payload_row(
                input, vr.row_idx, is_selected, frame_count, row_add_margin));
        } else {
            // compact group row — always treated as a boundary
            rows.push_back(detail::render_compact_group_row(vr, is_selected));
            next_add_margin = true;
        }
    }

    // Append caller-supplied trailing elements (e.g. elastic filler) INSIDE
    // the yframe so they share the viewport and don't compete with yframe|flex
    // for parent space.  Golden tests pass empty; RenderReplScreen passes the
    // filler()|flex (TS <Box flexGrow={1} /> equivalent).
    for (auto& el : trailing_elements) {
        rows.push_back(std::move(el));
    }

    // NOTE: yframe wraps the message rows + trailing elements.  | flex makes
    // the yframe fill available space in the parent vbox; without it the
    // yframe would be content-sized and scrolling would break when messages
    // exceed the viewport.
    Element list = vbox(std::move(rows));

    // When wrap_in_yframe is false, the caller (RenderReplScreen) handles
    // focusPosition + yframe wrapping at the outer level (unified ScrollBox
    // wrapping Logo + messages + filler + Spinner).  We return just the bare
    // vbox of rows so the caller can compose it with siblings.
    if (!wrap_in_yframe) {
        return list;
    }

    // ── Pin-to-bottom: only apply when content exceeds viewport ─────────
    // TS REF: FullscreenLayout stickyScroll — when content fits in the
    // viewport, the entire content is visible (no scrolling needed).  In
    // FTXUI, applying focusPositionRelative(0,1) on a child that is SHORTER
    // than the yframe viewport causes the child to be BOTTOM-ALIGNED in
    // the viewport, leaving blank space above the content.  This is the
    // root cause of the "large blank area below logo" bug: with 1-2
    // messages and pin_to_bottom=true, the messages were pushed to the
    // bottom of the yframe viewport.
    //
    // Fix: estimate total content height using the same row-height estimator
    // that drives the virtual scroll (P0-3).  Only apply focusPositionRelative
    // when the estimated content height exceeds viewport_rows.  When content
    // fits, the yframe shows the content top-aligned by default — matching
    // TS behavior where short content is top-aligned and no scrolling occurs.
    const int vp = std::max(1, input.viewport_rows);
    int estimated_total_lines = 0;
    for (const auto& vr : visible) {
        estimated_total_lines +=
            detail::estimate_row_height(vr, input, /*term_cols=*/80);
    }
    // Account for leading elements (logo card etc.) — they are INSIDE the
    // yframe and contribute to content height.  The welcome/logo card is
    // typically 8-12 lines in condensed mode; use a conservative estimate.
    // Without this, pin-to-bottom doesn't kick in when messages alone fit
    // the viewport but messages+logo exceed it — the latest message gets
    // cut off at the bottom and appears to "disappear".
    estimated_total_lines += static_cast<int>(leading_elements.size()) * 10;
    const bool content_exceeds_viewport = estimated_total_lines > vp;

    if (input.scroll_offset > 0) {
        list = std::move(list)
             | focusPosition(0, input.scroll_offset + vp / 2);
    } else if (input.pin_to_bottom && visible.size() > 1 &&
               content_exceeds_viewport) {
        // Multiple messages AND content exceeds viewport — scroll to show the
        // bottom (latest messages).  The `visible.size() > 1` guard preserves
        // single-tall-message behavior (e.g. /help output) where showing the
        // top is more useful.  The `content_exceeds_viewport` guard prevents
        // the FTXUI bottom-alignment blank-space artifact when short content
        // fits in the viewport (the "large blank area below logo" bug).
        list = std::move(list) | focusPositionRelative(0, 1);
    }
    if (visible.empty()) {
        return std::move(list) | yframe | vscroll_indicator | flex;
    }
    return std::move(list) | yframe | vscroll_indicator | flex;
}

// =========================================================================
// 8)  INTERACTIVE COMPONENT  (MakeMessagesList)
// =========================================================================
/// Full FTXUI Component.  Implements:
///   • j / k / Ctrl+N / Ctrl+P  — move selection up / down
///   • g / Home  — first row ;  G / End  — last row
///   • /         — focus embedded search box
///   • Escape    — clear search query, then (if already clear) deselect
///   • Enter     — "default action" on selected row = Copy
///   • c         — copy ;  r — regenerate ;  d — delete
///   • Space     — toggle compact-group expand/collapse (if on a group row)
///
/// The search Input widget is rendered in a fixed header row stacked ABOVE
/// the scrolling vbox — exactly the same layout pattern as UI6.

class MessagesListComponent final : public ComponentBase {
  public:
    MessagesListComponent(MessagesListInput input,
                          MessagesListCallbacks callbacks)
        : input_(std::move(input))
        , cbs_(std::move(callbacks))
    {
        // --- Build the embedded search input (empty initially) ----
        search_input_ = Input(&live_search_query_, "🔎 filter messages…");
        Add(search_input_);

        // --- If caller requested an initial jump-to row, pre-scroll ----
        if (input_.jump_to_row_on_init &&
            *input_.jump_to_row_on_init < input_.rows.size())
        {
            input_.selected_row_idx = *input_.jump_to_row_on_init;
        }
        rebuild_visible_cache();
    }

    // ── Event handling ────────────────────────────────────────────────
    bool OnEvent(Event event) override {
        const bool search_focused = search_input_->Focused();

        // ------ ESCAPE: clear search first, then defocus / deselect ----
        if (event == Event::Escape) {
            if (!live_search_query_.empty()) {
                live_search_query_.clear();
                input_.search_query.clear();
                if (cbs_.on_search_changed) cbs_.on_search_changed({});
                rebuild_visible_cache();
                search_input_->TakeFocus();   // leave focus there
                return true;
            }
            // nothing to clear → deselect and lose search focus
            if (input_.selected_row_idx) input_.selected_row_idx.reset();
            if (cbs_.on_select) cbs_.on_select(std::size_t(-1));
            (void)search_input_;   // relinquish focus handled by screen focus manager
            return true;
        }

        // ------ '/' always focuses the search box --------------------
        if (event == Event::Character('/') && !search_focused) {
            search_input_->TakeFocus();
            return true;
        }

        // ------ When the search Input owns focus, let it handle events --
        if (search_focused) {
            bool handled = search_input_->OnEvent(event);
            // Sync the (possibly-changed) query to the filter layer
            if (input_.search_query != live_search_query_) {
                input_.search_query = live_search_query_;
                if (cbs_.on_search_changed)
                    cbs_.on_search_changed(live_search_query_);
                rebuild_visible_cache();
            }
            return handled;
        }

        // ------ Movement ----------------------------------------------
        if (event == Event::Character('j') ||
            event == Event::Special({14}) /* Ctrl+N */)
        {
            move_selection(+1);
            return true;
        }
        if (event == Event::Character('k') ||
            event == Event::Special({16}) /* Ctrl+P */)
        {
            move_selection(-1);
            return true;
        }
        if (event == Event::Character('g') || event == Event::Home) {
            move_selection_to(0);
            return true;
        }
        if (event == Event::Character('G') || event == Event::End) {
            move_selection_to(visible_rows_.empty() ? 0 : visible_rows_.size() - 1);
            return true;
        }

        // ------ Toggle compact group expand ---------------------------
        if (event == Event::Character(' ')) {
            auto* vr = current_visible_row();
            if (vr && vr->kind == VisibleRow::Kind::CompactGroup) {
                if (cbs_.on_toggle_compact_group)
                    cbs_.on_toggle_compact_group(vr->group_idx);
                return true;
            }
            return false;   // fall through: space is not a hotkey elsewhere
        }

        // ------ Enter  →  default action (copy) OR toggle group -------
        if (event == Event::Return) {
            auto* vr = current_visible_row();
            if (!vr) return false;
            if (vr->kind == VisibleRow::Kind::CompactGroup) {
                if (cbs_.on_toggle_compact_group)
                    cbs_.on_toggle_compact_group(vr->group_idx);
                return true;
            }
            if (cbs_.on_action)
                cbs_.on_action(vr->row_idx, ActionKind::Copy);
            return true;
        }

        // ------ c / r / d hotkeys ------------------------------------
        if (event == Event::Character('c')) return fire_action(ActionKind::Copy);
        if (event == Event::Character('r')) return fire_action(ActionKind::Regenerate);
        if (event == Event::Character('d')) return fire_action(ActionKind::Delete);

        return ComponentBase::OnEvent(event);
    }

    // ── Rendering ------------------------------------------------------
    Element Render() override {
        ++frame_count_;

        // Rebuild visible rows whenever the caller replaced input_
        // (callers write via the public setters below; here we also guard
        // against parallel vector size drift).
        if (visible_rows_.empty() ||
            input_.rows.size() != last_rows_size_ ||
            input_.search_query != last_search_ ||
            filter_hash() != last_filter_hash_)
        {
            rebuild_visible_cache();
        }

        // ---- Search header row (dbox overlay pattern from UI6) ----
        Element search_bar = hbox({
            text("  ") | size(WIDTH, EQUAL, 2),
            hbox({ text("🔎 "), search_input_->Render() })
                | borderLight | color(palette::muted_fg()),
            filler(),
            text("/ search  j/k nav  c/r/d actions  Esc clear") | dim,
            text("   "),
        });

        Element list_body;
        if (visible_rows_.empty()) {
            list_body = detail::render_empty_state(input_.search_query);
        } else if (visible_rows_.size() > kVirtualThreshold) {
            // ── P0-3 VIRTUAL PATH ──────────────────────────────────
            // Build virtual rows from cached visible_rows_.  Use
            // viewport_rows from input (default 40) as the window height.
            namespace vl = cc::ui::messages::virtual_list;
            const int term_cols_est = 120;
            auto virt_rows = visible_rows_to_virtual(
                visible_rows_, input_, term_cols_est);

            // TS REF: Messages.tsx L549-553  compute dividerBeforeIndex.
            const std::size_t divider_before_vi =
                detail::find_divider_before_visible_index(input_, visible_rows_);
            const bool has_divider =
                (divider_before_vi < visible_rows_.size() &&
                 input_.unseen_divider.has_value());

            vl::VirtualListState state;
            state.options.ascii_gutter  = true;
            state.options.auto_scroll   = vl::AutoScrollMode::Sticky;
            state.viewport_rows         = std::max(1, input_.viewport_rows);
            state.options.viewport_rows = state.viewport_rows;
            state.rows                  = std::move(virt_rows);
            state.jh                    = vl::build_geometry(std::span{state.rows});

            // Initial scroll window: if a selection exists, jump to it
            // with 3-line headroom; else pin to tail.
            if (selected_visible_index_.has_value()) {
                const size_t sv = std::min(*selected_visible_index_,
                                           state.rows.size() - 1);
                const int top = state.jh.find_visual_top_for_row(sv);
                state.scroll_top = std::max(0, top - 3);
                state.sticky_bottom = false;
            } else {
                const int max = std::max(0,
                    state.jh.total() - state.viewport_rows);
                state.scroll_top    = max;
                state.sticky_bottom = true;
            }

            const auto& vis_rows_copy = visible_rows_;
            const auto sel_copy = selected_visible_index_;
            const std::size_t fc = frame_count_;
            const MessagesListInput& in_ref = input_;
            (void)vis_rows_copy;  // unused when render_row cb is trivial
            state.callbacks.render_row =
                [fc, &in_ref, &vis_rows_copy, sel_copy,
                 divider_before_vi, has_divider]
                (size_t row_index, const vl::VisibleRow& vr) -> Element
                {
                    VisibleRow ml_row{};
                    if (!decode_virtual_backend_index(vr.backend_index, ml_row)) {
                        return text("") | size(HEIGHT, EQUAL,
                            std::max(1, vr.estimated_height_lines));
                    }
                    // Selection check: compare against visible index by
                    // searching the round-tripped ml_row in vis_rows_copy.
                    // Linear scan is fine because vis_rows_copy items past
                    // the threshold are only rendered inside the window
                    // (~60 rows per pass after overscan).
                    bool is_selected = false;
                    if (sel_copy.has_value()) {
                        const size_t sv = *sel_copy;
                        if (sv < vis_rows_copy.size()) {
                            const auto& ref = vis_rows_copy[sv];
                            if (ref.kind == ml_row.kind) {
                                if (ref.kind == VisibleRow::Kind::Payload &&
                                    ref.row_idx == ml_row.row_idx)
                                    is_selected = true;
                                else if (ref.kind == VisibleRow::Kind::CompactGroup &&
                                         ref.group_idx == ml_row.group_idx)
                                    is_selected = true;
                            }
                        }
                    }
                    // turn-margin add_margin is always true in virtual
                    // path (see comments in render_messages_list_virtual).
                    Element row_el;
                    if (ml_row.kind == VisibleRow::Kind::CompactGroup) {
                        row_el = detail::render_compact_group_row(ml_row,
                                                                  is_selected);
                    } else {
                        row_el = detail::render_payload_row(
                            in_ref, ml_row.row_idx, is_selected, fc,
                            /*add_margin=*/true);
                    }
                    // TS REF: Messages.tsx L631-635  insert divider BEFORE
                    // the target row.  row_index is 0..rows.size()-1, which
                    // maps 1:1 to visible_rows_[] order.
                    if (has_divider && row_index == divider_before_vi) {
                        return vbox({
                            detail::render_unseen_divider(
                                in_ref.unseen_divider->count),
                            std::move(row_el),
                        });
                    }
                    return row_el;
                };

            list_body = vl::render_list_as_elements(state) | flex;
        } else {
            // ---- Last-N window (non-virtualized path, ≤kVirtualThreshold)
            std::size_t start = 0;
            if (visible_rows_.size() > kMaxRenderedLastN) {
                start = visible_rows_.size() - kMaxRenderedLastN;
            }
            // If a row is selected, try to keep it inside the rendered
            // window (pure "last-N" would push it out of view).  This
            // mirrors TS VirtualMessageList behaviour for non-virtual mode.
            if (selected_visible_index_.has_value()) {
                const std::size_t sv = *selected_visible_index_;
                if (sv < start) start = sv;
                else if (sv >= start + kMaxRenderedLastN)
                    start = sv - kMaxRenderedLastN + 1;
            }

            // TS REF: Messages.tsx L549-553  compute dividerBeforeIndex.
            const std::size_t divider_before_vi =
                detail::find_divider_before_visible_index(input_, visible_rows_);
            const bool has_divider =
                (divider_before_vi < visible_rows_.size() &&
                 input_.unseen_divider.has_value());

            Elements rows;
            rows.reserve(visible_rows_.size() - start + 3);
            // Same turn-state machine as render_messages_list_view() — only
            // the FIRST row of a user/assistant turn owns its marginTop;
            // sibling assistant blocks share it.
            bool next_add_margin = true;
            for (std::size_t vi = start; vi < visible_rows_.size(); ++vi) {
                // TS REF: Messages.tsx L631-635  insert divider BEFORE row.
                if (has_divider && vi == divider_before_vi) {
                    rows.push_back(
                        detail::render_unseen_divider(
                            input_.unseen_divider->count));
                }

                const auto& vr = visible_rows_[vi];
                const bool is_selected =
                    selected_visible_index_.has_value() &&
                    *selected_visible_index_ == vi;

                bool row_add_margin = next_add_margin;
                if (vr.kind == VisibleRow::Kind::Payload) {
                    const MessageShape shape =
                        (vr.row_idx < input_.shapes.size())
                            ? input_.shapes[vr.row_idx]
                            : MessageShape::SystemTaskAssignment;  // = max enum; treated as "not user/assistant"
                    using S = MessageShape;
                    const bool is_assistant_block =
                        (shape == S::AssistantText ||
                         shape == S::AssistantThinking ||
                         shape == S::AssistantRedactedThinking ||
                         shape == S::AssistantToolUse ||
                         shape == S::AssistantGroupedTools);
                    const bool is_user_row =
                        (shape == S::UserText ||
                         shape == S::UserPrompt ||
                         shape == S::UserCommand);
                    if (is_user_row || !is_assistant_block) {
                        next_add_margin = true;
                    } else {
                        next_add_margin = false;
                    }

                    rows.push_back(detail::render_payload_row(
                        input_, vr.row_idx, is_selected, frame_count_, row_add_margin));
                } else {
                    rows.push_back(
                        detail::render_compact_group_row(vr, is_selected));
                    next_add_margin = true;
                }
            }
            list_body = vbox(std::move(rows)) | flex;
        }

        // dbox: the search bar is overlaid on top (height = 1–2 lines);
        // the list_body occupies all remaining space UNDERNEATH it.
        return dbox({
            std::move(list_body) | yframe | vscroll_indicator | flex,
            vbox({
                std::move(search_bar),
                filler(),
            }),
        });
    }

    // ── Public setters — callers use these between frames to feed
    //    streaming deltas, filter toggles, etc. --------------------------

    void set_input(MessagesListInput next) {
        input_ = std::move(next);
        rebuild_visible_cache();
    }

    auto& input()       noexcept { return input_; }
    auto& input() const noexcept { return input_; }

  private:
    // ── Internals ------------------------------------------------------
    void rebuild_visible_cache() {
        visible_rows_     = build_visible_rows(input_);
        last_rows_size_   = input_.rows.size();
        last_search_      = input_.search_query;
        last_filter_hash_ = filter_hash();

        // Re-derive selected_visible_index_ from input.selected_row_idx
        // so that moving through the list updates the right row
        // immediately even across re-builds.
        selected_visible_index_.reset();
        if (input_.selected_row_idx.has_value()) {
            const std::size_t target = *input_.selected_row_idx;
            for (std::size_t i = 0; i < visible_rows_.size(); ++i) {
                const auto& vr = visible_rows_[i];
                if (vr.kind == VisibleRow::Kind::Payload && vr.row_idx == target) {
                    selected_visible_index_ = i;
                    break;
                }
            }
        }
    }

    auto filter_hash() const -> std::uint64_t {
        // Cheap bitmask of booleans; enough to detect *changes*.
        std::uint64_t h = 0;
        h |= std::uint64_t(input_.filters.show_system    ? 1u : 0u) << 0;
        h |= std::uint64_t(input_.filters.show_tool_in   ? 1u : 0u) << 1;
        h |= std::uint64_t(input_.filters.show_tool_out  ? 1u : 0u) << 2;
        h |= std::uint64_t(input_.filters.show_thinking  ? 1u : 0u) << 3;
        h |= std::uint64_t(input_.filters.show_compact   ? 1u : 0u) << 4;
        // TS REF: Messages.tsx L758-763  unseenDivider stability guard — when
        // firstUnseenUuid + count are unchanged, REPL skips re-render work.
        // We include both "present?" bit and count in the hash so either
        // change triggers a rebuild.
        h |= std::uint64_t(input_.unseen_divider.has_value() ? 1u : 0u) << 5;
        h |= (std::uint64_t(input_.unseen_divider.has_value()
                            ? (input_.unseen_divider->count & 0xFFFFF)
                            : 0u))
             << 6;
        h |= (std::uint64_t(input_.compact_boundary_groups.size() & 0xFFFFF)) << 28;
        // NOTE: streaming_tail_row is intentionally NOT hashed — it changes
        // every frame during streaming and the Render() body already re-reads
        // it fresh on each paint; no cache invalidation needed.
        return h;
    }

    void move_selection(int delta) {
        if (visible_rows_.empty()) return;
        std::size_t idx = selected_visible_index_.value_or(visible_rows_.size() - 1);
        // Wrap with saturation (TS behaviour: stop at boundaries, no cycle)
        if (delta > 0) {
            if (idx + 1 >= visible_rows_.size()) return;
            idx += 1;
        } else {
            if (idx == 0) return;
            idx -= 1;
        }
        commit_selection(idx);
    }

    void move_selection_to(std::size_t abs_idx) {
        if (visible_rows_.empty()) return;
        if (abs_idx >= visible_rows_.size()) abs_idx = visible_rows_.size() - 1;
        commit_selection(abs_idx);
    }

    void commit_selection(std::size_t visible_idx) {
        selected_visible_index_ = visible_idx;
        const auto& vr = visible_rows_[visible_idx];
        if (vr.kind == VisibleRow::Kind::Payload) {
            input_.selected_row_idx = vr.row_idx;
            if (cbs_.on_select) cbs_.on_select(vr.row_idx);
        } else {
            // Group row: clear the raw row selection so callers that don't
            // understand groups see "no payload selected".  The visible
            // index itself still highlights.
            input_.selected_row_idx.reset();
            if (cbs_.on_select) cbs_.on_select(std::size_t(-1));
        }
    }

    auto current_visible_row() -> const VisibleRow* {
        if (!selected_visible_index_) return nullptr;
        if (*selected_visible_index_ >= visible_rows_.size()) return nullptr;
        return &visible_rows_[*selected_visible_index_];
    }

    auto fire_action(ActionKind k) -> bool {
        auto* vr = current_visible_row();
        if (!vr || vr->kind != VisibleRow::Kind::Payload) return false;
        if (cbs_.on_action) { cbs_.on_action(vr->row_idx, k); }
        return true;
    }

    // ── Members --------------------------------------------------------
    MessagesListInput    input_;
    MessagesListCallbacks cbs_;

    Component            search_input_;
    std::string          live_search_query_;

    std::vector<VisibleRow> visible_rows_;
    std::optional<std::size_t> selected_visible_index_;
    std::size_t          frame_count_ = 0;

    // Change-detection cache keys
    std::size_t          last_rows_size_   = std::size_t(-1);
    std::string          last_search_;
    std::uint64_t        last_filter_hash_ = std::uint64_t(-1);
};

[[nodiscard]] inline auto MakeMessagesList(
    MessagesListInput input,
    MessagesListCallbacks callbacks = {}) -> Component
{
    return Make<MessagesListComponent>(std::move(input), std::move(callbacks));
}

} // namespace cc::ui::messages_list

// =========================================================================
// REPL integration note:
//   In repl_screen.cppm's `RenderMessages` / `MakeReplScreen` the call site
//   uses this shape/payload API once the engine owns the parallel vectors:
//
//     import cc.ui.messages.messages_list;
//     using cc::ui::messages_list::MakeMessagesList;
//     using cc::ui::messages_list::MessagesListInput;
//     using cc::ui::messages_list::MessagesListCallbacks;
//
//     MessagesListInput in;
//     in.rows   = engine.message_payloads();   // vector<MessageRowPayload>
//     in.shapes = engine.message_shapes();     // vector<MessageShape>
//     in.selected_row_idx     = state.selected_message_idx >= 0 ?
//                                 std::optional(state.selected_message_idx) :
//                                 std::nullopt;
//     in.streaming_tail_row   = engine.is_streaming() ?
//                                 (in.rows.empty() ? 0 : in.rows.size() - 1)
//                               : in.rows.size();   // "not streaming" sentinel
//     in.compact_boundary_groups = engine.compact_groups();
//     in.filters              = {state.show_system, state.show_tool_in, …};
//     in.search_query         = state.search_query;
//     in.jump_to_row_on_init  = state.jump_row;
//
//     MessagesListCallbacks cb{
//         .on_select = [](size_t i){ state.selected_message_idx = int(i); },
//         .on_action = [](size_t i, ActionKind k){ engine.on_row_action(i,k); },
//         .on_toggle_compact_group  = [](size_t g){ engine.toggle_group(g); },
//         .on_click_attachment      = … ,
//         .on_search_changed        = [&](auto& q){ state.search_query = q; },
//     };
//     auto list = MakeMessagesList(std::move(in), std::move(cb));
//
//   Use Render() in the non-interactive path:
//     auto elm = cc::ui::messages_list::render_messages_list_view(in, n, 80);
// =========================================================================
