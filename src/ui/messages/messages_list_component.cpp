// messages_list_component.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). MessagesListComponent's ctor, OnEvent (the key
// function), Render and private selection/cache helpers, plus the
// MakeMessagesList factory - all out-of-line here so each emits once as a
// strong symbol (no weak duplicates across TUs).
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/box.hpp>

module cc.ui.messages.messages_list;

import std;

import cc.ui.messages.message_row;
import cc.ui.messages.virtual_list;

namespace cc::ui::messages_list {

MessagesListComponent::MessagesListComponent(
    MessagesListInput input,
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

bool MessagesListComponent::OnEvent(Event event) {
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
        // ------ When the search Input owns focus, let it handle events --
        // Mouse events bypass the search-focus gate: clicking a message row
        // should work even when the search box has keyboard focus.  TS REF:
        //   VirtualMessageList.tsx onClickK fires regardless of search state.
        if (search_focused && !event.is_mouse()) {
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

        // ------ Toggle compact group expand OR toggle row expansion ------
        if (event == Event::Character(' ')) {
            auto* vr = current_visible_row();
            if (!vr) return false;
            if (vr->kind == VisibleRow::Kind::CompactGroup) {
                if (cbs_.on_toggle_compact_group)
                    cbs_.on_toggle_compact_group(vr->group_idx);
                return true;
            }
            // TS REF: Messages.tsx onItemClick (L564-571)
            // Space on a clickable/expanded payload row toggles verbose expansion.
            if (vr->kind == VisibleRow::Kind::Payload) {
                auto idx = vr->row_idx;
                if (idx < input_.shapes.size() && idx < input_.rows.size()) {
                    std::string_view uuid = (idx < input_.uuids.size())
                        ? std::string_view(input_.uuids[idx]) : std::string_view{};
                    auto key = compute_expand_key(
                        input_.shapes[idx], input_.rows[idx], uuid);
                    bool clickable = is_row_clickable(
                        input_.shapes[idx], input_.rows[idx]);
                    bool expanded = !key.empty() &&
                        input_.expanded_keys.count(key) > 0;
                    if (clickable || expanded) {
                        if (cbs_.on_toggle_expand && !key.empty())
                            cbs_.on_toggle_expand(key);
                        return true;
                    }
                }
            }
            return false;   // fall through: space is not a hotkey elsewhere
        }

        // ------ Enter  →  default action (copy) OR toggle group OR expand -------
        if (event == Event::Return) {
            auto* vr = current_visible_row();
            if (!vr) return false;
            if (vr->kind == VisibleRow::Kind::CompactGroup) {
                if (cbs_.on_toggle_compact_group)
                    cbs_.on_toggle_compact_group(vr->group_idx);
                return true;
            }
            // TS REF: Messages.tsx cursor.expanded (L624)
            // Enter on a clickable/expanded payload row toggles verbose expansion
            // (takes priority over copy for tool rows with truncated output).
            if (vr->kind == VisibleRow::Kind::Payload) {
                auto idx = vr->row_idx;
                if (idx < input_.shapes.size() && idx < input_.rows.size()) {
                    std::string_view uuid = (idx < input_.uuids.size())
                        ? std::string_view(input_.uuids[idx]) : std::string_view{};
                    auto key = compute_expand_key(
                        input_.shapes[idx], input_.rows[idx], uuid);
                    bool clickable = is_row_clickable(
                        input_.shapes[idx], input_.rows[idx]);
                    bool expanded = !key.empty() &&
                        input_.expanded_keys.count(key) > 0;
                    if (clickable || expanded) {
                        if (cbs_.on_toggle_expand && !key.empty())
                            cbs_.on_toggle_expand(key);
                        return true;
                    }
                }
            }
            if (cbs_.on_action)
                cbs_.on_action(vr->row_idx, ActionKind::Copy);
            return true;
        }

        // ------ c / r / d hotkeys ------------------------------------
        if (event == Event::Character('c')) return fire_action(ActionKind::Copy);
        if (event == Event::Character('r')) return fire_action(ActionKind::Regenerate);
        if (event == Event::Character('d')) return fire_action(ActionKind::Delete);

        // ------ Mouse: click to expand / select row -------------------
        // TS REF: Messages.tsx onItemClick (L564-571) +
        //   VirtualMessageList.tsx onClickK (L847-850) + onEnterK/onLeaveK
        //   (L851-856).  Each message row is a "clickable cell"; left-click
        //   toggles verbose expansion for truncated tool outputs / collapsed
        //   groups, and moves the keyboard cursor to that row.
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            const int mx = m.x;
            const int my = m.y;

            // Find which tracked row (if any) contains the mouse cursor.
            // Linear scan: typical viewport shows ≤60 rows after overscan,
            // so this is sub-millisecond.
            std::optional<std::size_t> hit_vi;
            for (std::size_t i = 0; i < tracked_boxes_.size(); ++i) {
                if (tracked_boxes_[i] && tracked_boxes_[i]->Contain(mx, my)) {
                    if (i < tracked_vi_.size()) {
                        hit_vi = tracked_vi_[i];
                    }
                    break;
                }
            }

            // Hover tracking: update hovered_vi_ on every mouse event so
            // visual feedback (underline / cursor hint) follows the cursor.
            // TS REF: VirtualMessageList.tsx onEnterK/onLeaveK hover state.
            if (hovered_vi_ != hit_vi) {
                hovered_vi_ = hit_vi;
                // Returning true would consume the event and prevent the
                // underlying component from receiving it.  We only consume
                // on actual clicks; hover changes are side-effects.
            }

            // Left-click released: toggle expansion AND move selection.
            if (m.button == Mouse::Left && m.motion == Mouse::Released) {
                if (!hit_vi.has_value()) return false;
                const std::size_t vi = *hit_vi;
                if (vi >= visible_rows_.size()) return false;

                const auto& vr = visible_rows_[vi];

                // Compact group: toggle group expand/collapse.
                if (vr.kind == VisibleRow::Kind::CompactGroup) {
                    // Also select the group row so keyboard cursor follows.
                    commit_selection(vi);
                    if (cbs_.on_toggle_compact_group)
                        cbs_.on_toggle_compact_group(vr.group_idx);
                    return true;
                }

                // Payload row: if clickable or already expanded, toggle.
                if (vr.kind == VisibleRow::Kind::Payload) {
                    const auto idx = vr.row_idx;
                    // Always move selection to the clicked row first.
                    commit_selection(vi);

                    if (idx < input_.shapes.size() && idx < input_.rows.size()) {
                        std::string_view uuid = (idx < input_.uuids.size())
                            ? std::string_view(input_.uuids[idx]) : std::string_view{};
                        auto key = compute_expand_key(
                            input_.shapes[idx], input_.rows[idx], uuid);
                        bool clickable = is_row_clickable(
                            input_.shapes[idx], input_.rows[idx]);
                        bool expanded = !key.empty() &&
                            input_.expanded_keys.count(key) > 0;
                        if (clickable || expanded) {
                            if (cbs_.on_toggle_expand && !key.empty())
                                cbs_.on_toggle_expand(key);
                            return true;
                        }
                    }
                    return true;  // click consumed (selection moved)
                }
            }
            // Non-click mouse events: don't consume; let parent components
            // (e.g. scroll wheel) handle them.
        }

        return ComponentBase::OnEvent(event);
}

Element MessagesListComponent::Render() {
        ++frame_count_;

        // Reset the row-tracking cursor.  We REUSE existing Box objects in
        // tracked_boxes_ rather than clearing them, because the previous
        // frame's element tree still holds Box& references via reflect().
        // Destroying those Boxes before the old tree is replaced would be a
        // use-after-free.  Instead: overwrite in place, then trim excess at
        // the end of Render() after the new tree is fully built.
        std::size_t track_pos = 0;
        auto push_tracked = [this, &track_pos](std::size_t vi) -> Box& {
            if (track_pos < tracked_boxes_.size()) {
                // Reuse existing Box (old tree's reflect ref will be
                // overwritten by new tree's layout pass).
                tracked_vi_[track_pos] = vi;
                Box& b = *tracked_boxes_[track_pos];
                ++track_pos;
                return b;
            }
            tracked_vi_.push_back(vi);
            tracked_boxes_.push_back(std::make_unique<Box>());
            Box& b = *tracked_boxes_.back();
            ++track_pos;
            return b;
        };
        auto trim_tracked = [this, &track_pos]() {
            if (track_pos < tracked_vi_.size()) {
                tracked_vi_.erase(tracked_vi_.begin() + track_pos,
                                  tracked_vi_.end());
                tracked_boxes_.erase(tracked_boxes_.begin() + track_pos,
                                     tracked_boxes_.end());
            }
        };

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
            trim_tracked();  // no rows → clear all tracked entries
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
                [&push_tracked, fc, &in_ref, &vis_rows_copy, sel_copy,
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
                    } else if (ml_row.kind == VisibleRow::Kind::TranscriptCapDivider) {
                        row_el = detail::render_transcript_cap_divider(
                            ml_row.hidden_count);
                    } else {
                        row_el = detail::render_payload_row(
                            in_ref, ml_row.row_idx, is_selected, fc,
                            /*add_margin=*/true);
                    }

                    // Track this virtual row's screen box for mouse
                    // click-to-expand.  TS REF: VirtualMessageList.tsx
                    //   measureRef + onClickK hit-testing.
                    Box& vbox_ref = push_tracked(row_index);

                    // TS REF: Messages.tsx L631-635  insert divider BEFORE
                    // the target row.  row_index is 0..rows.size()-1, which
                    // maps 1:1 to visible_rows_[] order.
                    if (has_divider && row_index == divider_before_vi) {
                        return vbox({
                            detail::render_unseen_divider(
                                in_ref.unseen_divider->count),
                            std::move(row_el),
                        }) | reflect(vbox_ref);
                    }
                    return row_el | reflect(vbox_ref);
                };

            list_body = vl::render_list_as_elements(state) | flex;
            trim_tracked();
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
            bool prev_was_user = false;
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

                // Track this row's screen box for mouse click-to-expand.
                // TS REF: VirtualMessageList.tsx VirtualItem — each item has
                //   a measured Box used for onClick hit-testing.
                Box& row_box = push_tracked(vi);

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
                         shape == S::UserCommand ||
                         shape == S::UserImage);
                    // TS VISUAL PARITY (2026-07-04): tool results are part of
                    // the assistant's visual turn — same as static path above.
                    const bool is_tool_result = (shape == S::UserToolResult);
                    const bool is_same_turn_as_assistant =
                        is_assistant_block || is_tool_result;
                    const bool is_turn_boundary = is_user_row ||
                        (!is_same_turn_as_assistant && !is_tool_result);

                    // Match static path: user rows use prev_was_user for ⎿,
                    // turn boundaries get true, same-turn blocks use next_add_margin.
                    const bool row_add_margin = is_user_row
                        ? !prev_was_user
                        : (is_turn_boundary ? true : next_add_margin);
                    prev_was_user = is_user_row;

                    if (is_turn_boundary) {
                        next_add_margin = !is_user_row;
                    } else {
                        next_add_margin = false;
                    }

                    rows.push_back(detail::render_payload_row(
                        input_, vr.row_idx, is_selected, frame_count_, row_add_margin)
                        | reflect(row_box));
                } else if (vr.kind == VisibleRow::Kind::TranscriptCapDivider) {
                    rows.push_back(
                        detail::render_transcript_cap_divider(vr.hidden_count)
                        | reflect(row_box));
                    next_add_margin = true;
                } else {
                    rows.push_back(
                        detail::render_compact_group_row(vr, is_selected)
                        | reflect(row_box));
                    next_add_margin = true;
                }
            }
            trim_tracked();
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

void MessagesListComponent::set_input(MessagesListInput next) {
        input_ = std::move(next);
        rebuild_visible_cache();
}

void MessagesListComponent::rebuild_visible_cache() {
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

auto MessagesListComponent::filter_hash() const -> std::uint64_t {
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

void MessagesListComponent::move_selection(int delta) {
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

void MessagesListComponent::move_selection_to(std::size_t abs_idx) {
        if (visible_rows_.empty()) return;
        if (abs_idx >= visible_rows_.size()) abs_idx = visible_rows_.size() - 1;
        commit_selection(abs_idx);
}

void MessagesListComponent::commit_selection(std::size_t visible_idx) {
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

auto MessagesListComponent::current_visible_row() -> const VisibleRow* {
        if (!selected_visible_index_) return nullptr;
        if (*selected_visible_index_ >= visible_rows_.size()) return nullptr;
        return &visible_rows_[*selected_visible_index_];
}

auto MessagesListComponent::fire_action(ActionKind k) -> bool {
        auto* vr = current_visible_row();
        if (!vr || vr->kind != VisibleRow::Kind::Payload) return false;
        if (cbs_.on_action) { cbs_.on_action(vr->row_idx, k); }
        return true;
}

Component MakeMessagesList(
    MessagesListInput input,
    MessagesListCallbacks callbacks) {
    return Make<MessagesListComponent>(std::move(input), std::move(callbacks));
}

} // namespace cc::ui::messages_list
