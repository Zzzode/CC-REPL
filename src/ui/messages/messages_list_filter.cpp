// messages_list_filter.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). Brief-mode filtering, drop-text mask, expand
// keys, the lowered/cached search-text accessor, shape categories and the
// O(N) build_visible_rows walk. No FTXUI types are named here.
//
// Phase A (#184957): std is TEXTUAL here on purpose — no `import std;`.
// With an empty GMF, this impl unit of a primary whose own GMF pulls libc++
// textually (via FTXUI) made clang 22's reduced-BMI writer emit a duplicate
// aligned operator new ("call to 'operator new' is ambiguous" in
// std::__libcpp_allocate). Impl units that textually include FTXUI
// themselves are unaffected; this one names no FTXUI type, so the
// app_extra_methods.cpp fallback applies.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module cc.ui.messages.messages_list;

import cc.ui.messages.message_row;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.thinking_message;
import cc.ui.messages.tool_use_message;

namespace cc::ui::messages_list {

namespace brief_detail {

[[nodiscard]] bool is_brief_tool_name(std::string_view name) {
    for (auto tn : kBriefToolNames) {
        if (name == tn) return true;
    }
    return false;
}

/// Returns true if the tool name triggers dropTextInBriefTurns (TS: dropTextToolNames).
[[nodiscard]] bool is_drop_text_tool_name(std::string_view name) {
    for (auto tn : kDropTextToolNames) {
        if (name == tn) return true;
    }
    return false;
}

/// Extract tool_name from a MessageRowPayload if it is a tool_use or
/// tool_result variant.  Returns empty string otherwise.
[[nodiscard]] std::string_view extract_tool_name(const MessageRowPayload& p) {
    if (auto* opts = std::get_if<::cc::ui::messages::tool_use_message::ToolUseRenderOptions>(&p)) {
        return opts->call.tool_name;
    }
    if (auto* grp = std::get_if<::cc::ui::messages::tool_use_message::GroupedToolsOptions>(&p)) {
        if (!grp->calls.empty()) return grp->calls[0].tool_name;
    }
    if (auto* tro = std::get_if<::cc::ui::messages::ToolResultOptions>(&p)) {
        return tro->tool_name;
    }
    return {};
}

}  // namespace brief_detail

/// Returns true if the row at index `i` should be VISIBLE in brief mode.
/// TS REF: Messages.tsx filterForBriefTool (lines 93-158).
auto passes_brief_filter(
    MessageShape shape,
    const MessageRowPayload& payload) -> bool {
    using S = MessageShape;
    namespace bd = brief_detail;

    switch (shape) {
        // System rows: always visible (TS: system messages must stay visible
        // for user feedback; api_metrics subtype dropped but not tagged in CPP)
        case S::SystemText:
        case S::SystemRateLimit:
        case S::SystemPlanApproval:
        case S::SystemHookProgress:
        case S::SystemShutdown:
        case S::SystemCompactBoundary:
        case S::SystemAdvisor:
        case S::SystemTaskAssignment:
        case S::SystemCollapsedContent:
        case S::SystemAPIError:
            return true;

        // Assistant rows: only brief tool uses are visible
        case S::AssistantToolUse:
        case S::AssistantGroupedTools:
            return bd::is_brief_tool_name(bd::extract_tool_name(payload));

        // Assistant text + thinking: hidden in brief mode
        case S::AssistantText:
        case S::AssistantThinking:
        case S::AssistantRedactedThinking:
            return false;

        // User input: always visible (real user text + command chips)
        case S::UserText:
        case S::UserPrompt:
        case S::UserCommand:
        case S::UserBashInput:
        case S::UserBashOutput:
        case S::UserLocalCommandOutput:
        case S::UserLocalJsxOutput:
        case S::UserImage:
        case S::UserTeammate:
        case S::UserChannel:
        case S::UserAgentNotification:
        case S::UserMemoryInput:
        case S::UserPlan:
        case S::UserResourceUpdate:
        case S::UserAttachments:
            return true;

        // Tool results: only brief-tool results visible
        case S::UserToolResult:
            return bd::is_brief_tool_name(bd::extract_tool_name(payload));

        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// dropTextInBriefTurns (TS REF: Messages.tsx L169-206).
//
// In default mode (neither transcript nor brief-only), drops assistant TEXT
// rows in turns that called a Brief/SendUserMessage/SendUserFile tool.  The
// model's text output is redundant with the SendUserMessage content it wrote
// right after — dropping it keeps the transcript focused on tool output.
//
// Per-turn: only drops text in turns that actually called a Brief tool.  If
// the model forgets to call Brief, text still shows — otherwise the user
// would see nothing for that turn.
//
// Returns a vector<bool> mask (true = KEEP the row, false = DROP it).
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<bool> compute_drop_text_mask(
    const std::vector<MessageShape>& shapes,
    const std::vector<MessageRowPayload>& payloads)
{
    using S = MessageShape;
    namespace bd = brief_detail;

    const auto N = shapes.size();
    std::vector<bool> keep(N, true);   // default: keep everything

    // First pass: find which turns contain a Brief tool_use.
    std::set<std::size_t> turns_with_brief;
    // text_index_to_turn[i] = turn number for assistant text at index i.
    std::vector<std::optional<std::size_t>> text_index_to_turn(N, std::nullopt);

    std::size_t turn = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const auto sh = shapes[i];
        // Real user message (non-tool_result) → advance turn counter.
        // TS REF: L188  msg.type === 'user' && block?.type !== 'tool_result' && !msg.isMeta
        const bool is_real_user =
            (sh == S::UserText || sh == S::UserPrompt || sh == S::UserCommand ||
             sh == S::UserBashInput || sh == S::UserBashOutput ||
             sh == S::UserLocalCommandOutput || sh == S::UserLocalJsxOutput ||
             sh == S::UserImage || sh == S::UserPlan || sh == S::UserResourceUpdate ||
             sh == S::UserMemoryInput || sh == S::UserChannel ||
             sh == S::UserAgentNotification || sh == S::UserTeammate ||
             sh == S::UserAttachments);
        if (is_real_user) {
            ++turn;
            continue;
        }
        if (sh == S::AssistantText) {
            text_index_to_turn[i] = turn;
        } else if ((sh == S::AssistantToolUse || sh == S::AssistantGroupedTools) &&
                   i < payloads.size()) {
            if (bd::is_drop_text_tool_name(bd::extract_tool_name(payloads[i]))) {
                turns_with_brief.insert(turn);
            }
        }
    }

    if (turns_with_brief.empty()) return keep;   // no brief turns → keep all

    // Second pass: mark assistant text rows for dropping if their turn had Brief.
    for (std::size_t i = 0; i < N; ++i) {
        if (text_index_to_turn[i].has_value() &&
            turns_with_brief.count(*text_index_to_turn[i]) > 0) {
            keep[i] = false;
        }
    }
    return keep;
}

// ---------------------------------------------------------------------------
// Expand-key computation (TS REF: Messages.tsx expandKey L725-727)
//
// For tool_use and tool_result rows, returns the tool_name so a tool_use
// and its corresponding tool_result share the same key and expand together.
// For other rows, returns the uuid (or empty string if not available).
// ---------------------------------------------------------------------------
[[nodiscard]] std::string compute_expand_key(
    MessageShape shape,
    const MessageRowPayload& payload,
    std::string_view uuid) {
    using S = MessageShape;
    namespace bd = brief_detail;

    if (shape == S::AssistantToolUse || shape == S::AssistantGroupedTools ||
        shape == S::UserToolResult) {
        auto name = bd::extract_tool_name(payload);
        if (!name.empty()) return std::string(name);
    }
    // Fallback: use uuid (first 24 chars to match TS deriveUUID prefix)
    if (!uuid.empty()) return std::string(uuid.substr(0, 24));
    return {};
}

/// TS REF: Messages.tsx isItemClickable (L582-594).
/// Returns true if the row supports click-to-expand: tool results that are
/// truncated, collapsed read/search groups, or advisor tool results.
[[nodiscard]] bool is_row_clickable(
    MessageShape shape,
    const MessageRowPayload& payload) {
    using S = MessageShape;
    namespace bd = brief_detail;

    // Collapsed content groups: always clickable
    if (shape == S::SystemCollapsedContent) return true;

    // Tool results: clickable if the result is marked truncated
    if (shape == S::UserToolResult) {
        if (auto* opts = std::get_if<::cc::ui::messages::ToolResultOptions>(&payload)) {
            return opts->is_truncated;
        }
    }

    // Tool uses: clickable if they have a result preview (means they have content
    // that could be expanded)
    if (shape == S::AssistantToolUse) {
        if (auto* opts = std::get_if<::cc::ui::messages::tool_use_message::ToolUseRenderOptions>(&payload)) {
            return opts->call.result_preview.has_value() &&
                   !opts->call.result_preview->empty();
        }
    }

    return false;
}

/// Returns true if the row at index `i` is currently "expanded" (user has
/// toggled it open via Enter/Space).  Expanded rows render with verbose=true
/// showing full content instead of truncated summaries.
[[nodiscard]] bool is_row_expanded(
    const MessagesListInput& input,
    std::size_t row_idx) {
    if (input.expanded_keys.empty()) return false;
    if (row_idx >= input.shapes.size() || row_idx >= input.rows.size()) return false;
    std::string_view uuid = (row_idx < input.uuids.size())
        ? std::string_view(input.uuids[row_idx]) : std::string_view{};
    auto key = compute_expand_key(input.shapes[row_idx], input.rows[row_idx], uuid);
    if (key.empty()) return false;
    return input.expanded_keys.count(key) > 0;
}

namespace detail {

/// Lower-case a UTF-8 string in place.  Only touches ASCII letters because
/// MessageRowPayload text fields are overwhelmingly English / path literals
/// (same strategy as TS renderableSearchText → toLowerCase).
auto lowered(std::string s) -> std::string {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

// =========================================================================
// 2c)  Cached lowered search text accessor
// =========================================================================
// TS REF: Messages.tsx L649-676
//   const searchTextCache = useRef(new WeakMap<RenderableMessage, string>());
//   const extractSearchText = useCallback((msg) => {
//     const cached = searchTextCache.current.get(msg);
//     if (cached !== undefined) return cached;
//     let text = renderableSearchText(msg);
//     // ... tool.extractSearchText override ...
//     const lowered = text.toLowerCase();
//     searchTextCache.current.set(msg, lowered);
//     return lowered;
//   }, [...]);
//
// Returns the LOWERED rich searchable text for row_idx, using the per-row
// cache on MessagesListInput.  First call computes + caches; subsequent
// calls (build_visible_rows then visible_rows_to_virtual) hit the cache
// with zero alloc.
//
// Cache is keyed by row_idx (parallel to input.rows) — messages are
// append-only and immutable, so a cached entry is always valid.
[[nodiscard]] auto get_cached_lowered_search_text(
    const MessagesListInput& input,
    std::size_t row_idx,
    MessageShape shape) -> std::string
{
    if (row_idx >= input.rows.size()) return {};

    // Grow cache to match rows size on first access.
    if (input.lowered_search_cache.size() <= row_idx) {
        input.lowered_search_cache.resize(input.rows.size());
    }

    auto& slot = input.lowered_search_cache[row_idx];
    if (slot.has_value()) {
        return *slot;   // cache hit — zero alloc
    }

    // Cache miss: compute + store.  extract_search_text does the 2-tier
    // lookup (tool.extractSearchText preferred, renderableSearchText
    // fallback); we lower once here and cache the result.
    std::string lowered_text = detail::lowered(
        detail::extract_search_text(input.rows[row_idx], shape));
    slot = lowered_text;
    return lowered_text;
}

/// Returns true for message SHAPEs that belong to each filter category.
/// Mirrors the TS Messages.tsx category switches (system / tool_use /
/// tool_result / thinking / compacted).
auto shape_category(MessageShape s) -> std::string_view {
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

auto passes_filters(MessageShape s, const Filters& f) -> bool {
    auto cat = shape_category(s);
    if (cat == "system"   && !f.show_system)   return false;
    if (cat == "tool_in"  && !f.show_tool_in)  return false;
    if (cat == "tool_out" && !f.show_tool_out) return false;
    if (cat == "thinking" && !f.show_thinking) return false;
    return true;
}

} // namespace detail

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
auto build_visible_rows(MessagesListInput& input) -> std::vector<VisibleRow> {
    const auto N = input.rows.size();
    // ---- Step 0 : pre-compute lowered search needle (empty = skip search)
    const std::string needle = detail::lowered(input.search_query);
    const bool do_search     = !needle.empty();

    // ---- Step 0b : pre-compute dropTextInBriefTurns mask for default mode.
    //              Only needed when NOT in transcript mode AND NOT in brief mode.
    //              TS REF: Messages.tsx L510-514 (3-tier briefFiltered logic).
    std::vector<bool> drop_text_keep_mask;
    const bool apply_drop_text = !input.is_transcript_mode && !input.is_brief_mode;
    if (apply_drop_text && N > 0) {
        drop_text_keep_mask = compute_drop_text_mask(input.shapes, input.rows);
    }

    // ---- Step 1 : mark rows that pass (filters AND search AND 3-tier filter).
    //              Separately build a "row visible" bitmask so Step 2 can use it.
    //
    // 3-tier filter (TS REF: Messages.tsx L505-514):
    //   Tier 1 (transcript mode): show ALL message types — bypass brief/dropText.
    //   Tier 2 (brief-only):     only brief tool chain + user input + system.
    //   Tier 3 (default):        drop assistant text in turns that called Brief
    //                            (dropTextInBriefTurns), keep everything else.
    std::vector<bool> row_passes(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        if (i >= input.shapes.size()) break;   // malformed input → safe stop
        if (!detail::passes_filters(input.shapes[i], input.filters)) continue;

        // Tier 1: transcript mode — skip brief/dropText filters entirely.
        // TS REF: L514  !isTranscriptMode ? ... : messagesToShowNotTruncated
        if (!input.is_transcript_mode) {
            if (input.is_brief_mode) {
                // Tier 2: brief-only — filterForBriefTool.
                // TS REF: L514  isBriefOnly ? filterForBriefTool(...)
                if (i < input.rows.size() &&
                    !passes_brief_filter(input.shapes[i], input.rows[i])) {
                    continue;
                }
            } else if (apply_drop_text && i < drop_text_keep_mask.size()) {
                // Tier 3: default — dropTextInBriefTurns.
                // TS REF: L514  dropTextInBriefTurns(messagesToShowNotTruncated, ...)
                if (!drop_text_keep_mask[i]) continue;
            }
        }

        if (do_search) {
            // TS REF: Messages.tsx L650-676  2-tier search text extraction
            //   with WeakMap cache.  Use cached accessor: first call computes
            //   + caches lowered rich text; subsequent calls (visible_rows_to_virtual
            //   scroll_search) hit the cache with zero alloc.
            const std::string hay = detail::get_cached_lowered_search_text(
                input, i, input.shapes[i]);
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
        // 3) skip hidden thinking rows entirely (TS returns null → zero height;
        //    FTXUI vbox always allocates 1 line per child, so we must not emit them).
        //    Keep them visible if selected (user expanded) or streaming tail.
        if (i < input.shapes.size()) {
            const auto shape = input.shapes[i];
            if (shape == MessageShape::AssistantThinking ||
                shape == MessageShape::AssistantRedactedThinking) {
                const bool is_selected_row = input.selected_row_idx.has_value() &&
                    *input.selected_row_idx == i;
                const bool is_streaming = (i == input.streaming_tail_row &&
                    input.streaming_tail_row < input.rows.size());
                if (!is_selected_row && !is_streaming) {
                    // TS REF: Messages.tsx L395-419 — when streaming thinking
                    // is globally visible, hide ALL completed thinking rows
                    // (TS: lastThinkingBlockId = 'streaming' means no
                    // completed thinking block matches → all are hidden).
                    if (input.streaming_thinking_globally_visible) continue;
                    if (auto* opts = std::get_if<thinking_message::ThinkingMessageOptions>(
                            &input.rows[i])) {
                        using TM = thinking_message::ThinkingState;
                        if (opts->data.state == TM::Complete) continue;
                    }
                }
            }
        }
        // 4) regular payload row
        if (!row_passes[i]) continue;
        out.push_back(VisibleRow{
            .kind    = VisibleRow::Kind::Payload,
            .row_idx = i,
        });
    }

    // ---- Step 4 : transcript-mode cap (TS REF: Messages.tsx L515-516, L276).
    //              When is_transcript_mode and NOT show_all_in_transcript, cap
    //              visible rows to last kMaxMessagesInTranscriptMode (30).
    //              Prepend a TranscriptCapDivider showing how many were hidden.
    if (input.is_transcript_mode && !input.show_all_in_transcript &&
        out.size() > kMaxMessagesInTranscriptMode)
    {
        const std::size_t hidden = out.size() - kMaxMessagesInTranscriptMode;
        std::vector<VisibleRow> capped;
        capped.reserve(kMaxMessagesInTranscriptMode + 1);
        capped.push_back(VisibleRow{
            .kind         = VisibleRow::Kind::TranscriptCapDivider,
            .hidden_count = hidden,
        });
        // Copy last kMax rows from the full output.
        const auto start = out.end() - static_cast<std::ptrdiff_t>(kMaxMessagesInTranscriptMode);
        capped.insert(capped.end(), start, out.end());
        out = std::move(capped);
    }

    return out;
}

} // namespace cc::ui::messages_list
