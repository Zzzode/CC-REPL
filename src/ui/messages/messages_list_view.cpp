// messages_list_view.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). The static render_messages_list_view and the
// windowed render_messages_list_virtual element builders.
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

module cc.ui.messages.messages_list;

import std;

import cc.ui.messages.message_row;
import cc.ui.messages.virtual_list;

namespace cc::ui::messages_list {

[[nodiscard]] auto render_messages_list_virtual(
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

    // ── Pre-compute add_margin for each visible row using the same
    //    turn-state machine as the static path (TS visual parity). ──
    std::vector<bool> add_margin_for_vi(visible.size(), true);
    {
        bool next_add_margin = true;
        bool prev_was_user = false;
        for (std::size_t vi = 0; vi < visible.size(); ++vi) {
            const auto& vr = visible[vi];
            if (vr.kind == VisibleRow::Kind::Payload) {
                const MessageShape shape =
                    (vr.row_idx < input.shapes.size())
                        ? input.shapes[vr.row_idx]
                        : MessageShape::SystemTaskAssignment;
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
                     shape == S::UserCommand ||
                     shape == S::UserImage);
                const bool is_tool_result = (shape == S::UserToolResult);
                const bool is_same_turn_as_assistant =
                    is_assistant_block || is_tool_result;
                const bool is_turn_boundary = is_user_row ||
                    (!is_same_turn_as_assistant && !is_tool_result);

                add_margin_for_vi[vi] = is_user_row
                    ? !prev_was_user
                    : (is_turn_boundary ? true : next_add_margin);
                prev_was_user = is_user_row;

                // TS REF: Messages.tsx L714-719 — streaming thinking tail has
                // addMargin={false}.  When this is the last visible row and
                // it's a thinking block while streaming thinking is globally
                // visible, force 0 top margin (flush against preceding row).
                if (shape == S::AssistantThinking &&
                    vi == visible.size() - 1 &&
                    input.streaming_thinking_globally_visible) {
                    add_margin_for_vi[vi] = false;
                }

                if (is_turn_boundary) {
                    next_add_margin = !is_user_row;
                } else if (is_tool_result) {
                    // TS: after a tool result, the next assistant response
                    // starts a new visual group with marginTop=1.
                    next_add_margin = true;
                } else {
                    next_add_margin = false;
                }
            } else {
                add_margin_for_vi[vi] = true;
                next_add_margin = true;
                prev_was_user = false;
            }
        }
    }

    // ── render_row callback: translate virtual back to messages_list VR
    state.callbacks.render_row =
        [frame_count, &input, divider_before_vi, has_divider,
         &add_margin_for_vi]
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
            // Use pre-computed add_margin from the turn-state machine above.
            // row_index maps 1:1 to visible[] index because visible_rows_to_virtual
            // preserves order with no dropping.
            const bool add_margin = (row_index < add_margin_for_vi.size())
                ? add_margin_for_vi[row_index]
                : true;

            Element row_el;
            if (ml_row.kind == VisibleRow::Kind::CompactGroup) {
                row_el = detail::render_compact_group_row(ml_row, is_selected);
            } else if (ml_row.kind == VisibleRow::Kind::TranscriptCapDivider) {
                row_el = detail::render_transcript_cap_divider(ml_row.hidden_count);
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

    // ── Wire 2-tier search engine into virtual list state ──────────────
    // TS REF: Messages.tsx L700  extractSearchText passed to VirtualMessageList
    //   (the same callback used by build_visible_rows search filter).
    //
    // When input.search_query is non-empty, run the search engine over the
    // virtual rows.  This populates state.search_matches + prefixSum so
    // that scroll_keys n/N navigation can jump between matches.
    //
    // Note: build_visible_rows already FILTERED the visible set by query.
    // The virtual list search engine is for NAVIGATION within that set
    // (finding which row contains the query, jumping to nearest match).
    if (!input.search_query.empty()) {
        std::string lowered_query = detail::lowered(input.search_query);
        vl::run_search(state, lowered_query);
    }

    // Wire search_step callback so scroll_keys FSM n/N keys can navigate
    // between search matches.  TS REF: VirtualMessageList.tsx L762 search_step
    state.callbacks.search_step =
        [&state](int delta) -> int {
            if (state.search_matches.empty()) return -1;
            size_t n = state.search_matches.size();
            size_t target_ptr = (static_cast<int>(state.search_ptr) + delta +
                                 static_cast<int>(n)) % static_cast<int>(n);
            size_t target_row = state.search_matches[target_ptr];
            return state.jh.find_visual_top_for_row(target_row);
        };

    Element body = vl::render_list_as_elements(state);
    return body | yframe | vscroll_indicator | flex;
}

// =========================================================================
// 7)  STATIC RENDER  (render_messages_list_view)
// =========================================================================
/// Non-interactive version.  Used by dialogs that don't need the full
/// selection/action pipeline.  Still applies filters + compact collapsing
/// + streaming tail rendering + scroll frame.


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
[[nodiscard]] auto render_messages_list_view(
    const MessagesListInput& input_const,
    std::size_t frame_count,
    std::size_t render_last_n,
    Elements trailing_elements,
    bool wrap_in_yframe,
    Elements leading_elements) -> Element
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
    // ── TS PARITY (2026-07-05): Per-message addMargin ──────────────────────
    // TS REF: MessageRow.tsx  addMargin = !hasMetadata.
    //   hasMetadata = isTranscriptMode && type==="assistant" && has-text &&
    //                 (timestamp || model)
    // In REPL mode (isTranscriptMode=false), hasMetadata is ALWAYS false →
    // addMargin=true for every message.  Each leaf component applies
    // marginTop={addMargin ? 1 : 0}.
    //
    // EXCEPTIONS (matching TS):
    //   1. UserToolResultMessage — does NOT receive addMargin prop, 0 marginTop.
    //      Tool results sit flush against the preceding tool_use message.
    //   2. User continuations (isUserContinuation in TS) — user images
    //      following another user block suppress marginTop (⎿ connector).
    //
    // This replaces the previous "turn-boundary" model which incorrectly
    // suppressed margins on ALL assistant blocks after a user row, causing:
    //   - User→assistant text gap = 0 (too small)
    //   - Inconsistent spacing when tool results were present vs absent.
    bool prev_was_user = false;    // for user-turn continuation ⎿ connector (TS parity)
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
            const bool is_user_row =
                (shape == S::UserText ||
                 shape == S::UserPrompt ||
                 shape == S::UserCommand ||
                 shape == S::UserImage);
            // TS: UserToolResultMessage has 0 marginTop (no addMargin prop).
            // Also include UserBashOutput and UserLocalCommandOutput as
            // tool-result-like rows that sit flush.
            const bool is_tool_result =
                (shape == S::UserToolResult ||
                 shape == S::UserBashOutput);

            // TS PARITY: compute addMargin per-row, not per-turn.
            //   - User rows: first user in turn → true, continuation → false (⎿)
            //   - Tool result rows: false (flush against preceding tool_use)
            //   - All other rows: true (TS: !hasMetadata = true in REPL mode)
            bool row_add_margin = is_user_row
                ? !prev_was_user
                : (is_tool_result ? false : true);
            // TS REF: Messages.tsx L714-719 — streaming thinking tail has
            // addMargin={false} (sits flush against preceding row).  When
            // this is the last visible row and it's a thinking block while
            // streaming thinking is globally visible, force 0 top margin.
            if (shape == S::AssistantThinking &&
                vi == visible.size() - 1 &&
                input.streaming_thinking_globally_visible) {
                row_add_margin = false;
            }
            prev_was_user = is_user_row;

            rows.push_back(detail::render_payload_row(
                input, vr.row_idx, is_selected, frame_count, row_add_margin));
        } else if (vr.kind == VisibleRow::Kind::TranscriptCapDivider) {
            // "─── N older messages hidden · Ctrl+E to show all ───"
            rows.push_back(detail::render_transcript_cap_divider(vr.hidden_count));
        } else {
            // compact group row — renders its own header/spacing
            rows.push_back(detail::render_compact_group_row(vr, is_selected));
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
    // NOTE: leading elements (logo card) are intentionally NOT included in
    // the pin-to-bottom estimate.  Pin-to-bottom should engage when MESSAGES
    // overflow the viewport, ensuring the latest message is visible.  The
    // logo is a decorative leading element that scrolls naturally — when
    // messages alone fit, the user sees logo + all messages top-aligned.
    // When messages overflow, pin-to-bottom engages and the logo scrolls
    // off-screen (reachable by scrolling up).  Adding logo height would
    // cause premature pin-to-bottom with just 1-2 messages, pushing content
    // to the bottom and creating a large blank area below the logo.
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

} // namespace cc::ui::messages_list
