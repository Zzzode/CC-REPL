/// =========================================================================
/// @file messages_list.cppm
/// @brief Messages list container + single-message envelope wrapper.
///
/// UI21 RESPONSIBILITY — "列表容器 + 单消息外容器" (REPL 主路径 P0 阻塞项)
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
///   import cc.ui.design.themed_box;         // UI20 primitives placeholder
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
#include <ftxui/component/input.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.messages.messages_list;

// ─── Strict re-uses (no type / colour duplication) ──────────────────────
import cc.ui.messages.message_row;
import cc.ui.messages.message_timestamp;
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
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;

            // User
            if constexpr (std::is_same_v<T, UserTextMessageData>) {
                return v.content;
            } else if constexpr (std::is_same_v<T, user_command::UserCommandData>) {
                return v.raw_input.size() ? v.raw_input : v.command_line;
            } else if constexpr (std::is_same_v<T, BashIOEntry>) {
                return v.content;
            } else if constexpr (std::is_same_v<T, UserMessageData>) {
                return v.content_preview.size() ? v.content_preview : v.title;
            } else if constexpr (std::is_same_v<T, ImageMessageData>) {
                return v.caption.size() ? v.caption : ("image " + v.file_name);
            } else if constexpr (std::is_same_v<T, ToolResultOptions>) {
                return v.tool_name + " " + v.output_preview;
            } else if constexpr (std::is_same_v<T, local_cmd::LocalCommandOptions>) {
                return v.command + "\n" + v.output_preview;
            } else if constexpr (std::is_same_v<T, attachment_message::AttachmentGridOptions>) {
                std::string out;
                for (const auto& a : v.attachments) {
                    out += a.name;
                    out += ' ';
                }
                return out;
            }
            // Assistant
            else if constexpr (std::is_same_v<T, AssistantTextMessageData>) {
                return v.content;
            } else if constexpr (std::is_same_v<T, tool_use_message::ToolUseRenderOptions>) {
                return v.tool_name + " " + v.input_preview;
            } else if constexpr (std::is_same_v<T, tool_use_message::GroupedToolsOptions>) {
                std::string out;
                for (const auto& t : v.tools) out += t.name + " ";
                return out;
            } else if constexpr (std::is_same_v<T, thinking_message::ThinkingMessageOptions>) {
                return v.text;
            }
            // System
            else if constexpr (std::is_same_v<T, SystemTextMessageData>) {
                return v.summary + " " + v.detail;
            } else if constexpr (std::is_same_v<T, ErrorMessageData>) {
                return v.title + " " + v.body;
            } else if constexpr (std::is_same_v<T, api_error_message::APIErrorOptions>) {
                return v.message + " " + v.recovery_hint;
            } else if constexpr (std::is_same_v<T, collapsed_content::CollapsedContentOptions>) {
                return v.summary;
            } else if constexpr (std::is_same_v<T, RateLimitInfo>) {
                return v.reason + " " + v.message;
            } else if constexpr (std::is_same_v<T, PlanApprovalOptions>) {
                std::string out;
                for (const auto& s : v.steps) out += s.description + " ";
                return out;
            } else if constexpr (std::is_same_v<T, std::vector<HookProgressEntry>>) {
                std::string out;
                for (const auto& h : v) out += h.hook_name + " ";
                return out;
            } else if constexpr (std::is_same_v<T, shutdown::ShutdownMessageData>) {
                return v.reason + " " + v.detail;
            } else if constexpr (std::is_same_v<T, AdvisorMessage>) {
                return v.title + " " + v.body;
            } else if constexpr (std::is_same_v<T, UI5HandledTag>) {
                return {};
            } else {
                static_assert(!sizeof(T*), "Unreachable: variant branch missing in payload_preview.");
                return {};
            }
        },
        p);
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
                    // engine can write precomputed stats into
                    // compact_boundary_groups[].{additions,deletions} via
                    // the optional parallel vector (see TODO below).  For
                    // now we approximate from UserToolResult preview_text
                    // occurrences of "+++" / "---" markers — cheap.
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
    header_els.push_back(text(format_timestamp(opts.timestamp)) | color(muted_fg()));

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
                               std::size_t frame_count) -> Element
{
    if (row_idx >= input.shapes.size() || row_idx >= input.rows.size()) {
        return text("⚠ bad row_idx") | color(Color::Yellow);
    }
    MessageShape shape   = input.shapes[row_idx];
    MessageRowCallbacks cb{};   // envelope callbacks come via the outer component
    Component inner = RenderMessageRowByType(shape, input.rows[row_idx], std::move(cb));

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
    for (std::size_t vi = start; vi < visible.size(); ++vi) {
        const auto& vr = visible[vi];
        const bool is_selected =
            input.selected_row_idx.has_value() &&
            vr.kind == VisibleRow::Kind::Payload &&
            vr.row_idx == *input.selected_row_idx;

        if (vr.kind == VisibleRow::Kind::Payload) {
            rows.push_back(detail::render_payload_row(
                input, vr.row_idx, is_selected, frame_count));
        } else {
            rows.push_back(detail::render_compact_group_row(vr, is_selected));
        }
        rows.push_back(separatorEmpty());

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

    return vbox(std::move(rows)) | yframe | vscroll_indicator | flex;
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
            search_input_->SetActive(false);
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
            for (std::size_t vi = start; vi < visible_rows_.size(); ++vi) {
                const auto& vr = visible_rows_[vi];
                const bool is_selected =
                    selected_visible_index_.has_value() &&
                    *selected_visible_index_ == vi;

                if (vr.kind == VisibleRow::Kind::Payload) {
                    rows.push_back(detail::render_payload_row(
                        input_, vr.row_idx, is_selected, frame_count_));
                } else {
                    rows.push_back(
                        detail::render_compact_group_row(vr, is_selected));
                }
                rows.push_back(separatorEmpty());

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
// TODO (UI21 → REPL integration):
//   In repl_screen.cppm's `RenderMessages` / `MakeReplScreen` the call site
//   will look roughly like this — real wiring follows once the engine owns
//   the shape/payload parallel vectors:
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
