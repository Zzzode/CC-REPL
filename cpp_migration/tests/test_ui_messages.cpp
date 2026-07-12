/// @file test_ui_messages.cpp
/// @brief Split from test_ui.cpp - CollapseBackgroundBash, ImagePaste, ImagePasteCtrlV, ImagePasteFormat, ImagePasteOrphanCleanup, ImagePasteSubmit, MessagePipeline, Messages, MessagesList, VirtualList (SLOC budget fix)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <expected>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <gtest/gtest.h>
#include <httplib.h>

#include "test_ui_helpers.h"

import cc.ui.messages;
import cc.ui.messages.message_pipeline;
import cc.ui.messages.collapse_background_bash;
import cc.ui.messages.virtual_list;
import cc.ui.messages.messages_list;
import cc.ui.messages.message_row;
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.message_image;
import cc.ui.markdown;
import cc.ui.design.tokens;
import cc.ui.design.figures;
import cc.constants.constants;
import cc.utils.parse_references;
import cc.ui.components;
import cc.ui.components_extended;
import cc.types.types;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Messages, ParseMarkdownRecognizesHeadingListAndCodeBlock) {
    auto blocks = cc::ui::parse_markdown("# Title\n- item\n```cpp\nint main() {}\n```");

    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].type, cc::ui::BlockType::Heading);
    EXPECT_EQ(blocks[1].type, cc::ui::BlockType::List);
    EXPECT_EQ(blocks[2].type, cc::ui::BlockType::CodeBlock);
    EXPECT_EQ(blocks[2].language, "cpp");
}

TEST(Messages, ToolUseViewFormatsStatusAndDuration) {
    cc::ui::ToolUseView view{
        .tool_name = "bash",
        .tool_input = "{}",
        .tool_output = "done",
        .status = cc::ui::ToolStatus::Success,
        .started_at = std::chrono::system_clock::now(),
        .completed_at = std::nullopt,
        .expanded = false,
        .duration_ms = 1200,
    };

    EXPECT_EQ(view.status_icon(), "✓");
    EXPECT_EQ(view.duration_display(), "1.2s");
    view.toggle_expand();
    EXPECT_TRUE(view.expanded);
}

TEST(Messages, ErrorViewFormatsErrorCode) {
    cc::ui::ErrorView error{
        .message = "failed",
        .error_code = "E_TEST",
        .timestamp = std::chrono::system_clock::now(),
        .suggestion = std::nullopt,
        .is_retryable = false,
    };

    EXPECT_EQ(error.formatted(), "[E_TEST] failed");
}

TEST(Messages, ThinkingViewCanToggleCollapse) {
    cc::ui::ThinkingView view{
        .content = "first line\nsecond line",
        .timestamp = std::chrono::system_clock::now(),
        .collapsed = true,
        .token_count = 0,
    };

    EXPECT_TRUE(view.summary().starts_with("first line"));
    view.toggle_collapse();
    EXPECT_FALSE(view.collapsed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.prompt_input: prompt buffer, history, typeahead, vim behavior
// ═══════════════════════════════════════════════════════════════════════════════

namespace pl = cc::ui::messages::pipeline;

TEST(MessagePipeline, DedupStartDeltaStopSmoke) {
    pl::DedupTracker t;
    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kNotSeen);

    EXPECT_TRUE(t.should_accept_start(0));
    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kOpen);

    // Duplicate Start while Open → accept (still open, no harm).
    EXPECT_TRUE(t.should_accept_start(0));
    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kOpen);

    // Deltas while Open → accept.
    EXPECT_TRUE(t.should_accept_delta(0));
    EXPECT_TRUE(t.should_accept_delta(0));

    // First Stop → accept and transition to terminal.
    EXPECT_TRUE(t.should_accept_stop(0));
    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kStopped);

    // Post-Stop: Start / Delta / Stop all dedup dropped.
    EXPECT_FALSE(t.should_accept_start(0)) << "replay Start after Stop must be dropped";
    EXPECT_FALSE(t.should_accept_delta(0)) << "Delta after Stop must be dropped";
    EXPECT_FALSE(t.should_accept_stop(0))  << "Second Stop must be dropped";

    // Other indices are unaffected.
    EXPECT_TRUE(t.should_accept_start(1));
    EXPECT_TRUE(t.should_accept_start(42));
}

TEST(MessagePipeline, DedupDeltaBeforeStartPromotesToOpen) {
    pl::DedupTracker t;
    // Out-of-order Delta (server sent events in a wacky order) → should accept
    // and leave the index in Open state so that the subsequent Start doesn't
    // create a duplicate.
    EXPECT_TRUE(t.should_accept_delta(7));
    EXPECT_EQ(t.state_of(7), pl::DedupTracker::IndexState::kOpen);
    EXPECT_TRUE(t.should_accept_start(7));   // Start after Delta → dedup accept
}

TEST(MessagePipeline, DedupToolUseExecStartAndEndOneShot) {
    pl::DedupTracker t;
    EXPECT_TRUE(t.should_accept_exec_start("toolu_01abc"));
    EXPECT_TRUE(t.should_accept_exec_end("toolu_01abc"));
    // Repeated calls are dedup'd.
    EXPECT_FALSE(t.should_accept_exec_start("toolu_01abc"));
    EXPECT_FALSE(t.should_accept_exec_end("toolu_01abc"));
    // Different id is independent.
    EXPECT_TRUE(t.should_accept_exec_start("toolu_02def"));
}

TEST(MessagePipeline, DedupClearResetsAllState) {
    pl::DedupTracker t;
    (void)t.should_accept_start(0);
    (void)t.should_accept_stop(0);
    (void)t.should_accept_exec_start("abc");
    (void)t.should_accept_exec_end("abc");
    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kStopped);

    t.clear();

    EXPECT_EQ(t.state_of(0), pl::DedupTracker::IndexState::kNotSeen);
    EXPECT_TRUE(t.should_accept_start(0));
    EXPECT_TRUE(t.should_accept_exec_start("abc"));
}

// ── Stage 4 USER_INPUT_FILTER ──────────────────────────────────────────────
TEST(MessagePipeline, FilterPlainTextFastPath) {
    auto rows = pl::filter_user_text("hello world");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, pl::UserRowKind::kPlainText);
    EXPECT_EQ(rows[0].display_text, "hello world");
}

TEST(MessagePipeline, FilterBashInputTag) {
    auto rows = pl::filter_user_text("<bash-input>ls -la ~/Documents</bash-input>");
    ASSERT_GE(rows.size(), 1u);
    auto it = std::find_if(rows.begin(), rows.end(),
        [](const auto& r){ return r.kind == pl::UserRowKind::kBashInput; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->display_text, "ls -la ~/Documents");
}

TEST(MessagePipeline, FilterQuotedReply) {
    auto rows = pl::filter_user_text(
        "<quoted-reply># Old issue number 42\nsecond line</quoted-reply>\n"
        "Please take a look");
    ASSERT_GE(rows.size(), 1u);
    auto it = std::find_if(rows.begin(), rows.end(),
        [](const auto& r){ return r.kind == pl::UserRowKind::kQuotedReply; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->quoted_reply, "# Old issue number 42\nsecond line");
    EXPECT_EQ(it->display_text, "Please take a look");
}

TEST(MessagePipeline, FilterToolInvocation) {
    auto rows = pl::filter_user_text("<tool:BashTool>ls /tmp | head</tool:BashTool>");
    ASSERT_GE(rows.size(), 1u);
    auto it = std::find_if(rows.begin(), rows.end(),
        [](const auto& r){ return r.kind == pl::UserRowKind::kToolInvocation; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->tool_name, "BashTool");
    EXPECT_EQ(it->display_text, "ls /tmp | head");
}

TEST(MessagePipeline, FilterAtMentions) {
    auto rows = pl::filter_user_text(
        "Please review "
        "<at-file>src/main.cpp</at-file> and "
        "<at-agent>SeniorEngineer</at-agent>"
        "<at-tool>SearchTool</at-tool>");
    auto has_attach = [](const auto& r){ return r.kind == pl::UserRowKind::kAttachment; };
    const auto attachments = std::count_if(rows.begin(), rows.end(), has_attach);
    EXPECT_GE(attachments, 3u);
    bool found_file = false, found_agent = false, found_tool = false;
    for (const auto& r : rows) {
        if (r.kind != pl::UserRowKind::kAttachment) continue;
        if (r.attachment_ref == "@src/main.cpp") found_file = true;
        if (r.attachment_ref == "@SeniorEngineer") found_agent = true;
        if (r.attachment_ref == "@tool:SearchTool") found_tool = true;
    }
    EXPECT_TRUE(found_file);
    EXPECT_TRUE(found_agent);
    EXPECT_TRUE(found_tool);
}

TEST(MessagePipeline, FilterMixedContentProducesPlainTextRemainder) {
    // <bash-input> + extra plain text should also yield a plain-text row.
    auto rows = pl::filter_user_text(
        "<bash-input>make</bash-input>\n<at-file>Makefile</at-file>");
    // Expect at least: kBashInput + kAttachment.  Any extra whitespace/text
    // may collapse to plain-text row; verify no row has raw XML left.
    for (const auto& r : rows) {
        EXPECT_EQ(r.display_text.find('<'), std::string::npos)
            << "display_text must never contain raw XML: " << r.display_text;
        EXPECT_EQ(r.quoted_reply.find('<'), std::string::npos);
    }
    EXPECT_TRUE(std::any_of(rows.begin(), rows.end(),
        [](const auto& r){ return r.kind == pl::UserRowKind::kBashInput; }));
    EXPECT_TRUE(std::any_of(rows.begin(), rows.end(),
        [](const auto& r){ return r.kind == pl::UserRowKind::kAttachment; }));
}

TEST(MessagePipeline, FilterEmptyInputProducesSinglePlaceholder) {
    auto rows = pl::filter_user_text("   ");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].kind, pl::UserRowKind::kPlainText);
}

// ── Stage 5 TOOL_RESULT_AUGMENT ────────────────────────────────────────────
TEST(MessagePipeline, ToolAugment_TruncationFlagTriggersAboveThreshold) {
    std::string big(pl::kMaxToolPreviewBytes + 100, 'x');
    auto a = pl::augment_tool_result(big, /*is_error=*/false);
    EXPECT_TRUE(a.truncated);
    EXPECT_EQ(a.error_code, 0);
    EXPECT_FALSE(a.preview.empty());   // truncated or not, preview is set

    std::string small(100, 'a');
    auto b = pl::augment_tool_result(small, /*is_error=*/false);
    EXPECT_FALSE(b.truncated);
}

TEST(MessagePipeline, ToolAugment_ParseErrorCodeRecognisesPatterns) {
    // Pattern 1: "Error 42: ..."
    EXPECT_EQ(pl::parse_error_code("Error 42: something failed"), 42);
    // Pattern 2: "exit code: 127"
    EXPECT_EQ(pl::parse_error_code("exit code: 127\n..."), 127);
    EXPECT_EQ(pl::parse_error_code("Exit status = 1\n"), 1);
    EXPECT_EQ(pl::parse_error_code("[exit_code=7] Done"), 7);
    // Pattern 3: HTTP status codes
    EXPECT_EQ(pl::parse_error_code("HTTP 404 Not Found\n"), 404);
    EXPECT_EQ(pl::parse_error_code("http 500 internal server error"), 500);
    // No numeric prefix → 0
    EXPECT_EQ(pl::parse_error_code("Success!"), 0);
    EXPECT_EQ(pl::parse_error_code(""), 0);
}

TEST(MessagePipeline, ToolAugment_PreviewUsesFirstNonEmptyLine) {
    const std::string input = "\n\n  \nThis is line four with content.\nLine five ignored.\n";
    auto a = pl::augment_tool_result(input, false);
    EXPECT_NE(a.preview.find("This is line four"), std::string::npos)
        << "preview: " << a.preview;
    EXPECT_EQ(a.preview.find("Line five"), std::string::npos)
        << "preview should only include first non-empty line";
}

TEST(MessagePipeline, ToolAugment_PreviewTruncatesTo200Codepoints) {
    std::string line = "abcdefghij";   // 10 chars
    std::string big;
    for (int i = 0; i < 30; ++i) big += line;  // 300 chars, one line
    auto a = pl::augment_tool_result(big, false);
    // kMaxCompactPreviewChars = 200 → preview capped at that plus ellipsis
    EXPECT_LE(a.preview.size(), 200u + 10u);
    EXPECT_NE(a.preview.find(cc::ui::design::figures::kEllipsis), std::string::npos)
        << "long preview should end with … ellipsis";
}

// ── Stage 6 HIDE_POLICY ────────────────────────────────────────────────────
TEST(MessagePipeline, HidePolicy_HidesCompletedThinkingWhenUnselected) {
    pl::HideContext ctx{
        .is_complete = true,
        .is_selected = false,
        .is_streaming_tail = false,
    };
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantThinking, ctx));

    ctx.is_selected = true;
    EXPECT_FALSE(pl::should_hide_row(pl::PayloadShape::kAssistantThinking, ctx));

    ctx.is_selected = false;
    ctx.is_streaming_tail = true;
    EXPECT_FALSE(pl::should_hide_row(pl::PayloadShape::kAssistantThinking, ctx));

    ctx.is_streaming_tail = false;
    ctx.is_complete = false;
    EXPECT_FALSE(pl::should_hide_row(pl::PayloadShape::kAssistantThinking, ctx));
}

TEST(MessagePipeline, HidePolicy_AlwaysHidesRedactedThinking) {
    pl::HideContext ctx{};
    // Redacted thinking is ALWAYS hidden, regardless of context.
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantRedactedThinking, ctx));
    ctx.is_selected = true;
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantRedactedThinking, ctx));
}

TEST(MessagePipeline, HidePolicy_HidesCompactedTurnsAndSilentBridgeToolUses) {
    pl::HideContext ctx{};
    // UserPrompt is never hidden by default.
    EXPECT_FALSE(pl::should_hide_row(pl::PayloadShape::kUserPrompt, ctx));

    // Turn compacted → every shape inside is hidden.
    ctx.is_compacted_turn = true;
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantText, ctx));
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantToolResult, ctx));
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kSystemText, ctx));
    ctx.is_compacted_turn = false;

    // Silent bridge tool call (no error, zero-note preview) → hidden.
    ctx.is_silent_bridge_call = true;
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantToolUse, ctx));
    EXPECT_TRUE(pl::should_hide_row(pl::PayloadShape::kAssistantToolResult, ctx));
    ctx.is_silent_bridge_call = false;
    EXPECT_FALSE(pl::should_hide_row(pl::PayloadShape::kAssistantToolUse, ctx));
}

// ── Stage 7 VISIBLE_INDEX ──────────────────────────────────────────────────
TEST(MessagePipeline, VisibleIndex_EmptyInput) {
    auto idx = pl::build_visible_index(0, [](std::size_t){ return true; });
    EXPECT_TRUE(idx.empty());
}

TEST(MessagePipeline, VisibleIndex_AllRowsVisible) {
    auto idx = pl::build_visible_index(5, [](std::size_t){ return true; });
    ASSERT_EQ(idx.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) EXPECT_EQ(idx[i], i);
}

TEST(MessagePipeline, VisibleIndex_EveryOtherRowHidden) {
    auto idx = pl::build_visible_index(6, [](std::size_t i){ return i % 2 == 0; });
    ASSERT_EQ(idx.size(), 3u);
    EXPECT_EQ(idx[0], 0u);
    EXPECT_EQ(idx[1], 2u);
    EXPECT_EQ(idx[2], 4u);
}

TEST(MessagePipeline, VisibleIndex_FirstRowsHiddenPreservesOrder) {
    // First 10 hidden, then 10 visible, then 5 hidden.
    auto idx = pl::build_visible_index(25, [](std::size_t i){
        return i >= 10 && i < 20;
    });
    ASSERT_EQ(idx.size(), 10u);
    for (std::size_t i = 0; i < 10; ++i) EXPECT_EQ(idx[i], 10u + i);
}

TEST(MessagePipeline, VisibleIndex_ClampViewport) {
    // 10 visible: V[0..9]
    const auto [f1, c1] = pl::clamp_viewport(10, 0, 5);
    EXPECT_EQ(f1, 0u);
    EXPECT_EQ(c1, 5u);

    // Overflow: ask for 5 starting at 8 → [8,9] only (count=2)
    const auto [f2, c2] = pl::clamp_viewport(10, 8, 5);
    EXPECT_EQ(f2, 8u);
    EXPECT_EQ(c2, 2u);

    // Empty visible list → no rows.
    const auto [f3, c3] = pl::clamp_viewport(0, 0, 10);
    EXPECT_EQ(c3, 0u);

    // Zero viewport count → nothing rendered.
    const auto [f4, c4] = pl::clamp_viewport(100, 50, 0);
    EXPECT_EQ(c4, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// P0-2 collapseBackgroundBashNotifications tests
// TS REF: src/utils/collapseBackgroundBashNotifications.ts
// ═══════════════════════════════════════════════════════════════════════════

namespace cbb = cc::ui::messages::collapse;

namespace {
/// Build a user Message carrying a task-notification with the given status
/// and summary, matching the CPP wire format (underscored tags).
inline cc::core::Message make_notification(std::string_view status,
                                           std::string_view summary) {
    cc::core::UserMessage m{};
    std::string text = "<task_notification><status>";
    text += status;
    text += "</status><summary>";
    text += summary;
    text += "</summary></task_notification>";
    m.content.push_back(cc::core::TextBlock{std::move(text)});
    return m;
}

/// A completed background-bash notification (collapsible).
inline cc::core::Message make_completed_bash(std::string_view name = "\"foo\"") {
    return make_notification("completed",
                             std::string("Background command ") + std::string(name) + " completed");
}

/// Plain user text (never collapses).
inline cc::core::Message make_plain_user(std::string text) {
    cc::core::UserMessage m{};
    m.content.push_back(cc::core::TextBlock{std::move(text)});
    return m;
}

/// Read the first text block of a message (test helper).
inline std::string first_text(const cc::core::Message& msg) {
    const auto* u = std::get_if<cc::core::UserMessage>(&msg);
    if (!u || u->content.empty()) return {};
    const auto* t = std::get_if<cc::core::TextBlock>(&u->content.front());
    return t ? t->text : std::string{};
}
}  // namespace

TEST(CollapseBackgroundBash, SingleCompletionLeftUnchanged) {
    std::vector<cc::core::Message> in;
    in.push_back(make_completed_bash());
    auto out = cbb::collapse_background_bash_notifications(in, /*fullscreen=*/true, /*verbose=*/false);
    ASSERT_EQ(out.size(), 1u);
    // Not synthesized — original text preserved.
    EXPECT_NE(first_text(out[0]).find("Background command"), std::string::npos);
    EXPECT_EQ(first_text(out[0]).find("background commands completed"), std::string::npos);
}

TEST(CollapseBackgroundBash, MultipleConsecutiveCollapseIntoSynthetic) {
    std::vector<cc::core::Message> in;
    in.push_back(make_completed_bash("\"a\""));
    in.push_back(make_completed_bash("\"b\""));
    in.push_back(make_completed_bash("\"c\""));
    auto out = cbb::collapse_background_bash_notifications(in, true, false);
    ASSERT_EQ(out.size(), 1u);
    // TS: `<summary>3 background commands completed</summary>`
    EXPECT_NE(first_text(out[0]).find("3 background commands completed"), std::string::npos);
    EXPECT_NE(first_text(out[0]).find("<status>completed</status>"), std::string::npos);
}

TEST(CollapseBackgroundBash, FailedAndKilledStayVisible) {
    std::vector<cc::core::Message> in;
    in.push_back(make_notification("failed",  "Background command \"x\" failed with exit code 1"));
    in.push_back(make_notification("killed",  "Background command \"y\" was stopped"));
    auto out = cbb::collapse_background_bash_notifications(in, true, false);
    // Neither is a completed-bash, so both pass through untouched.
    EXPECT_EQ(out.size(), 2u);
}

TEST(CollapseBackgroundBash, NonBashSummaryNotCollapsed) {
    // Same 'completed' status but a summary that does NOT start with the
    // BACKGROUND_BASH_SUMMARY_PREFIX (e.g. an agent/workflow notification).
    std::vector<cc::core::Message> in;
    in.push_back(make_notification("completed", "Agent \"planner\" finished"));
    in.push_back(make_notification("completed", "Agent \"builder\" finished"));
    auto out = cbb::collapse_background_bash_notifications(in, true, false);
    EXPECT_EQ(out.size(), 2u);  // untouched
}

TEST(CollapseBackgroundBash, InterleavedRunsPreserveOrderAndCollapseOnlyRuns) {
    std::vector<cc::core::Message> in;
    in.push_back(make_plain_user("hello"));
    in.push_back(make_completed_bash("\"a\""));   // run of 2 → collapses
    in.push_back(make_completed_bash("\"b\""));
    in.push_back(make_plain_user("world"));
    in.push_back(make_completed_bash("\"c\""));   // run of 1 → stays
    auto out = cbb::collapse_background_bash_notifications(in, true, false);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(first_text(out[0]), "hello");
    EXPECT_NE(first_text(out[1]).find("2 background commands completed"), std::string::npos);
    EXPECT_EQ(first_text(out[2]), "world");
    EXPECT_NE(first_text(out[3]).find("Background command"), std::string::npos);  // single, unchanged
}

TEST(CollapseBackgroundBash, VerbosePassThrough) {
    std::vector<cc::core::Message> in;
    in.push_back(make_completed_bash("\"a\""));
    in.push_back(make_completed_bash("\"b\""));
    // TS: `if (verbose) return messages;`
    auto out = cbb::collapse_background_bash_notifications(in, /*fullscreen=*/true, /*verbose=*/true);
    EXPECT_EQ(out.size(), 2u);
}

TEST(CollapseBackgroundBash, NonFullscreenPassThrough) {
    std::vector<cc::core::Message> in;
    in.push_back(make_completed_bash("\"a\""));
    in.push_back(make_completed_bash("\"b\""));
    // TS: `if (!isFullscreenEnvEnabled()) return messages;`
    auto out = cbb::collapse_background_bash_notifications(in, /*fullscreen=*/false, /*verbose=*/false);
    EXPECT_EQ(out.size(), 2u);
}

// Integration: the collapse pass must be WIRED into the live AppAdapter
// message-projection path (TS Messages.tsx:520), not just unit-tested in
// isolation.  Append 3 consecutive completed-background-bash notifications to
// the engine conversation, run SyncState, and verify the transcript shows a
// single collapsed row instead of 3.

// P0-3 VirtualMessageList helpers
namespace vl = cc::ui::messages::virtual_list;
using ::cc::ui::messages::VisibleRow;

namespace {
/// Build N virtual rows of cycling heights [1,3,7,11] (same as internal
/// test helpers).  Pattern ensures we mix tiny + tall rows throughout.
inline std::vector<VisibleRow> make_vl_rows(size_t n) {
    std::vector<VisibleRow> rows(n);
    int pat[4] = {1, 3, 7, 11};
    for (size_t i = 0; i < n; ++i) {
        rows[i].row_id = i + 1;
        rows[i].estimated_height_lines = pat[i % 4];
        rows[i].backend_index = i;
        rows[i].type_hint = 0;
    }
    return rows;
}
}  // namespace

TEST(VirtualList, GeometryPrefixSumIsExact) {
    // N=5 rows  h=[1,3,7,11,1]  psum=[0,1,4,11,22,23]
    auto rows = make_vl_rows(5);
    auto jh = vl::build_geometry(std::span{rows});
    ASSERT_EQ(jh.size(), 5u);
    EXPECT_EQ(jh.total(), 23);
    EXPECT_EQ(jh.top_of(0), 0);
    EXPECT_EQ(jh.top_of(1), 1);
    EXPECT_EQ(jh.top_of(2), 4);
    EXPECT_EQ(jh.top_of(3), 11);
    EXPECT_EQ(jh.top_of(4), 22);
    EXPECT_EQ(jh.top_of(5), 23);  // one past last
    EXPECT_EQ(jh.height_of(2), 7);
    EXPECT_EQ(jh.height_of(3), 11);
}

TEST(VirtualList, FindRowBinarySearch) {
    auto rows = make_vl_rows(1000);
    auto jh = vl::build_geometry(std::span{rows});
    // Exact psum boundary → row whose top is at the target.
    EXPECT_EQ(jh.find_row_at_visual_line(0), 0u);
    // 1st row is h=1, so line 1 starts row 1
    EXPECT_EQ(jh.find_row_at_visual_line(1), 1u);
    // mid of row 2 (top=4, height=7): lines 4..10
    EXPECT_EQ(jh.find_row_at_visual_line(4), 2u);
    EXPECT_EQ(jh.find_row_at_visual_line(5), 2u);
    EXPECT_EQ(jh.find_row_at_visual_line(10), 2u);
    // line 11 → row 3
    EXPECT_EQ(jh.find_row_at_visual_line(11), 3u);
    // OOB clamp
    EXPECT_EQ(jh.find_row_at_visual_line(-999), 0u);
    EXPECT_EQ(jh.find_row_at_visual_line(999999), rows.size() - 1);
}

TEST(VirtualList, VisibleSliceCoversViewportPlusOverscan) {
    // 1000 rows of cycling heights → total ≈ 5500 lines
    auto rows = make_vl_rows(1000);
    auto jh = vl::build_geometry(std::span{rows});
    // slice start=0, viewport=40 → must cover lines [0, 40+kOverscan)
    auto [start, cnt] = vl::build_visible_slice(jh, 0, 40);
    EXPECT_EQ(start, 0u);
    const int end_line = jh.find_visual_top_for_row(start + cnt);
    const int need = 40 + vl::kOverscanRows;
    EXPECT_GE(end_line, need)
        << "slice must cover viewport + overscan vertically";
}

TEST(VirtualList, VisibleSliceMiddleJump) {
    auto rows = make_vl_rows(1000);
    auto jh = vl::build_geometry(std::span{rows});
    // Jump to the row whose visual top is nearest line 2500.
    size_t target_row = jh.find_row_at_visual_line(2500);
    ASSERT_GT(target_row, 200u);   // sanity: not in the first 200
    int scroll_top = jh.find_visual_top_for_row(target_row);
    auto [start, cnt] = vl::build_visible_slice(jh, scroll_top, 40);
    EXPECT_LE(start, target_row);
    EXPECT_GT(start + cnt, target_row);
    // start should be within overscan of target_row (overscan rows
    // *average_height* ≈ overscan * 5.5 lines ≈ 110 lines).
    EXPECT_GE(start + 10, target_row)  // generous bound
        << "slice should start WITHIN overscan ABOVE the target row";
}

TEST(VirtualList, LargeN100K_NoOverflow) {
    // 100'000 rows: build_geometry must not grow memory beyond ~800 KB,
    // find_row_at_visual_line stays O(log N), slice finds the right region.
    auto rows = make_vl_rows(100'000);
    auto jh = vl::build_geometry(std::span{rows});
    EXPECT_EQ(jh.size(), 100'000u);
    // Avg row height ≈ (1+3+7+11)/4 = 5.5 → total ≈ 550'000 lines
    EXPECT_GT(jh.total(), 500'000);
    // Sample 3 locations
    for (int line : {12345, 271828, 540000}) {
        size_t idx = jh.find_row_at_visual_line(line);
        ASSERT_LT(idx, rows.size());
        EXPECT_LE(jh.top_of(idx), line);
        if (idx + 1 < jh.size())
            EXPECT_GT(jh.top_of(idx + 1), line);
    }
    // slice near tail
    auto [s, c] = vl::build_visible_slice(jh, jh.total() - 100, 40);
    EXPECT_GT(s + c, 99'500u) << "tail slice must include the last rows";
    EXPECT_GT(c, 0u);
}

TEST(VirtualList, ClampScrollEmptyList) {
    vl::VirtualListState s;
    s.viewport_rows = 40;
    s.rows.clear();
    s.jh = vl::build_geometry(std::span{s.rows});
    vl::clamp_scroll(s);
    EXPECT_EQ(s.scroll_top, 0);
}

TEST(VirtualList, StickyBottomOnSetRowsGrowth) {
    vl::VirtualListHandle h;
    auto state = std::make_shared<vl::VirtualListState>();
    h.state = state.get();
    state->viewport_rows = 10;
    state->auto_mode = vl::AutoScrollMode::Sticky;
    // Initial list of 5 rows (heights [1,3,7,11,1]) → total = 23.
    auto rows = make_vl_rows(5);
    h.SetRows(std::vector<VisibleRow>(rows.begin(), rows.end()));
    // After initial SetRows with sticky semantics enabled:
    //   max = total - vp = 23 - 10 = 13.  Start at max means the viewport
    //   covers lines [13, 23) — pinned to the tail.
    EXPECT_EQ(state->scroll_top, 13);
    EXPECT_TRUE(state->sticky_bottom);

    // Simulate the user manually scrolling away (not sticky anymore).
    state->scroll_top = 0;
    state->sticky_bottom = false;
    vl::update_sticky_after_scroll(*state, 13);

    // Append rows 5..14 (10 more rows).  Because scroll is not sticky,
    // the new-message counter MUST increment.
    auto more = make_vl_rows(15);
    h.SetRows(std::vector<VisibleRow>(more.begin(), more.end()));
    EXPECT_GT(state->new_message_count, 0)
        << "scrolled-up + appended rows = new_message_count must grow";
    EXPECT_FALSE(state->sticky_bottom);
    EXPECT_EQ(state->scroll_top, 0) << "non-sticky list must NOT re-pin";
}

TEST(VirtualList, BuildRowGeometrySliceMatchesPsum) {
    auto rows = make_vl_rows(100);
    auto jh = vl::build_geometry(std::span{rows});
    // Request a slice at rows [37, 42).
    auto gs = vl::build_row_geometry_slice(std::span{rows}, jh, 37, 5);
    ASSERT_EQ(gs.size(), 5u);
    for (size_t k = 0; k < gs.size(); ++k) {
        size_t idx = 37 + k;
        EXPECT_EQ(gs[k].row_idx, idx);
        EXPECT_EQ(gs[k].top_line, jh.find_visual_top_for_row(idx));
        EXPECT_EQ(gs[k].height_lines,
                  std::max(1, (int)rows[idx].estimated_height_lines));
        // `height_measured` was never set → `cached == false`.
        EXPECT_FALSE(gs[k].cached);
    }
}

TEST(VirtualList, HandleJumpToRowCentersHeadroom) {
    vl::VirtualListHandle h;
    auto state = std::make_shared<vl::VirtualListState>();
    h.state = state.get();
    state->viewport_rows = 40;
    state->auto_mode = vl::AutoScrollMode::Disabled;
    auto rows = make_vl_rows(500);
    h.SetRows(std::vector<VisibleRow>(rows.begin(), rows.end()));

    // JumpToRow(100, headroom=3) → scroll_top set to top(100) - 3, clamped ≥ 0.
    h.JumpToRow(100, 3);
    int expected = std::max(0, state->jh.find_visual_top_for_row(100) - 3);
    EXPECT_EQ(state->scroll_top, expected);
    // JumpToRow(0) when headroom would go negative → clamp to 0.
    h.JumpToRow(0, 999);
    EXPECT_EQ(state->scroll_top, 0);
    // JumpToRow past the end → clamped to last row.
    h.JumpToRow(99999, 0);
    ASSERT_FALSE(state->rows.empty());
    size_t last = state->rows.size() - 1;
    int last_top = state->jh.find_visual_top_for_row(last);
    EXPECT_LE(state->scroll_top, last_top);
}

TEST(VirtualList, RenderListEmptyDoesNotCrash) {
    vl::VirtualListState s;
    s.viewport_rows = 40;
    s.rows.clear();
    s.jh = vl::build_geometry(std::span{s.rows});
    // render_list_as_elements on empty → returns 1-line filler, no segfault.
    ftxui::Element el = vl::render_list_as_elements(s);
    ftxui::Screen screen(80, 5);
    ftxui::Render(screen, el);
    // Screen must render successfully.  Empty output is acceptable as long
    // as the call didn't abort (ASAN would catch OOB).
    EXPECT_TRUE(true);
    (void)screen;
}

TEST(VirtualList, RenderListMediumProducesValidVBox) {
    // 200 rows, viewport 40 = definitely windowed (OVERSCAN in both dirs).
    vl::VirtualListState s;
    s.options.ascii_gutter = true;
    s.auto_mode = vl::AutoScrollMode::Disabled;
    s.viewport_rows = 40;
    s.options.viewport_rows = 40;
    auto rows = make_vl_rows(200);
    s.rows.assign(rows.begin(), rows.end());
    s.jh = vl::build_geometry(std::span{s.rows});
    int n_rendered = 0;
    s.callbacks.render_row = [&](size_t i, const VisibleRow&) -> ftxui::Element {
        ++n_rendered;
        return ftxui::text("row " + std::to_string(i))
             | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                           std::max(1, rows[i].estimated_height_lines));
    };
    // Scroll to mid-list.
    s.scroll_top = s.jh.total() / 2;
    ftxui::Element el = vl::render_list_as_elements(s);
    // Rendering is required to force FTXUI layout.
    ftxui::Screen screen(120, 40);
    ftxui::Render(screen, el);
    // Critical invariant: # of rendered rows MUST be strictly less than
    // rows.size() (the whole point of virtual windowing).  A conservative
    // upper bound is viewport_rows + 2·kOverscanRows + 4 (for pills).
    EXPECT_LT(n_rendered, (int)rows.size())
        << "virtual window must not render all rows";
    const int expected_max = 40 + 2 * (int)vl::kOverscanRows + 4;
    EXPECT_LE(n_rendered, expected_max)
        << "rendered count cap: viewport + 2·overscan + pills";
    (void)screen;
}

TEST(VirtualList, ScrollZeroViewportNeverUnderflows) {
    vl::VirtualListState s;
    s.viewport_rows = 0;   // pathological: caller supplies invalid size
    auto rows = make_vl_rows(10);
    s.rows.assign(rows.begin(), rows.end());
    s.jh = vl::build_geometry(std::span{s.rows});
    s.scroll_top = 9999;
    vl::clamp_scroll(s);
    // viewport_rows==0 ⇒ max = total - 0 = total.  Scroll is clamped there.
    EXPECT_EQ(s.scroll_top, s.jh.total());
    // slice with vp=0 → should not overflow / not produce a negative count.
    auto [start, cnt] = vl::build_visible_slice(s.jh, 0, 0);
    EXPECT_LE((int)start, (int)s.rows.size());
    // render must not crash / abort — pathological viewport size returns a
    // syntactically-valid (possibly zero-height) Element.
    ftxui::Element el = vl::render_list_as_elements(s);
    ftxui::Screen screen(80, 3);
    ftxui::Render(screen, el);
    EXPECT_TRUE(true);
    (void)cnt;
    (void)screen;
}

// ── P0-4 LogoV2 + WelcomeV2 + 10-deep notice stack ──────────────────────────

namespace unseen_divider_test {

using namespace cc::ui::messages_list;
using cc::ui::messages::MessageShape;
using cc::ui::messages::UserTextMessageData;
using cc::ui::messages::AssistantTextMessageData;

/// Helper: build a minimal MessagesListInput with N alternating rows, each
/// with a 24-char uuid of the form `<prefix>_<i>{pad}`.  Row i is user if
/// (i%2==0) else assistant.  The caller can then set input.unseen_divider.
auto make_synthetic_input(std::size_t num_rows,
                          std::string_view uuid_prefix20 = "ABCDEF0123456789abcd") {
    MessagesListInput in;
    in.rows.reserve(num_rows);
    in.shapes.reserve(num_rows);
    in.uuids.reserve(num_rows);
    for (std::size_t i = 0; i < num_rows; ++i) {
        char uuid_buf[25];
        // uuid_prefix20 is 20 chars, so with 4-digit suffix = 24 chars exactly.
        std::snprintf(uuid_buf, sizeof(uuid_buf), "%.*s%04zu",
                      20, uuid_prefix20.data(), i);
        in.uuids.emplace_back(uuid_buf, 24);
        if ((i & 1u) == 0u) {
            in.shapes.push_back(MessageShape::UserText);
            in.rows.push_back(UserTextMessageData{
                .content = std::string("User prompt #") + std::to_string(i),
                .quoted_reply = std::nullopt,
                .command_name = std::nullopt});
        } else {
            in.shapes.push_back(MessageShape::AssistantText);
            in.rows.push_back(AssistantTextMessageData{
                .content = std::string("Assistant reply #") + std::to_string(i),
                .model_name = std::nullopt,
                .is_streaming = false});
        }
    }
    in.viewport_rows = 40;
    in.pin_to_bottom = true;
    return in;
}

} // namespace unseen_divider_test

// T1: Condensed mode (default is_condensed_mode = true) renders the 3-row
//     CondensedLogo strip (Claude Code + version · model·billing · cwd), the
//     Opus1M notice, and the VoiceMode notice.  The brand chip and FeedColumn
//     / rounded border MUST NOT appear.
TEST(MessagesList, UnseenDivider_PrefixMatchFindsTargetRow) {
    using namespace unseen_divider_test;
    auto in = make_synthetic_input(6, "old0000000000000000000");
    // Row 0 (user) → uuid = "old0000000000000000000000" (indices 0-23)
    // Row 1 (asst) → "old0000000000000000000001"
    // Row 2 (user) → "old0000000000000000000002"
    // Row 3 (asst) → "old0000000000000000000003"
    // Row 4 (user) → "old0000000000000000000004"
    // Row 5 (asst) → "old0000000000000000000005"

    auto visible = build_visible_rows(in);
    ASSERT_EQ(visible.size(), 6u);

    // Case A: point at row 3 → divider_before = 3
    in.unseen_divider = UnseenDivider{
        .first_unseen_uuid_prefix = "old0000000000000000000003",
        .count = 1,
    };
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible), 3u);

    // Case B: point at row 0 (first message) → divider_before = 0
    in.unseen_divider->first_unseen_uuid_prefix = "old0000000000000000000000";
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible), 0u);

    // Case C: prefix match — TS's deriveUUID preserves 24-char prefix across
    // derived sub-blocks.  We match on prefix even if the divider's stored
    // value is a longer full uuid (36 chars) — only first 24 count.
    in.unseen_divider->first_unseen_uuid_prefix =
        std::string("old0000000000000000000002") + "-EXTRA-SUFFIX-IGNORED";
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible), 2u);

    // Case D: no match → return visible.size() (sentinel)
    in.unseen_divider->first_unseen_uuid_prefix = "ZZZZZZZZZZZZZZZZZZZZZZZZ00";
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible),
              visible.size());

    // Case E: unseen_divider = nullopt → sentinel (no divider)
    in.unseen_divider.reset();
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible),
              visible.size());
}

/// Unit test: count pluralisation in divider title.  TS: count === 1 → "message",
/// else → "messages".
TEST(MessagesList, UnseenDivider_TitlePluralisation) {
    using namespace cc::ui::messages_list;
    using namespace sticky_prompt_test;

    // count=1 → title reads "1 new message"
    auto div1 = cc::ui::messages_list::detail::render_unseen_divider(1);
    auto snap1 = strip_ansi(render_ansi(std::move(div1), 80, 4));
    EXPECT_NE(snap1.find("1 new message"), std::string::npos);
    // Singular must NOT contain "1 new messages" (note trailing 's')
    EXPECT_EQ(snap1.find("1 new messages"), std::string::npos);

    // count=3 → "3 new messages"
    auto divN = cc::ui::messages_list::detail::render_unseen_divider(3);
    auto snapN = strip_ansi(render_ansi(std::move(divN), 80, 4));
    EXPECT_NE(snapN.find("3 new messages"), std::string::npos);
}

/// Golden snapshot test: render a 4-row transcript WITH an unseen divider
/// inserted before row 2 (first of the "new" assistant turns) with count=2.
/// This test catches any drift in the divider layout (marginTop=1 blank line,
/// separator dashes, bold/muted title, trailing dashes stretched to width).
TEST(MessagesList, UnseenDivider_RendersDividerInTranscript_Golden) {
    using namespace unseen_divider_test;
    using namespace sticky_prompt_test;

    auto in = make_synthetic_input(4, "snap00000000000000000000");
    // Rows 0=user#0, 1=asst#1, 2=user#2, 3=asst#3
    // Unseen divider: 2 new assistant turns starting BEFORE row#2.
    in.unseen_divider = UnseenDivider{
        .first_unseen_uuid_prefix = "snap0000000000000000000002",
        .count = 2,
    };
    in.pin_to_bottom = false;
    in.viewport_rows = 30;

    auto el = render_messages_list_view(std::move(in), /*frame=*/0,
                                        /*render_last_n=*/80);
    std::string snap = strip_ansi(render_ansi(std::move(el), 100, 30));

    // Assert title exists in the transcript (the golden catches exact layout).
    EXPECT_NE(snap.find("2 new messages"), std::string::npos)
        << "divider title must appear in the rendered transcript";
    // Assert the user #2 row appears AFTER the divider (content ordering check).
    // If the divider were placed AFTER its target row, or not at all, this
    // ordering invariant would break.
    auto pos_divider = snap.find("2 new messages");
    auto pos_row2 = snap.find("User prompt #2");
    ASSERT_NE(pos_divider, std::string::npos);
    ASSERT_NE(pos_row2, std::string::npos);
    EXPECT_LT(pos_divider, pos_row2)
        << "divider must render BEFORE its target payload row";

    // ── Golden snapshot (TS REF: Messages.tsx L631-635 Divider element).
    //    UPDATE_GOLDENS=1 ./cc_test --gtest_filter='*Golden*' to refresh.
    // Re-render to a fresh snapshot (std::move consumed el above).
    auto in2 = make_synthetic_input(4, "snap00000000000000000000");
    in2.unseen_divider = UnseenDivider{
        .first_unseen_uuid_prefix = "snap0000000000000000000002",
        .count = 2,
    };
    in2.pin_to_bottom = false;
    in2.viewport_rows = 30;
    auto el2 = render_messages_list_view(std::move(in2), 0, 80);
    check_golden("unseen_divider_in_transcript_missing",
                 render_ansi(std::move(el2), 100, 30));
}

/// Baseline golden: same 4-row transcript BUT no unseen_divider set.  Serves
/// as the "control" to confirm the divider-only delta in the test above.
TEST(MessagesList, UnseenDivider_NoDividerBaseline_Golden) {
    using namespace unseen_divider_test;
    using namespace sticky_prompt_test;

    auto in = make_synthetic_input(4, "snap00000000000000000000");
    in.unseen_divider.reset();   // explicit nullopt: no divider
    in.pin_to_bottom = false;
    in.viewport_rows = 30;

    auto in_assert = make_synthetic_input(4, "snap00000000000000000000");
    in_assert.unseen_divider.reset();
    in_assert.pin_to_bottom = false;
    in_assert.viewport_rows = 30;
    std::string snap = strip_ansi(render_ansi(
        render_messages_list_view(std::move(in_assert), 0, 80), 100, 30));
    // Baseline must NOT contain the divider title.
    EXPECT_EQ(snap.find("new message"), std::string::npos)
        << "baseline (unseen_divider=nullopt) must NOT render a divider";

    // Golden snapshot for the no-divider case (control file).  Use a
    // separately-constructed input so the assertion snapshot and the golden
    // snapshot are independent (in_assert was moved-from above).
    auto in_ctrl = make_synthetic_input(4, "snap00000000000000000000");
    in_ctrl.unseen_divider.reset();
    in_ctrl.pin_to_bottom = false;
    in_ctrl.viewport_rows = 30;
    check_golden("unseen_divider_baseline_no_divider",
                 render_ansi(render_messages_list_view(std::move(in_ctrl), 0, 80),
                             100, 30));
}

/// Edge-case test: divider anchor lands on a ThinkingBlock (skipped in TS's
/// computeUnseenDivider per CC-724).  The CPP equivalent's guard (progress +
/// null-rendering attachments skipped) isn't in this repo yet, but the
/// prefix-match must still pick the first PAYLOAD row it encounters (not
/// crash on empty uuids).
TEST(MessagesList, UnseenDivider_AnchorWithEmptyUuidEntries_NoCrash) {
    using namespace unseen_divider_test;
    using namespace sticky_prompt_test;  // strip_ansi, render_ansi
    auto in = make_synthetic_input(5, "edge00000000000000000000");
    // Clear uuid on row 2 to simulate a filtered-out row.
    in.uuids[2].clear();
    in.unseen_divider = UnseenDivider{
        .first_unseen_uuid_prefix = "edge0000000000000000000003",
        .count = 1,
    };
    auto visible = build_visible_rows(in);
    // find_divider_before_visible_index must not UB; must return 3.
    EXPECT_EQ(cc::ui::messages_list::detail::find_divider_before_visible_index(in, visible), 3u);

    // Rendering path must not crash (empty uuid on row 2 is legal input).
    auto in_render = make_synthetic_input(5, "edge00000000000000000000");
    in_render.uuids[2].clear();
    in_render.unseen_divider = UnseenDivider{
        .first_unseen_uuid_prefix = "edge0000000000000000000003",
        .count = 1,
    };
    auto el = render_messages_list_view(std::move(in_render), 0, 80);
    // Smoke-render the transcript to catch any UB/crash in the divider
    // insertion branch when one row's uuid is empty.  We route through
    // strip_ansi so that Screen::ToString() fully traverses the Element
    // tree and exercises every node.
    std::string snap =
        strip_ansi(render_ansi(std::move(el), /*term_w=*/80, /*term_h=*/20));
    // Divider title for count=1 MUST appear.
    EXPECT_NE(snap.find("1 new message"), std::string::npos);
    (void)snap;
}

// =============================================================================
// GAP: image-paste-display-broken (P0, user-reported 2026-06-30)
// BUG: Clipboard Ctrl+V capture + API transmission paths (EXIST per commit
// f85a5b8), but the display pipeline was broken at TWO links:
//   (M3) project_message() silently dropped ImageBlocks when iterating user
//        message content — only TextBlock* was std::get_if'd.
//   (M4) repl_screen's row builder mapped ALL role=="user" entries to a single
//        MessageShape::UserText row — MessageShape::UserImage + message_image
//        module were dead code.
// FIX (commits above):
//   1. ImageBlock gains {width,height,size_bytes,file_name,source_path,source}
//   2. project_messages() SPLITS a UserMessage with mixed TextBlock+ImageBlock
//      content into MULTIPLE display rows (TS parity: each UserImageMessage is
//      its own transcript row).
//   3. BuildMessages() in repl_screen dispatches is_image entries to
//      MessageShape::UserImage and populates ImageMessageData from the block.
// TS REF: src/components/UserImageMessage.tsx (renderer)
//         src/utils/processUserInput/processUserInput.ts L351-395 (content blocks)
