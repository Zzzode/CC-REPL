// repl_screen_scroll.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). unseen-divider computation, visible-message projection, transcript row
// estimation and scroll bounds (incl. the virtual_list JumpHandle fast path).
//
// Phase A (#184957): std is TEXTUAL here on purpose - no `import std;`.
// Empty-GMF impl unit of an FTXUI-GMF primary: clang 22's reduced-BMI
// writer would otherwise emit a duplicate aligned operator new
// (LLVM #184957). Same fallback as app_extra_methods.cpp /
// messages_list_geometry.cpp.
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module cc.ui.screens.repl_screen;

import cc.ui.messages.messages_list;
import cc.ui.messages.virtual_list;

namespace cc::ui::repl_screen {

// ── UnseenDivider helpers ──────────────────────────────────────────────
// TS REF: FullscreenLayout.tsx countUnseenAssistantTurns (L200-216) +
//         computeUnseenDivider (L239-256).  Counts new assistant turns
//         that arrived after the user scrolled away from bottom, and
//         builds the UnseenDivider struct passed to RenderMessages.

namespace unseen_detail {

/// Whether an assistant entry has visible text content (TS:
/// assistantHasVisibleText L217-223).  Tool-use-only and thinking-only
/// entries don't count as "new messages" to the user.
[[nodiscard]] bool assistant_has_visible_text(
    const MessageDisplayEntry& e) {
    if (e.role != "assistant") return false;
    // Tool-use entries: never have visible text content to the user.
    // TS: tool_use blocks are not 'text' type, so they fail the check.
    if (e.is_tool_use) return false;
    // Thinking entries: TS checks for b.type === 'text', not 'thinking'.
    // Thinking blocks don't count as visible text for turn counting.
    if (e.is_thinking) return false;
    // If we reach here, it's an assistant text entry with content.
    return !e.content_preview.empty();
}

/// Count assistant turns in entries[start_idx..end).  A "turn" is a
/// non-assistant→assistant transition, skipping system/progress rows
/// and tool-use-only entries (TS REF: countUnseenAssistantTurns L200).
[[nodiscard]] std::size_t count_unseen_assistant_turns(
    const std::vector<MessageDisplayEntry>& entries,
    std::size_t start_idx) {
    std::size_t count = 0;
    bool prev_was_assistant = false;
    for (std::size_t i = start_idx; i < entries.size(); ++i) {
        const auto& e = entries[i];
        // Skip system rows (TS: progress type)
        if (e.role == "system") continue;
        // Skip tool-use-only assistant entries
        if (e.role == "assistant" && !assistant_has_visible_text(e)) {
            continue;  // don't update prev_was_assistant
        }
        const bool is_assistant = (e.role == "assistant");
        if (is_assistant && !prev_was_assistant) ++count;
        prev_was_assistant = is_assistant;
    }
    return count;
}

}  // namespace unseen_detail

/// Compute the UnseenDivider from state.divider_index + messages.
/// Returns nullopt when divider_index is unset, out of range, or no
/// messages have arrived past the divider.  TS REF: computeUnseenDivider
/// (FullscreenLayout.tsx L239-256).
[[nodiscard]] std::optional<::cc::ui::messages_list::UnseenDivider>
ComputeUnseenDivider(const ReplScreenState& s) {
    if (!s.divider_index.has_value()) return std::nullopt;
    const auto idx = *s.divider_index;
    if (idx >= s.messages.size()) return std::nullopt;

    // Find first non-system entry at or after divider_index (TS: anchorIdx
    // skips progress + null attachments).
    std::size_t anchor_idx = idx;
    while (anchor_idx < s.messages.size() &&
           s.messages[anchor_idx].role == "system") {
        ++anchor_idx;
    }
    if (anchor_idx >= s.messages.size()) return std::nullopt;

    const auto& anchor = s.messages[anchor_idx];
    const std::size_t count = std::max(
        std::size_t{1},
        unseen_detail::count_unseen_assistant_turns(s.messages, idx));

    ::cc::ui::messages_list::UnseenDivider ud;
    ud.first_unseen_uuid_prefix = anchor.id;
    ud.count = count;
    return ud;
}

[[nodiscard]] std::vector<MessageDisplayEntry> BuildVisibleMessages(
    const ReplScreenState& s) {
    auto entries = s.messages;
    if (!s.active_local_jsx_command) return entries;

    MessageDisplayEntry command;
    command.role = "user";
    command.is_local_command_input = true;
    command.content_preview = "/" + s.active_local_jsx_command_name;
    if (!s.active_local_jsx_command_args.empty()) {
        command.content_preview += " " + s.active_local_jsx_command_args;
    }
    command.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(command));

    std::string content = s.active_local_jsx_content;
    if (!content.empty() && content.back() != '\n') content.push_back('\n');
    if (content.find("Esc to close") == std::string::npos &&
        content.find("Esc to go back") == std::string::npos) {
        content += "\nEsc to close";
    }
    MessageDisplayEntry local_jsx;
    local_jsx.role = "system";
    local_jsx.is_local_jsx_output = true;
    local_jsx.content_preview = std::move(content);
    local_jsx.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(local_jsx));

    return entries;
}

[[nodiscard]] int CountTextLines(std::string_view text) {
    if (text.empty()) return 1;
    return static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
}

[[nodiscard]] int EstimateTranscriptRows(
    const std::vector<MessageDisplayEntry>& entries) {
    int rows = 0;
    for (const auto& entry : entries) {
        // TS PARITY (2026-07-05): content_preview for tool entries is often
        // just a short label ("Bash", "tool-use") while actual rendered
        // content can be dozens of lines.  Extract real content for accurate
        // scroll bounds (fixes "can't scroll to latest message" bug).
        int content_lines = 0;
        if (entry.is_tool_use) {
            // Tool-use card: header + input JSON + optional result preview.
            std::string combined;
            if (entry.tool_input_json && !entry.tool_input_json->empty()) {
                combined += *entry.tool_input_json;
            }
            if (entry.tool_result_preview && !entry.tool_result_preview->empty()) {
                if (!combined.empty()) combined += '\n';
                combined += *entry.tool_result_preview;
            }
            if (combined.empty()) combined = entry.content_preview;
            content_lines = CountTextLines(combined);
            // Cap input lines at 8 (collapsed args show first few) + 2 for chrome
            content_lines = std::min(content_lines, 8) + 3;
        } else if (entry.tool_result_content_items &&
                   !entry.tool_result_content_items->empty()) {
            // Structured tool result (MCP): concatenate text items.
            std::string full;
            for (const auto& item : *entry.tool_result_content_items) {
                if (item.type == "text") {
                    if (!full.empty()) full += '\n';
                    full += item.text;
                } else if (item.type == "image") {
                    if (!full.empty()) full += '\n';
                    full += "[Image]";
                }
            }
            content_lines = CountTextLines(full);
            content_lines += 2;  // header + status row
        } else if (entry.is_image) {
            content_lines = 4;  // label + metadata rows (no fake thumbnail)
        } else {
            content_lines = CountTextLines(entry.content_preview);
        }
        rows += content_lines;
        // Message list inserts one empty separator after each rendered row
        // (marginTop from addMargin=true).  Tool results skip this (flush).
        rows += 1;
    }
    return rows;
}

bool ScrollTranscript(const std::shared_ptr<ReplScreenState>& state,
                             int delta) {
    if (!state || delta == 0) return false;

    const int viewport_rows = std::max(1, state->viewport_height_lines);

    // P0-3 path: if the VirtualMessageList is active for this frame, use
    // its JumpHandle (prefix-sum table of exact visual lines) for O(log N)
    // scroll bounds instead of the crude EstimateTranscriptRows heuristic.
    if (state->virtual_list_active) {
        namespace vl = cc::ui::messages::virtual_list;
        const vl::JumpHandle& jh = state->virtual_jh;
        const int total_lines = jh.total();
        if (total_lines <= viewport_rows) return false;
        const int max_top = total_lines - viewport_rows;

        const int old_top = std::clamp(state->scroll_offset, 0, max_top);
        int target = old_top + delta;
        // Guarantee at least one row moves on PageUp/PageDown style deltas:
        // if target equals old_top, step by one row in the requested
        // direction using binary search.
        if (delta > 0 && target <= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur + 1 < jh.size()) target = jh.find_visual_top_for_row(cur + 1);
        } else if (delta < 0 && target >= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur > 0) target = jh.find_visual_top_for_row(cur - 1);
        }
        target = std::clamp(target, 0, max_top);
        if (target == old_top) return false;

        state->scroll_offset = target;
        const bool was_pinned = state->scroll_pinned_to_bottom;
        state->scroll_pinned_to_bottom = (target >= max_top);

        // TS REF: useUnseenDivider onScrollAway — on FIRST scroll-away from
        // bottom, snapshot message count as divider_index.  On repin, clear.
        if (was_pinned && !state->scroll_pinned_to_bottom) {
            state->divider_index = state->messages.size();
            state->message_count_at_scroll_away = state->messages.size();
        } else if (!was_pinned && state->scroll_pinned_to_bottom) {
            state->divider_index.reset();
            state->unseen_divider.reset();
            state->unseen_message_count = 0;
            state->pill_visible = false;
        }

        // If we are maintaining a live VirtualListState (Component-mode
        // wiring), also update its scroll_top so Render() reuses it.
        if (state->virtual_list_state) {
            state->virtual_list_state->scroll_top = target;
            vl::update_sticky_after_scroll(*state->virtual_list_state,
                                            old_top);
        }
        return true;
    }

    const auto visible_messages = BuildVisibleMessages(*state);
    if (visible_messages.empty()) return false;
    const int max_offset =
        std::max(0, EstimateTranscriptRows(visible_messages) - viewport_rows);
    if (max_offset == 0) return false;

    const int next =
        std::clamp(state->scroll_offset + delta, 0, max_offset);
    state->scroll_offset = next;
    const bool was_pinned = state->scroll_pinned_to_bottom;
    state->scroll_pinned_to_bottom = next >= max_offset;
    // TS REF: useUnseenDivider onScrollAway/onRepin.
    if (was_pinned && !state->scroll_pinned_to_bottom) {
        state->divider_index = state->messages.size();
        state->message_count_at_scroll_away = state->messages.size();
    } else if (!was_pinned && state->scroll_pinned_to_bottom) {
        state->divider_index.reset();
        state->unseen_divider.reset();
        state->unseen_message_count = 0;
        state->pill_visible = false;
    }
    return true;
}

}  // namespace cc::ui::repl_screen
