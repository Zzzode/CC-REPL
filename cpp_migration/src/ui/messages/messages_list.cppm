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

struct MessagesListInput {
    std::vector<MessageRowPayload> rows;
    /// Parallel to `rows`.  The dispatcher needs both shape and payload.
    std::vector<MessageShape>       shapes;

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
            const UserTextMessageData fd = *d;
            Element el = (shape == S::UserCommand || fd.command_name)
                ? RenderUserCommandMessage(fd, /*is_selected=*/is_selected)
                : RenderUserPromptMessage(fd, /*is_selected=*/is_selected);
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

[[nodiscard]] inline auto render_messages_list_view(
    const MessagesListInput& input_const,
    std::size_t frame_count = 0,
    std::size_t render_last_n = kMaxRenderedLastN) -> Element
{
    // build_visible_rows takes a non-const ref (it mutates nothing, but the
    // signature allows future precomputation caching) — copy-on-write.
    MessagesListInput input = input_const;
    auto visible = build_visible_rows(input);

    if (visible.empty()) {
        return vbox({
            detail::render_empty_state(input.search_query),
        }) | yframe | vscroll_indicator;
    }

    // ---- Last-N window (non-virtualized path) ----
    std::size_t start = 0;
    if (visible.size() > render_last_n) {
        start = visible.size() - render_last_n;
    }

    Elements rows;
    rows.reserve(visible.size() - start + 2);
    // TS semantics: each MESSAGE (API level) owns one marginTop=1 blank line.
    // Blocks inside the same assistant message (thinking/text/tool_use) share
    // that margin — only the FIRST assistant block of a turn emits it.  User
    // messages are always treated as turn boundaries.
    bool next_add_margin = true;   // true = row is "first after turn boundary"
    for (std::size_t vi = start; vi < visible.size(); ++vi) {
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
            const bool row_add_margin = next_add_margin;
            // Advance turn-state: user rows / system / tool-results are
            // boundaries — next row gets its own margin.  An assistant block
            // starts or continues a turn: next sibling assistant block has
            // add_margin=false.  Any non-assistant row resets.
            if (is_user_row || !is_assistant_block) {
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

        // ---- Streaming tail: append after the row indicated by
        //      streaming_tail_row.  If that row is at or beyond the end of
        //      `rows` we append at the very bottom.
        if (vr.kind == VisibleRow::Kind::Payload &&
            vr.row_idx == input.streaming_tail_row &&
            input.streaming_tail_row < input.rows.size())
        {
            const bool blink = (frame_count % 2) != 0;
            const char* sg = detail::spinner_glyph(frame_count);
            Element tail = hbox({
                text("   "),
                text(sg) | color(palette::streaming_fg()),
                text(" generating…") | dim | color(palette::muted_fg()),
                text(blink ? " ▌" : "  ") | color(palette::streaming_fg()) | bold,
            });
            rows.push_back(std::move(tail));
            rows.push_back(separatorEmpty());
        }
    }

    Element list = vbox(std::move(rows));
    if (input.scroll_offset > 0) {
        const int viewport_rows = std::max(1, input.viewport_rows);
        list = std::move(list)
             | focusPosition(0, input.scroll_offset + viewport_rows / 2);
    } else if (input.pin_to_bottom && visible.size() > 1) {
        list = std::move(list) | focusPositionRelative(0, 1);
    }
    // NOTE: We used to apply yframe | vscroll_indicator here unconditionally,
    // which caused a 0-row messages area (empty transcript) to EAT its sibling
    // WelcomeHeader inside the outer flex vbox.  Only apply flex when the
    // message list actually has content so the welcome strip keeps its
    // natural 4 rows.
    if (visible.empty()) {
        return std::move(list) | yframe | vscroll_indicator;
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
        } else {
            // ---- Last-N window ------------------------------------
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

            Elements rows;
            rows.reserve(visible_rows_.size() - start + 2);
            // Same turn-state machine as render_messages_list_view() — only
            // the FIRST row of a user/assistant turn owns its marginTop;
            // sibling assistant blocks share it.
            bool next_add_margin = true;
            for (std::size_t vi = start; vi < visible_rows_.size(); ++vi) {
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

                // Streaming tail
                if (vr.kind == VisibleRow::Kind::Payload &&
                    vr.row_idx == input_.streaming_tail_row &&
                    input_.streaming_tail_row < input_.rows.size())
                {
                    const bool blink = (frame_count_ % 2) != 0;
                    const char* sg = detail::spinner_glyph(frame_count_);
                    rows.push_back(hbox({
                        text("   "),
                        text(sg) | color(palette::streaming_fg()),
                        text(" generating…") | dim | color(palette::muted_fg()),
                        text(blink ? " ▌" : "  ")
                            | color(palette::streaming_fg()) | bold,
                    }));
                    rows.push_back(separatorEmpty());
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
        h |= (std::uint64_t(input_.compact_boundary_groups.size() & 0xFFFFF)) << 8;
        h |= (std::uint64_t(input_.streaming_tail_row    & 0xFFFFF)) << 28;
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
