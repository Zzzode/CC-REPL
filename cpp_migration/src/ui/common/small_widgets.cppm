/// @file small_widgets.cppm
/// @brief Lightweight presentational widgets (<= 100 lines each).
///
/// Consolidates small FTXUI renderers equivalent to these TS/React components
/// (all <= 100 lines, no complex hooks, no I/O):
///   - Source: src/components/InterruptedByUser.tsx (14 lines → merged here)
///   - Source: src/components/PressEnterToContinue.tsx (14 lines → merged here)
///   - Source: src/components/MCPServerDialogCopy.tsx (14 lines → merged here)
///   - Source: src/components/messages/CompactBoundaryMessage.tsx (17 lines → merged here)
///   - Source: src/components/ui/OrderedListItem.tsx (44 lines → merged here)
///   - Source: src/components/ui/OrderedList.tsx (70 lines → merged here)
///   - Source: src/components/FilePathLink.tsx (42 lines → merged here)
///   - Source: src/components/MessageModel.tsx (42 lines → merged here)
///   - Source: src/components/StructuredDiffList.tsx (29 lines → merged here)
///   - Source: src/components/FallbackToolUseRejectedMessage.tsx (15 lines → merged here)
///   - Source: src/components/messages/UserToolResultMessage/RejectedToolUseMessage.tsx (15 lines → merged here)
///   - Source: src/components/messages/UserToolResultMessage/UserToolCanceledMessage.tsx (15 lines → merged here)
///   - Source: src/components/messages/UserToolResultMessage/RejectedPlanMessage.tsx (30 lines → merged here)
///   - Source: src/components/messages/AssistantRedactedThinkingMessage.tsx (30 lines → merged here)
module;

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.common.widgets;

import cc.ui.common.types;
import cc.ui.common.formatting;

export namespace cc::ui::common::widgets {
using namespace ftxui;
using cc::ui::common::formatting::trim;

// ============================================================
// From: src/components/InterruptedByUser.tsx
// ============================================================
/// Renders the dimmed "Interrupted · What should Claude do instead?" line.
[[nodiscard]] inline Element render_interrupted_by_user() {
    return hbox({
        text("Interrupted ") | dim,
        text("· What should Claude do instead?") | dim,
    });
}

// ============================================================
// From: src/components/PressEnterToContinue.tsx
// ============================================================
/// Renders the "Press Enter to continue…" prompt line.
[[nodiscard]] inline Element render_press_enter_to_continue() {
    return hbox({
        text("Press ") | color(Color::BlueLight),
        text("Enter") | color(Color::BlueLight) | bold,
        text(" to continue…") | color(Color::BlueLight),
    });
}

// ============================================================
// From: src/components/MCPServerDialogCopy.tsx
// ============================================================
/// Renders the standard MCP server disclaimer paragraph shown above approval
/// dialogs.
[[nodiscard]] inline Element render_mcp_server_disclaimer() {
    return vbox({
        paragraphAlignLeft(
            "MCP servers may execute code or access system resources. "
            "All tool calls require approval. Learn more in the MCP "
            "documentation (https://code.claude.com/docs/en/mcp).") |
            color(Color::GrayLight),
    });
}

// ============================================================
// From: src/components/messages/CompactBoundaryMessage.tsx
// ============================================================
/// Renders the "Conversation compacted (ctrl+o for history)" divider line.
[[nodiscard]] inline Element render_compact_boundary(
    std::string_view history_shortcut = "ctrl+o") {
    return vbox({
        separatorEmpty(),
        hbox({
            text("✻ "),
            text(std::format("Conversation compacted ({} for history)",
                             history_shortcut)) | dim,
        }),
        separatorEmpty(),
    });
}

// ============================================================
// From: src/components/ui/OrderedListItem.tsx + OrderedList.tsx
// ============================================================
struct OrderedListEntry {
    std::string marker;
    Element content;
};

/// Renders a single ordered-list item with marker prefix.
[[nodiscard]] inline Element render_ordered_list_item(std::string_view marker,
                                                      Element content) {
    return hbox({
        text(std::string(marker) + " ") | dim,
        std::move(content),
    });
}

/// Renders a numbered ordered list. Caller supplies entries with a numeric
/// prefix; multi-level nesting is achieved by appending "1.2." style markers
/// at the call site (same semantics as the React Context-based variant).
[[nodiscard]] inline Element render_ordered_list(
    std::vector<OrderedListEntry> entries) {
    if (entries.empty()) return emptyElement();
    std::vector<Element> rows;
    rows.reserve(entries.size());
    // Compute maximum marker width for alignment.
    size_t max_w = 0;
    for (auto& e : entries) max_w = std::max(max_w, e.marker.size());
    for (auto& e : entries) {
        std::string padded(e.marker);
        if (padded.size() < max_w) padded.insert(0, max_w - padded.size(), ' ');
        rows.push_back(render_ordered_list_item(padded, std::move(e.content)));
    }
    return vbox(std::move(rows));
}

// ============================================================
// From: src/components/FilePathLink.tsx
// ============================================================
/// Renders a file path as a clickable hyperlink when the terminal supports
/// OSC 8; otherwise falls back to the path as plain text.
[[nodiscard]] inline Element render_file_path_link(
    std::string_view file_path,
    std::optional<std::string_view> display_text = std::nullopt) {
    const std::string shown = display_text ? std::string(*display_text)
                                           : std::string(file_path);
    // FTXUI's `link` decorator wraps text with OSC 8 sequences automatically.
    try {
        const auto p = std::filesystem::path(file_path);
        return text(shown) | link("file://" + p.string()) | color(Color::Cyan);
    } catch (...) {
        return text(shown) | color(Color::Cyan);
    }
}

// ============================================================
// From: src/components/MessageModel.tsx
// ============================================================
struct MessageModelInfo {
    std::string model;
    bool has_text_content = false;
    bool is_transcript_mode = false;
};

/// Renders the dimmed model-name badge that appears in transcript mode next to
/// assistant messages that contain text.
[[nodiscard]] inline Element render_message_model(
    const MessageModelInfo& info) {
    if (!info.is_transcript_mode || !info.has_text_content || info.model.empty()) {
        return emptyElement();
    }
    return text(info.model) | dim | size(WIDTH, GREATER_THAN,
                                         static_cast<int>(info.model.size()) + 8);
}

// ============================================================
// From: src/components/StructuredDiffList.tsx
// ============================================================
/// Renders a list of rendered diff hunks with "..." separator lines between
/// non-adjacent hunks. Equivalent to `intersperse(hunks.map(render), ...)`.
[[nodiscard]] inline Element render_structured_diff_list(
    std::vector<Element> rendered_hunks) {
    if (rendered_hunks.empty()) return emptyElement();
    std::vector<Element> parts;
    parts.reserve(rendered_hunks.size() * 2 - 1);
    for (size_t i = 0; i < rendered_hunks.size(); ++i) {
        parts.push_back(vbox({std::move(rendered_hunks[i])}));
        if (i + 1 < rendered_hunks.size()) {
            parts.push_back(text("...") | dim);
        }
    }
    return vbox(std::move(parts));
}

// ============================================================
// Simple "MessageResponse" wrapper (1-line bordered row)
// ============================================================
/// Thin wrapper that places content inside a 1-2 line message-response row.
/// Mirrors the React <MessageResponse height={N}> wrapper.
[[nodiscard]] inline Element render_message_response(Element content,
                                                     int height_hint = 1) {
    (void)height_hint;
    return vbox({std::move(content)});
}

// ============================================================
// From: src/components/FallbackToolUseRejectedMessage.tsx
//       + UserToolCanceledMessage.tsx  (identical output)
// ============================================================
/// Renders "Interrupted · …" inside a message-response wrapper.
/// Used by both FallbackToolUseRejected and UserToolCanceled variants.
[[nodiscard]] inline Element render_tool_canceled_message() {
    return render_message_response(render_interrupted_by_user(), 1);
}

// Alias for the Fallback variant (same rendering path in TS).
[[nodiscard]] inline Element render_fallback_tool_rejected_message() {
    return render_tool_canceled_message();
}

// ============================================================
// From: src/components/messages/UserToolResultMessage/RejectedToolUseMessage.tsx
// ============================================================
/// Renders the dim "Tool use rejected" row shown when the user denies a single
/// tool invocation.
[[nodiscard]] inline Element render_rejected_tool_use_message() {
    return render_message_response(
        text("Tool use rejected") | dim,
        1);
}

// ============================================================
// From: src/components/messages/UserToolResultMessage/RejectedPlanMessage.tsx
// ============================================================
/// Renders the plan-rejection card: subtle header + rounded bordered plan body.
[[nodiscard]] inline Element render_rejected_plan_message(
    std::string_view plan_text,
    Element rendered_plan_body) {
    return render_message_response(
        vbox({
            text("User rejected Claude's plan:") | color(Color::GrayLight),
            vbox({
                std::move(rendered_plan_body),
            }) | borderRounded | borderColor(Color::Purple4Bit) |
               size(WIDTH, GREATER_THAN, 40),
        }));
}

// ============================================================
// From: src/components/messages/AssistantRedactedThinkingMessage.tsx
// ============================================================
/// Renders the "✻ Thinking…" dim italic placeholder used when redacted
/// thinking is present but not expanded.
[[nodiscard]] inline Element render_redacted_thinking_placeholder(
    bool add_margin = false) {
    auto inner = text("✻ Thinking…") | dim | color(Color::GrayLight);
    if (add_margin) {
        return vbox({separatorEmpty(), std::move(inner)});
    }
    return inner;
}

} // namespace cc::ui::common::widgets
