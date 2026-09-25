// messages_list_geometry.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). Per-row line-height heuristics and the
// VisibleRow <-> virtual_list::VisibleRow encoding (backend_index bit pack).
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
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

module cc.ui.messages.messages_list;

import cc.ui.messages.message_row;
import cc.ui.messages.virtual_list;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.local_command_output_message;
import cc.ui.messages.thinking_message;

namespace cc::ui::messages_list {

namespace detail {

/// Count *wrapped* lines for `text` given terminal columns.  Mirrors TS
/// text-wrap heuristic (hard-break at term_cols, plus existing '\n').  The
/// result is the maximum vertical space the content COULD take inside a
/// 36-col reserved left-gutter message envelope; 36 is subtracted from
/// term_cols to account for the fixed avatar column.
[[nodiscard]] auto estimate_content_lines(
    std::string_view text,
    int term_cols,
    int envelope_gutter_cols) -> int {
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
[[nodiscard]] auto estimate_row_height(
    const VisibleRow& vr,
    const MessagesListInput& input,
    int term_cols) -> int {
    using K = VisibleRow::Kind;
    if (vr.kind == K::CompactGroup) {
        // Collapsed "📦 27 messages collapsed (📦 8 tool turns, +++12 ---7)"
        return 1;
    }
    if (vr.kind == K::TranscriptCapDivider) {
        // "─── N older messages hidden · Ctrl+E to show all ───"
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
        case S::AssistantGroupedTools: {
            if (auto* topts = std::get_if<tool_use_message::ToolUseRenderOptions>(
                    &input.rows[vr.row_idx])) {
                const auto& call = topts->call;
                // Resolved built-in tools render as just a 1-line header
                // (● ToolName (command)) — no Input/Output sections.
                const bool is_resolved =
                    (call.status == tool_use_message::ToolStatus::Success ||
                     call.status == tool_use_message::ToolStatus::Error ||
                     call.status == tool_use_message::ToolStatus::Cancelled);
                if (is_resolved) {
                    content_lines = 1;
                } else {
                    // Running/Pending: header + progress line
                    content_lines = 2;
                }
            } else if (auto* gopts = std::get_if<tool_use_message::GroupedToolsOptions>(
                           &input.rows[vr.row_idx])) {
                int visible = std::min(
                    static_cast<int>(gopts->calls.size()),
                    gopts->max_visible_preview);
                content_lines = 2 + visible;
            } else {
                content_lines = 2;
            }
            break;
        }
        case S::UserToolResult:
        case S::UserBashOutput: {
            // TS PARITY (2026-07-05): payload_preview returns just tool_name
            // (e.g. "Bash") — 1 line.  Actual tool result output can be
            // dozens of lines.  Extract real output from ToolResultOptions.
            if (auto* ropts = std::get_if<ToolResultOptions>(
                    &input.rows[vr.row_idx])) {
                std::string full_text;
                if (ropts->content_items && !ropts->content_items->empty()) {
                    // Structured content items (MCP tools): concatenate text.
                    for (const auto& item : *ropts->content_items) {
                        if (item.type == "text") {
                            if (!full_text.empty()) full_text += '\n';
                            full_text += item.text;
                        } else if (item.type == "image") {
                            if (!full_text.empty()) full_text += '\n';
                            full_text += "[Image]";
                        }
                    }
                } else if (ropts->output && !ropts->output->empty()) {
                    full_text = *ropts->output;
                } else if (ropts->error_message && !ropts->error_message->empty()) {
                    full_text = *ropts->error_message;
                }
                if (!full_text.empty()) {
                    content_lines = estimate_content_lines(full_text, term_cols, 4);
                } else {
                    content_lines = 2;  // minimal: header + "(no output)"
                }
                // +1 for header row (status icon + tool name + duration)
                content_lines += 1;
                if (ropts->is_truncated) content_lines += 1;  // "(output truncated)"
            } else {
                // Fallback for UserBashOutput (BashIOEntry variant)
                content_lines = estimate_content_lines(preview, term_cols, 4);
            }
            break;
        }
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
            content_lines = 4;   // label + metadata + source (no fake thumbnail)
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

/// Convert a messages_list `VisibleRow` vector into the format consumed by
/// VirtualMessageList.  Each entry carries:
///   - row_id           stable hash (for future cache invalidation)
///   - estimated_hl     content-aware estimate (lines)
///   - search_key       lowered preview text (for scroll_search callback)
///   - type_hint        0 = Payload, 1 = CompactGroup
///   - backend_index    round-trip key for render_row callback
[[nodiscard]] auto visible_rows_to_virtual(
    const std::vector<VisibleRow>& visible,
    const MessagesListInput& input,
    int term_cols)
    -> std::vector<cc::ui::messages::virtual_list::VisibleRow>
{
    namespace vl = cc::ui::messages::virtual_list;
    std::vector<vl::VisibleRow> out;
    out.reserve(visible.size());
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const auto& vr = visible[i];
        const int est = detail::estimate_row_height(vr, input, term_cols);

        // Encode kind into backend_index MSBs for round-trip via
        // decode_virtual_backend_index.  Bit 63 = CompactGroup,
        // bit 62 = TranscriptCapDivider, neither = Payload.
        constexpr std::uint64_t kGroupBit  = std::uint64_t(1) << 63;
        constexpr std::uint64_t kCapBit    = std::uint64_t(1) << 62;
        std::uint64_t backend;
        int type_hint;

        if (vr.kind == VisibleRow::Kind::CompactGroup) {
            backend   = kGroupBit | static_cast<std::uint64_t>(vr.group_idx);
            type_hint = 1;
        } else if (vr.kind == VisibleRow::Kind::TranscriptCapDivider) {
            backend   = kCapBit | static_cast<std::uint64_t>(vr.hidden_count);
            type_hint = 2;
        } else {
            backend   = static_cast<std::uint64_t>(vr.row_idx);
            type_hint = 0;
        }

        vl::VisibleRow row{
            .row_id                 = static_cast<std::uint64_t>(i) + 1,
            .estimated_height_lines = est,
            .height_measured        = false,
            .search_key             = {},
            .type_hint              = type_hint,
            .backend_index          = backend,
        };
        if (row.type_hint == 0 && vr.row_idx < input.rows.size()) {
            // Populate search_key using the cached lowered rich text.
            // If build_visible_rows already warmed the cache (search was
            // active), this is a zero-alloc cache hit.  Otherwise this
            // call computes + caches for future use.
            //
            // TS REF: Messages.tsx L700  extractSearchText passed to VirtualMessageList
            //   (the same callback used by build_visible_rows search filter).
            const MessageShape shape =
                vr.row_idx < input.shapes.size()
                    ? input.shapes[vr.row_idx]
                    : MessageShape::SystemTaskAssignment;
            row.search_key = detail::get_cached_lowered_search_text(
                input, vr.row_idx, shape);
        }
        out.push_back(std::move(row));
    }
    return out;
}

/// Decode the backend_index set by `visible_rows_to_virtual` back into a
/// messages_list::VisibleRow.  `out` is filled in place; returns true if
/// decode succeeded, false if the key was malformed (out is reset to a
/// safe payload(0) sentinel on failure).
[[nodiscard]] bool decode_virtual_backend_index(
    std::uint64_t backend_index,
    VisibleRow& out) noexcept
{
    constexpr std::uint64_t kGroupBit = std::uint64_t(1) << 63;
    constexpr std::uint64_t kCapBit   = std::uint64_t(1) << 62;
    if ((backend_index & kGroupBit) != 0) {
        out.kind      = VisibleRow::Kind::CompactGroup;
        out.group_idx = backend_index & (~kGroupBit);
        out.row_idx   = 0;
        return true;
    }
    if ((backend_index & kCapBit) != 0) {
        out.kind         = VisibleRow::Kind::TranscriptCapDivider;
        out.hidden_count = backend_index & (~kCapBit);
        out.row_idx      = 0;
        out.group_idx    = 0;
        return true;
    }
    out.kind    = VisibleRow::Kind::Payload;
    out.row_idx = static_cast<std::size_t>(backend_index);
    out.group_idx = 0;
    return true;
}

} // namespace cc::ui::messages_list
