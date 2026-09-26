/// @file test_ui_runtime.cpp
/// @brief Split from test_ui.cpp - AppRuntime, E2E_Gate, FullscreenLayout, LogoV2, PromptInput, ReplScreen (SLOC budget fix)

#include <cstdlib>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <gtest/gtest.h>
#include <httplib.h>

#include "test_ui_helpers.h"

import std;
import cc.ui.app.app;
import cc.ui.screens.repl_screen;
import cc.commands.registry;
import cc.query.query_engine;
import cc.tools.tool;
import cc.utils.session_storage;
import cc.ui.foundation.design_figures;
import cc.ui.foundation.theme_provider;
import cc.ui.widgets.all_components;
import cc.ui.messages.user_text_message;
import cc.ui.foundation.declared_cursor;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.chrome.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════





TEST(ReplScreen, WelcomeHeaderUsesHomeCard) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/tmp/cpp_migration";

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/0, /*term_cols=*/120),
        120,
        16));

    // Phase 2 Faithful: CondensedLogo 3-line strip + Opus1M notice banner
    // (replaces the old ASCII-card + Recent activity / What's new feed).
    EXPECT_NE(rendered.find("Loom"), std::string::npos);
    EXPECT_NE(rendered.find("v9.9.9-test"), std::string::npos);
    EXPECT_NE(rendered.find("GLM-5.2"), std::string::npos);
    EXPECT_NE(rendered.find("/tmp/cpp_migration"), std::string::npos);
    EXPECT_NE(rendered.find("Opus now defaults to 1M context"),
              std::string::npos);
    EXPECT_NE(rendered.find("5x more room, same pricing"),
              std::string::npos);

    // Old feed-card fields that no longer appear in the faithful layout.
    EXPECT_EQ(rendered.find("Welcome back!"), std::string::npos);
    EXPECT_EQ(rendered.find("Recent activity"), std::string::npos);
    EXPECT_EQ(rendered.find("What's new"), std::string::npos);
    EXPECT_EQ(rendered.find("Welcome to Loom"), std::string::npos);
    EXPECT_EQ(rendered.find("Use /model to switch between models"),
              std::string::npos);
    // Faithful Loom mascot is a 9×3 block-art composed of unicode BOX DRAWING /
    // QUADRANT chars (▛ ▜ ▝ ▘ etc.) — there must be NO 🐱 U+1F431 emoji
    // anywhere (the UTF-8 encoding of U+1F431 is the 4-byte sequence below).
    EXPECT_EQ(rendered.find("\xF0\x9F\x90\xB1"), std::string::npos);
}



TEST(ReplScreen, WelcomeHeaderShowsConfiguredAgentName) {
    namespace repl = cc::ui::repl_screen;

    // TS REF: logoV2Utils.ts:259 — settings.agent renders as "@<agent> · <cwd>"
    // on the welcome header's cwd line.
    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/tmp/cpp_migration";
    state.settings_agent_name = "custom-agent";

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/0, /*term_cols=*/120),
        120, 16));
    EXPECT_NE(rendered.find("@custom-agent \xC2\xB7 /tmp/cpp_migration"),
              std::string::npos)
        << "cwd line should carry the @<agent> prefix when settings.agent is set";

    // With no configured agent the prefix must disappear (plain cwd only).
    repl::ReplScreenState plain;
    plain.app_version = "9.9.9-test";
    plain.model_display_name = "GLM-5.2";
    plain.cwd = "/tmp/cpp_migration";
    auto rendered_plain = strip_ansi(render_to_plain_text(
        repl::RenderWelcomeHeader(plain, /*spinner_frame=*/0, /*term_cols=*/120),
        120, 16));
    EXPECT_EQ(rendered_plain.find("@custom-agent"), std::string::npos);
}



TEST(ReplScreen, ShiftReturnInsertsNewlineForBothTerminalEncodings) {
    // Shift+Enter must insert a newline (not submit). Terminals emit either
    // CSI-u (ESC [ 13 ; 2 u) or kitty protocol (ESC [ 27 ; 2 ; 13 ~).
    for (const auto* seq : {"\x1b[13;2u", "\x1b[27;2;13~"}) {
        cc::core::ToolRegistry tools;
        cc::core::QueryEngineConfig config;
        config.context_window.auto_compact = false;
        config.cwd = fs::temp_directory_path().string();
        cc::core::QueryEngine engine(std::move(config), tools);
        cc::commands::AppCommandRegistry commands;
        const auto storage_root = fs::temp_directory_path() /
            ("loom_shift_ret_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()) +
             (seq[3] == '1' ? "_csiu" : "_kitty"));
        cc::utils::SessionStorage storage(storage_root);
        auto app = ftxui::Make<cc::ui::AppAdapter>(
            &engine, nullptr, &commands, &storage, [] {});

        app->OnEvent(ftxui::Event::Character('a'));
        app->OnEvent(ftxui::Event::Special(seq));
        app->OnEvent(ftxui::Event::Character('b'));

        const auto text = app->input_text_for_testing();
        EXPECT_EQ(text, "a\nb")
            << "shift-return sequence should insert a newline";
        EXPECT_FALSE(app->is_query_running_for_testing())
            << "shift-return must not submit the prompt";

        app.reset();
        std::error_code ec;
        fs::remove_all(storage_root, ec);
    }
}



TEST(ReplScreen, WelcomeHeaderWidthAndClaudeColorTrackTerminal) {
    namespace repl = cc::ui::repl_screen;
    namespace thm = cc::ui::design::theme;

    const auto previous_theme = thm::current_theme();
    thm::set_theme(thm::ThemeVariant::Dark);
    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/Users/example/Develop/Project";

    auto wide_element =
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/0, /*term_cols=*/200);
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(200),
        ftxui::Dimension::Fixed(16));
    ftxui::Render(screen, wide_element);
    auto wide = strip_ansi(screen.ToString());
    // CondensedLogo should be able to consume almost the full terminal width
    // when cwd is long enough to need it.
    EXPECT_GE(max_line_width_bytes(wide), 40u);
    EXPECT_NE(wide.find("Opus now defaults to 1M context"),
              std::string::npos);

    // Faithful condensed logo uses the brand accent (same as TS
    // LogoV2's Loom mascot accent RGB(215,119,87) = #D77757) on the first row
    // glyph, instead of the old primary-palette border decoration.  The
    // accent must appear somewhere in the rendered header.
    const ftxui::Color kBrandAccent(215, 119, 87);
    bool has_brand_accent_pixel = false;
    for (int y = 0; y < 16 && !has_brand_accent_pixel; ++y) {
        for (int x = 0; x < 200; ++x) {
            if (screen.PixelAt(x, y).foreground_color == kBrandAccent) {
                has_brand_accent_pixel = true;
                break;
            }
        }
    }
    EXPECT_TRUE(has_brand_accent_pixel);

    auto normal = strip_ansi(render_to_plain_text(
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/0, /*term_cols=*/120),
        120,
        16));
    // CWD field truncates to fit narrower terminal; wide row should still be
    // measurably more filled on the CWD row.
    EXPECT_LT(max_line_width_bytes(normal),
              max_line_width_bytes(wide) + 1);  // monotonic non-decrease
    thm::set_theme(previous_theme);
}



TEST(ReplScreen, WelcomeHeaderAnimatesAsteriskColor) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";

    auto frame0 = render_to_plain_text(
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/0, /*term_cols=*/120),
        120,
        16);
    auto frame8 = render_to_plain_text(
        repl::RenderWelcomeHeader(state, /*spinner_frame=*/8, /*term_cols=*/120),
        120,
        16);

    // Phase 2 + P0-4 Faithful CondensedLogo: the welcome strip is a
    // static 3-line logo + notice stack (Opus1m + gated rest).
    // There is NO per-frame asterisk animation — both frames must
    // therefore render byte-for-byte identical, and no "asterisk-like"
    // glyph is allowed in the output; the old rotating `✦✧✶` spinner
    // chars embedded inside the old ASCII-art card must NOT appear.
    EXPECT_EQ(frame0, frame8);
    // Sanity: condensed-logo branding + Opus1m body present.
    EXPECT_NE(strip_ansi(frame0).find("Loom"), std::string::npos);
    EXPECT_NE(strip_ansi(frame0).find("Opus now defaults to 1M context"),
              std::string::npos);
    // Old rotating-spinner glyphs (✦ U+2726, ✧ U+2727, ✶ U+2736) must be
    // absent — these were the per-frame animation characters.
    EXPECT_EQ(strip_ansi(frame0).find("\xE2\x9C\xA6"), std::string::npos);  // ✦
    EXPECT_EQ(strip_ansi(frame0).find("\xE2\x9C\xA7"), std::string::npos);  // ✧
    EXPECT_EQ(strip_ansi(frame0).find("\xE2\x9C\xB6"), std::string::npos);  // ✶
}



TEST(ReplScreen, PromptInputRendersTopAndBottomBorders) {
    // TS PromptInput.tsx:2237/2268: borderStyle="round" with borderBottom and
    // borderLeft/Right={false}.  Ink defaults borderTop to TRUE when borderStyle
    // is set (render-background.js: `borderTop !== false ? 1 : 0`), and the top
    // border carries `borderText` (mode indicator text) embedded in the line.
    //
    // CPP emulates borderText by composing a hbox: mode-label (or "❯" in Prompt
    // mode) + separator fill.  This gives visual separation without the bare
    // "白条" of a plain separator().
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "/";

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderPromptInput(state, 80),
        80,
        4));

    std::size_t border_lines = 0;
    std::size_t line_start = 0;
    while (line_start <= rendered.size()) {
        const auto line_end = rendered.find('\n', line_start);
        const auto line = rendered.substr(
            line_start,
            line_end == std::string::npos ? std::string::npos : line_end - line_start);
        if (line.find("──────────") != std::string::npos) {
            ++border_lines;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }

    EXPECT_GE(border_lines, 2u);  // top (with embedded ❯) + bottom
    EXPECT_NE(rendered.find("❯ /"), std::string::npos);
}



// Audit round7 P0 prefix-glyph-no-unified-impl: the prompt prefix — the first
// glyph the user sees on every render — must be TS-faithful.  TS
// PromptInputModeIndicator.tsx emits exactly two variants: '!' in bash mode
// and figures.pointer ('❯') otherwise.  This test locks that contract at the
// live render site AND at the shared TextInputOptions default (which used to
// carry a CPP-only "▶ " invention that contradicted its own doc comment).
TEST(ReplScreen, PromptPrefixGlyphIsTsFaithfulPointerOrBang) {
    namespace repl = cc::ui::repl_screen;
    namespace figs = cc::ui::design::figures;

    // Sanity: the shared figures constants are the single source of truth.
    EXPECT_EQ(std::string(figs::kPointer), "\xE2\x9D\xAF");  // ❯ U+276F
    EXPECT_EQ(std::string(figs::kBashGlyph), "!");
    // The '▶' (U+25B6) CPP-only glyph must NOT be the pointer.
    EXPECT_NE(std::string(figs::kPointer), "\xE2\x96\xB6");

    // Non-bash prompt renders '❯ ' and never the old '▶' glyph.
    {
        repl::ReplScreenState state;
        state.input_text = "hello";
        auto rendered = strip_ansi(render_to_plain_text(
            repl::RenderPromptInput(state, 80), 80, 4));
        EXPECT_NE(rendered.find("❯ hello"), std::string::npos);
        EXPECT_EQ(rendered.find("\xE2\x96\xB6"), std::string::npos);  // no ▶
        // The retired CPP-only badge pills must not appear as a prefix.
        EXPECT_EQ(rendered.find("NORMAL"), std::string::npos);
        EXPECT_EQ(rendered.find("PLAN"), std::string::npos);
    }

    // Bash mode (leading '!') renders the '!' prefix, not '❯'.
    {
        repl::ReplScreenState state;
        state.input_text = "!ls";
        auto rendered = strip_ansi(render_to_plain_text(
            repl::RenderPromptInput(state, 80), 80, 4));
        // The rendered line begins with the bash bang prefix.
        EXPECT_NE(rendered.find("! "), std::string::npos);
    }

    // The standalone TextInputOptions default prefix must be '❯ ' (TS
    // figures.pointer), guarding against regression to the '▶ ' invention.
    {
        cc::ui::components::TextInputOptions opts;
        EXPECT_EQ(opts.prefix, "\xE2\x9D\xAF ");  // "❯ "
    }
}



TEST(ReplScreen, TranscriptScrollOffsetMovesLongLocalCommandOutput) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.viewport_height_lines = 8;
    state.scroll_pinned_to_bottom = false;

    repl::MessageDisplayEntry message;
    message.is_local_command_output = true;
    for (int i = 0; i < 40; ++i) {
        message.content_preview += std::format("line-{:02}", i);
        if (i != 39) message.content_preview += '\n';
    }
    state.messages.push_back(std::move(message));

    auto top = strip_ansi(render_to_plain_text(
        repl::RenderMessages(state.messages,
                             state.selected_message_idx,
                             state.viewport_height_lines,
                             state.scroll_offset,
                             state.scroll_pinned_to_bottom),
        120,
        8));
    EXPECT_NE(top.find("line-00"), std::string::npos);
    EXPECT_EQ(top.find("line-24"), std::string::npos);

    state.scroll_offset = 24;
    auto scrolled = strip_ansi(render_to_plain_text(
        repl::RenderMessages(state.messages,
                             state.selected_message_idx,
                             state.viewport_height_lines,
                             state.scroll_offset,
                             state.scroll_pinned_to_bottom),
        120,
        8));
    EXPECT_EQ(scrolled.find("line-00"), std::string::npos);
    EXPECT_NE(scrolled.find("line-25"), std::string::npos);
}



TEST(ReplScreen, PromptInputParksHiddenNativeCursorAtCaret) {
    namespace repl = cc::ui::repl_screen;
    namespace dc = cc::ui::common::declared_cursor;

    repl::ReplScreenState state;
    state.input_text = "hello";

    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(80),
        ftxui::Dimension::Fixed(5));
    ftxui::Render(screen, repl::RenderPromptInput(state, 80) | dc::cursor_reset());

    const auto cursor = screen.cursor();
    EXPECT_EQ(cursor.shape, ftxui::Screen::Cursor::Shape::Hidden);
    EXPECT_GT(cursor.x, 0);
    EXPECT_GT(cursor.y, 0);
    EXPECT_LT(cursor.x, 79);
    EXPECT_LT(cursor.y, 4);
}



TEST(ReplScreen, CursorResetParksHiddenCursorAwayFromTopLeft) {
    namespace dc = cc::ui::common::declared_cursor;

    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(20),
        ftxui::Dimension::Fixed(5));
    ftxui::Render(screen, ftxui::text("idle") | dc::cursor_reset());

    const auto cursor = screen.cursor();
    EXPECT_EQ(cursor.shape, ftxui::Screen::Cursor::Shape::Hidden);
    EXPECT_EQ(cursor.x, 19);
    EXPECT_EQ(cursor.y, 4);
}



// ─── Placeholder cascade tests (TS REF: usePromptInputPlaceholder.ts) ──────

TEST(ReplScreen, PlaceholderEmptyInputShowsExampleOnFirstSubmit) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 0;
    state.prompt_suggestion_enabled = true;

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    // First example command is "fix lint errors" (index 0 of kExampleCommands).
    EXPECT_NE(placeholder->find("fix lint errors"), std::string::npos);
    EXPECT_NE(placeholder->find("Try"), std::string::npos);
}



TEST(ReplScreen, PlaceholderNonEmptyInputReturnsNullopt) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "hello";
    state.submit_count = 0;

    auto placeholder = repl::ComputePlaceholder(state);
    EXPECT_FALSE(placeholder.has_value());
}



TEST(ReplScreen, PlaceholderAfterSubmitNoExample) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 1;  // Already submitted once
    state.prompt_suggestion_enabled = true;

    auto placeholder = repl::ComputePlaceholder(state);
    // After first submit, the onboarding example is no longer shown.
    // Without other conditions matching, returns nullopt.
    EXPECT_FALSE(placeholder.has_value());
}



TEST(ReplScreen, PlaceholderViewingAgentShowsMessageHint) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;  // Past onboarding
    state.viewing_agent_name = "researcher";

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    EXPECT_NE(placeholder->find("Message @researcher…"), std::string::npos);
}



TEST(ReplScreen, PlaceholderViewingAgentLongNameTruncated) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;
    // 25 characters — exceeds the 20-char limit.
    state.viewing_agent_name = "very-long-agent-name-here";

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    // Should be truncated to "very-long-agent-n..." (17 chars + "..." = 20).
    // Should be truncated: TS uses '...' (3 dots) for name truncation + '…' (U+2026) for suffix.
    // Result = "Message @very-long-agent-n...…" (17-char name + "..." + "…")
    EXPECT_NE(placeholder->find("Message @very-long-agent-n..."), std::string::npos);
}



TEST(ReplScreen, PlaceholderQueuedCommandsHint) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;  // Past onboarding
    state.has_editable_queued_commands = true;
    state.queued_command_hint_shown_count = 0;

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    EXPECT_EQ(*placeholder, "Press up to edit queued messages");
}



TEST(ReplScreen, PlaceholderQueuedCommandsHintCappedAt3) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;
    state.has_editable_queued_commands = true;
    state.queued_command_hint_shown_count = 3;  // Already shown 3 times

    auto placeholder = repl::ComputePlaceholder(state);
    // Should NOT show the hint anymore (capped at 3).
    EXPECT_FALSE(placeholder.has_value());
}



TEST(ReplScreen, PlaceholderAiSuggestionOverridesExample) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 0;
    state.input_mode = repl::InputMode::Normal;
    state.next_action_suggestion = "explain the error above";
    state.prompt_suggestion_enabled = true;

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    // AI suggestion takes priority over the onboarding example.
    EXPECT_EQ(*placeholder, "explain the error above");
}



TEST(ReplScreen, PlaceholderAiSuggestionIgnoredInBashMode) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 0;
    state.input_mode = repl::InputMode::Bash;
    state.next_action_suggestion = "explain the error above";
    state.prompt_suggestion_enabled = true;

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    // In bash mode, AI suggestion is ignored. Falls through to example.
    EXPECT_NE(placeholder->find("Try"), std::string::npos);
    EXPECT_NE(placeholder->find("fix lint errors"), std::string::npos);
}



TEST(ReplScreen, PlaceholderAiSuggestionIgnoredWhenViewingAgent) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;
    state.input_mode = repl::InputMode::Normal;
    state.next_action_suggestion = "explain the error above";
    state.viewing_agent_name = "helper";

    auto placeholder = repl::ComputePlaceholder(state);
    ASSERT_TRUE(placeholder.has_value());
    // When viewing agent, teammate hint takes priority over AI suggestion.
    EXPECT_EQ(*placeholder, "Message @helper…");
}



TEST(ReplScreen, PlaceholderAiSuggestionIgnoredWhenSlashCommand) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 5;
    state.input_mode = repl::InputMode::Normal;
    // Slash-command suggestions start with '/' — should not be used as placeholder.
    state.next_action_suggestion = "/commit";

    auto placeholder = repl::ComputePlaceholder(state);
    // The '/' suggestion is ignored; no other conditions match.
    EXPECT_FALSE(placeholder.has_value());
}



TEST(ReplScreen, PlaceholderPriorityOrder) {
    // Verify the full priority cascade (TS REF: PromptInput.tsx line 2014 +
    // usePromptInputPlaceholder.ts).
    //
    // Priority when NOT viewing agent:
    //   AI suggestion > queue hint > example > none
    // Priority when viewing agent:
    //   viewing agent hint > queue hint > example > none  (AI suggestion is
    //     suppressed by !viewingAgentTaskId in TS showPromptSuggestion)
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 0;
    state.input_mode = repl::InputMode::Normal;
    state.prompt_suggestion_enabled = true;
    state.has_editable_queued_commands = true;
    state.queued_command_hint_shown_count = 0;
    state.next_action_suggestion = "do something";

    // ── Case 1: not viewing agent, all other conditions set ──
    // AI suggestion wins (highest priority when !viewingAgent).
    auto p1 = repl::ComputePlaceholder(state);
    EXPECT_EQ(*p1, "do something");

    // ── Case 2: viewing agent, all conditions set ──
    // Viewing agent hint wins (AI suggestion suppressed by !viewingAgentTaskId).
    state.viewing_agent_name = "agent1";
    auto p2 = repl::ComputePlaceholder(state);
    EXPECT_EQ(*p2, "Message @agent1…");

    // ── Case 3: viewing agent, no AI suggestion ──
    // Viewing agent still wins.
    state.next_action_suggestion.reset();
    auto p3 = repl::ComputePlaceholder(state);
    EXPECT_EQ(*p3, "Message @agent1…");

    // ── Case 4: not viewing agent, no AI suggestion, has queue hint ──
    // Queue hint wins.
    state.viewing_agent_name.reset();
    auto p4 = repl::ComputePlaceholder(state);
    EXPECT_EQ(*p4, "Press up to edit queued messages");

    // ── Case 5: not viewing agent, no AI suggestion, no queue hint ──
    // Example wins (submit_count == 0).
    state.has_editable_queued_commands = false;
    auto p5 = repl::ComputePlaceholder(state);
    EXPECT_NE(p5->find("Try"), std::string::npos);

    // ── Case 6: after submit, no other conditions ──
    // No placeholder.
    state.submit_count = 1;
    auto p6 = repl::ComputePlaceholder(state);
    EXPECT_FALSE(p6.has_value());
}



TEST(ReplScreen, PlaceholderRenderedInPromptInput) {
    // End-to-end: verify the computed placeholder actually appears in the
    // rendered prompt input element.
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_text = "";
    state.submit_count = 0;
    state.prompt_suggestion_enabled = true;

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderPromptInput(state, 80),
        80,
        6));

    // The placeholder "Try \"fix lint errors\"" should appear in the rendered output.
    EXPECT_NE(rendered.find("fix lint errors"), std::string::npos);
    EXPECT_NE(rendered.find("Try"), std::string::npos);
    // And the ❯ prefix glyph should be there.
    EXPECT_NE(rendered.find("\xE2\x9D\xAF"), std::string::npos);  // ❯
}



// ─── User prompt truncation (TS REF: UserPromptMessage.tsx lines 28-70) ─

TEST(ReplScreen, UserPromptTruncationShortTextUnchanged) {
    // Messages <= 10_000 chars pass through unmodified.
    namespace msgs = cc::ui::messages;
    std::string short_text = "hello world";
    auto result = msgs::TruncateUserPromptText(short_text);
    EXPECT_EQ(result, short_text);
}



TEST(ReplScreen, UserPromptTruncationExactLimitUnchanged) {
    // Exactly at the 10_000 char limit — no truncation.
    namespace msgs = cc::ui::messages;
    std::string exact(10'000, 'x');
    auto result = msgs::TruncateUserPromptText(exact);
    EXPECT_EQ(result.size(), 10'000u);
    EXPECT_EQ(result, exact);
}



TEST(ReplScreen, UserPromptTruncationLongTextHeadTailSplit) {
    // > 10_000 chars: head 2500 + separator + tail 2500.
    namespace msgs = cc::ui::messages;
    std::string long_text(15'000, 'a');
    // Mark head and tail boundaries for verification.
    long_text.replace(0, 5, "HEAD!");
    long_text.replace(14'995, 5, "TAIL!");
    auto result = msgs::TruncateUserPromptText(long_text);
    // Head preserved.
    EXPECT_NE(result.find("HEAD!"), std::string::npos);
    // Tail preserved.
    EXPECT_NE(result.find("TAIL!"), std::string::npos);
    // Separator present (ellipsis U+2026 = \xe2\x80\xa6).
    EXPECT_NE(result.find("\xe2\x80\xa6 +"), std::string::npos);  // "… +"
    EXPECT_NE(result.find(" lines \xe2\x80\xa6"), std::string::npos);  // " lines …"
    // Result is much shorter than original.
    EXPECT_LT(result.size(), 6'000u);  // 2500 + ~40 sep + 2500
}



TEST(ReplScreen, UserPromptTruncationHiddenLineCount) {
    // Verify hidden line count in the separator.
    namespace msgs = cc::ui::messages;
    // Build text: 2500 chars of "head\n" repeated (to get many newlines
    // in the hidden region), then filler.
    std::string text;
    text.reserve(12'000);
    // Head region (first 2500 chars): few newlines.
    text += std::string(2'400, 'x');
    text += "\n";
    text += std::string(99, 'x');
    // Hidden region: 50 newlines spread across ~2000 chars.
    for (int i = 0; i < 50; ++i) text += "line\n";
    // Fill to exceed 10K.
    while (text.size() < 10'500) text += 'y';
    // Tail region: last 2500 chars, with 2 newlines.
    text += "\ntail1\ntail2";
    auto result = msgs::TruncateUserPromptText(text);
    // The separator should show hidden line count.
    // Newlines from head-end to text-end: 50 (hidden region) + 2 (tail) = 52.
    // Minus newlines in tail: 2.
    // So hidden_lines = 52 - 2 = 50.
    EXPECT_NE(result.find("+50 lines"), std::string::npos);
}



TEST(ReplScreen, UserPromptTruncationNoNewlinesShowsZero) {
    // Single long line with no newlines — hidden count = 0.
    namespace msgs = cc::ui::messages;
    std::string long_line(12'000, 'z');
    auto result = msgs::TruncateUserPromptText(long_line);
    EXPECT_NE(result.find("+0 lines"), std::string::npos);
}



TEST(ReplScreen, UserPromptTruncationRenderedInMessage) {
    // End-to-end: RenderUserPromptMessage uses truncated text.
    namespace msgs = cc::ui::messages;
    msgs::UserTextMessageData data;
    data.content = std::string(11'000, 'q');
    data.content.replace(0, 8, "MARKER_H");
    data.content.replace(10'992, 8, "MARKER_T");
    auto el = msgs::RenderUserPromptMessage(data, false, true);
    auto rendered = strip_ansi(render_to_plain_text(el, 80, 100));
    // Head marker visible.
    EXPECT_NE(rendered.find("MARKER_H"), std::string::npos);
    // Tail marker visible.
    EXPECT_NE(rendered.find("MARKER_T"), std::string::npos);
    // Truncation separator visible.
    EXPECT_NE(rendered.find("lines \xe2\x80\xa6"), std::string::npos);
}
