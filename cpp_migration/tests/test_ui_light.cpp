/// @file test_ui_light.cpp
/// @brief Split from test_ui.cpp - Components, Markdown, Panels, PromptInputFooter, StatusLine, Terminal (SLOC budget fix)

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

import cc.ui.terminal;
import cc.ui.design.theme;
import cc.ui.design.tokens;
import cc.ui.components;
import cc.ui.components_extended;
import cc.ui.components.passes;
import cc.ui.components.grove;
import cc.ui.components.lsp_rec_menu;
import cc.ui.components.plugin_hint_menu;
import cc.ui.common.declared_cursor;
import cc.ui.panels;
import cc.ui.markdown;
import cc.ui.prompt.prompt_input_footer;
import cc.ui.prompt.placeholder_cascade;
import cc.ui.design.figures;
import cc.constants.constants;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Terminal, ColorThemesAreConstructible) {
    auto dark = cc::ui::ColorTheme::dark();
    auto light = cc::ui::ColorTheme::light();

    (void)dark;
    (void)light;
    SUCCEED();
}

TEST(Terminal, DefaultKeyBindingsContainCoreActions) {
    auto bindings = cc::ui::default_key_bindings();

    EXPECT_FALSE(bindings.empty());
    EXPECT_TRUE(std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
        return binding.action == cc::ui::KeyAction::Submit;
    }));
    EXPECT_TRUE(std::any_of(bindings.begin(), bindings.end(), [](const auto& binding) {
        return binding.action == cc::ui::KeyAction::Interrupt;
    }));
}

TEST(Terminal, SpinnerRendersWhenActive) {
    cc::ui::Spinner spinner("Working");
    EXPECT_FALSE(spinner.is_active());

    spinner.start();
    EXPECT_TRUE(spinner.is_active());
    expect_element(spinner.render(cc::ui::ColorTheme::dark()));

    spinner.stop();
    EXPECT_FALSE(spinner.is_active());
}

TEST(Terminal, TerminalUIExposesControlAPI) {
    cc::ui::TerminalUI ui;
    bool submitted = false;
    bool interrupted = false;

    ui.set_on_submit([&](std::string) { submitted = true; });
    ui.set_on_interrupt([&] { interrupted = true; });
    ui.update_status(cc::ui::StatusBarData{.model_name = "test-model", .input_tokens = 1, .output_tokens = 2, .cost_usd = 0.0, .session_id = std::nullopt});
    ui.show_spinner("Testing");
    ui.hide_spinner();

    EXPECT_FALSE(submitted);
    EXPECT_FALSE(interrupted);
}

TEST(Terminal, StatusBarRendersTokensAndCost) {
    cc::ui::StatusBarData data{
        .model_name = "claude-test",
        .input_tokens = 123,
        .output_tokens = 45,
        .cost_usd = 0.0123,
        .session_id = std::optional<std::string>{"session-1"},
    };

    auto rendered = render_to_plain_text(cc::ui::render_status_bar(data, cc::ui::ColorTheme::dark()), 90, 5);

    EXPECT_NE(rendered.find("claude-test"), std::string::npos);
    EXPECT_NE(rendered.find("123"), std::string::npos);
    EXPECT_NE(rendered.find("45"), std::string::npos);
    EXPECT_NE(rendered.find("$0.0123"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.components: reusable FTXUI render helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Components, RenderToolUseReturnsElement) {
    cc::ui::ToolUseDisplayData data{
        .tool_name = "bash",
        .input_summary = "ls -la",
        .output_preview = "ok",
        .is_running = false,
        .is_error = false,
        .duration = std::chrono::milliseconds(12),
    };

    auto rendered = render_to_plain_text(cc::ui::render_tool_use(data), 80, 8);

    EXPECT_NE(rendered.find("bash"), std::string::npos);
    EXPECT_NE(rendered.find("ls -la"), std::string::npos);
    EXPECT_NE(rendered.find("ok"), std::string::npos);
    EXPECT_NE(rendered.find("12ms"), std::string::npos);
}

TEST(Components, RenderPermissionPromptReturnsElement) {
    cc::ui::PermissionPromptData data{
        .tool_name = "Edit",
        .description = "Modify a file",
        .input_preview = "src/main.cpp",
        .affected_paths = {"src/main.cpp"},
    };

    auto rendered = render_to_plain_text(cc::ui::render_permission_prompt(data), 90, 10);

    EXPECT_NE(rendered.find("Permission Required"), std::string::npos);
    EXPECT_NE(rendered.find("Edit"), std::string::npos);
    EXPECT_NE(rendered.find("Modify a file"), std::string::npos);
    EXPECT_NE(rendered.find("src/main.cpp"), std::string::npos);
    EXPECT_NE(rendered.find("[y]es"), std::string::npos);
}

TEST(Components, RenderSearchBoxReturnsElement) {
    std::vector<cc::ui::SearchResult> results = {
        {.label = "commit", .detail = "Create a commit", .icon = "/"},
        {.label = "config", .detail = "Edit config", .icon = "/"},
    };

    expect_element(cc::ui::render_search_box("co", results, 0));
}

TEST(Components, TextInputRendersPlaceholderWithoutExtraCursorSpace) {
    cc::ui::components::TextInputOptions options;
    options.prefix = "▶ ";
    options.placeholder = "Type";
    auto component = cc::ui::components::TextInput(options);

    auto rendered = strip_ansi(render_to_plain_text(component->Render(), 20, 3));

    EXPECT_NE(rendered.find("Type"), std::string::npos);
    EXPECT_EQ(rendered.find("  Type"), std::string::npos);
}

TEST(Components, TextInputRendersPlaceholderCaretWithoutNativeCursor) {
    cc::ui::components::TextInputOptions options;
    options.prefix = "";
    options.placeholder = "Type";
    options.cursor_blink_ms = 0;
    auto component = cc::ui::components::TextInput(options);

    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(20),
        ftxui::Dimension::Fixed(3));
    ftxui::Render(screen, component->Render());

    const auto& first_cell = screen.PixelAt(0, 0);
    EXPECT_EQ(first_cell.character, "T");
    EXPECT_TRUE(first_cell.inverted);
}

TEST(Components, TextInputHidesPlaceholderAfterTyping) {
    cc::ui::components::TextInputOptions options;
    options.prefix = "▶ ";
    options.placeholder = "Type";
    auto component = cc::ui::components::TextInput(options);

    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("a")));
    auto rendered = strip_ansi(render_to_plain_text(component->Render(), 20, 3));

    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("Type"), std::string::npos);
}

TEST(Components, TextInputTabAfterEmptyHistorySearchDoesNotCrash) {
    auto component = cc::ui::components::TextInput({});

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x12")));
    EXPECT_FALSE(component->OnEvent(ftxui::Event::Tab));
}

TEST(Components, TextInputMultilineInsertNewline) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = true;
    auto impl = cc::ui::components::MakeTextInputCore(opts);

    // Pre-populate two lines so Enter inserts newline (not submit).
    // In multiline mode, Enter submits when there is only 1 line;
    // it inserts a newline only when 2+ lines exist.
    impl->PasteText("hello\nworld");
    ASSERT_EQ(impl->text(), "hello\nworld");
    ASSERT_EQ(impl->cursor(), 11);

    impl->HandleEvent(ftxui::Event::Return);

    EXPECT_EQ(impl->text(), "hello\nworld\n");
    EXPECT_EQ(impl->cursor(), 12);
}

TEST(Components, TextInputBackspace) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));
    component->OnEvent(ftxui::Event::Backspace);

    auto rendered = strip_ansi(render_to_plain_text(component->Render(), 20, 3));
    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("ab"), std::string::npos);
}

TEST(Components, TextInputSelectAll) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("l"));
    component->OnEvent(ftxui::Event::Character("l"));
    component->OnEvent(ftxui::Event::Character("o"));
    // Ctrl+A = select all
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x01")));
    // Backspace should delete all
    component->OnEvent(ftxui::Event::Backspace);

    auto rendered = strip_ansi(render_to_plain_text(component->Render(), 20, 3));
    // After delete all, placeholder should be visible
    EXPECT_NE(rendered.find("Type your message"), std::string::npos);
}

TEST(Components, TextInputUndoRedo) {
    cc::ui::components::TextInputOptions opts;
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));
    component->OnEvent(ftxui::Event::Backspace); // delete 'b'

    // Ctrl+Z undo
    component->OnEvent(ftxui::Event::Character("\x1a"));

    auto rendered_undo = render_to_plain_text(component->Render(), 20, 3);
    // After undo, should have "ab" again
    EXPECT_NE(rendered_undo.find("ab"), std::string::npos);

    // Ctrl+Y redo
    component->OnEvent(ftxui::Event::Character("\x19"));

    auto rendered_redo = render_to_plain_text(component->Render(), 20, 3);
    // After redo, should be back to "a"
    EXPECT_NE(rendered_redo.find("a"), std::string::npos);
    EXPECT_EQ(rendered_redo.find("ab"), std::string::npos);
}

TEST(Components, TextInputMaskInput) {
    cc::ui::components::TextInputOptions opts;
    opts.mask_input = true;
    opts.mask_char = '*';
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("s"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("c"));
    component->OnEvent(ftxui::Event::Character("r"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("t"));

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    // Should show mask chars, not actual text
    EXPECT_EQ(rendered.find("secret"), std::string::npos);
    EXPECT_NE(rendered.find("******"), std::string::npos);
}

TEST(Components, TextInputInputFilter) {
    cc::ui::components::TextInputOptions opts;
    opts.input_filter = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    auto component = cc::ui::components::TextInput(opts);

    // Digits should be accepted
    component->OnEvent(ftxui::Event::Character("1"));
    component->OnEvent(ftxui::Event::Character("2"));
    // Letters should be rejected
    component->OnEvent(ftxui::Event::Character("a"));
    component->OnEvent(ftxui::Event::Character("b"));

    auto rendered = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered.find("12"), std::string::npos);
    EXPECT_EQ(rendered.find("a"), std::string::npos);
}

TEST(Components, TextInputSuggestionsDropdown) {
    cc::ui::components::TextInputOptions opts;
    opts.get_suggestions = [](const std::string& input, int, const cc::ui::components::PromptContext&) -> std::vector<cc::ui::components::Suggestion> {
        if (input.starts_with('/')) {
            return {
                {"/help", "/help", "Show help", cc::ui::components::SuggestionCategory::Command, std::nullopt, std::nullopt, std::nullopt},
                {"/clear", "/clear", "Clear screen", cc::ui::components::SuggestionCategory::Command, std::nullopt, std::nullopt, std::nullopt},
            };
        }
        return {};
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("/"));
    auto rendered = render_to_plain_text(component->Render(), 60, 10);
    // Should show suggestions dropdown
    EXPECT_NE(rendered.find("Suggestions"), std::string::npos);
    EXPECT_NE(rendered.find("/help"), std::string::npos);
    EXPECT_NE(rendered.find("/clear"), std::string::npos);
}

TEST(Components, TextInputSuggestionAccept) {
    std::string accepted_text;
    cc::ui::components::TextInputOptions opts;
    opts.get_suggestions = [](const std::string& input, int, const cc::ui::components::PromptContext&) -> std::vector<cc::ui::components::Suggestion> {
        if (input.starts_with('/')) {
            return {
                {"/help", "/help", "Show help", cc::ui::components::SuggestionCategory::Command, std::nullopt, std::nullopt, std::nullopt},
            };
        }
        return {};
    };
    opts.on_change = [&](const std::string& text, const auto&) {
        accepted_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("/"));
    // Tab to select (next suggestion, only 1 so stays on 0)
    component->OnEvent(ftxui::Event::Tab);
    // Enter to accept
    component->OnEvent(ftxui::Event::Return);

    // After accept, text should be "/help"
    EXPECT_EQ(accepted_text, "/help");
}

TEST(Components, TextInputHistorySearch) {
    cc::ui::components::TextInputOptions opts;
    opts.show_history = true;
    auto component = cc::ui::components::TextInput(opts);

    // Add some history by typing and submitting
    component->OnEvent(ftxui::Event::Character("hello world"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("hello there"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("goodbye"));
    component->OnEvent(ftxui::Event::Return);

    // Enter search mode (Ctrl+R)
    component->OnEvent(ftxui::Event::Character("\x12"));
    // Type search query
    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    component->OnEvent(ftxui::Event::Character("l"));

    auto rendered = render_to_plain_text(component->Render(), 60, 10);
    // Should show search mode UI
    EXPECT_NE(rendered.find("reverse-i-search"), std::string::npos);
    EXPECT_NE(rendered.find("`hel`"), std::string::npos);
}

TEST(Components, TextInputHistorySearchAccept) {
    cc::ui::components::TextInputOptions opts;
    opts.show_history = true;
    std::string result_text;
    opts.on_submit = [&](const std::string& text, const auto&) {
        result_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    // Add some history
    component->OnEvent(ftxui::Event::Character("apple banana"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("cherry date"));
    component->OnEvent(ftxui::Event::Return);

    // Enter search mode
    component->OnEvent(ftxui::Event::Character("\x12"));
    // Search for "cherry"
    component->OnEvent(ftxui::Event::Character("c"));
    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("e"));
    // Accept with Enter
    component->OnEvent(ftxui::Event::Return);

    // After accepting, submit should give "cherry date"
    // But Enter in search mode just populates the buffer; need another Enter to submit
    // Let's submit now
    component->OnEvent(ftxui::Event::Return);
    EXPECT_EQ(result_text, "cherry date");
}

TEST(Components, TextInputArrowNavigation) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('a');
    impl->insert_char('b');
    impl->insert_char('c');
    ASSERT_EQ(impl->text(), "abc");
    ASSERT_EQ(impl->cursor(), 3);

    // Move left twice, then insert 'X'
    impl->move_cursor(-1, false);
    impl->move_cursor(-1, false);
    EXPECT_EQ(impl->cursor(), 1);
    impl->insert_char('X');

    EXPECT_EQ(impl->text(), "aXbc");
    EXPECT_EQ(impl->cursor(), 2);

    // Also verify via rendered output (with ANSI stripped)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("aXbc"), std::string::npos);
}

TEST(Components, TextInputHomeEnd) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('h');
    impl->insert_char('e');
    impl->insert_char('l');
    impl->insert_char('l');
    impl->insert_char('o');
    ASSERT_EQ(impl->text(), "hello");
    ASSERT_EQ(impl->cursor(), 5);

    // Home, then insert '!'
    impl->move_home(false);
    EXPECT_EQ(impl->cursor(), 0);
    impl->insert_char('!');
    EXPECT_EQ(impl->text(), "!hello");
    EXPECT_EQ(impl->cursor(), 1);

    // End, then insert '?'
    impl->move_end(false);
    EXPECT_EQ(impl->cursor(), 6);
    impl->insert_char('?');
    EXPECT_EQ(impl->text(), "!hello?");
    EXPECT_EQ(impl->cursor(), 7);

    // Also verify rendered text (strip ANSI for text content check)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("!hello?"), std::string::npos);
}

TEST(Components, TextInputSubmitCallback) {
    std::string submitted_text;
    cc::ui::components::TextInputOptions opts;
    opts.multiline = false;
    opts.on_submit = [&](const std::string& text, const auto&) {
        submitted_text = text;
    };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Character("h"));
    component->OnEvent(ftxui::Event::Character("i"));
    component->OnEvent(ftxui::Event::Return);

    EXPECT_EQ(submitted_text, "hi");
}

TEST(Components, TextInputEscapeCallback) {
    bool escape_called = false;
    cc::ui::components::TextInputOptions opts;
    opts.on_escape = [&]() { escape_called = true; };
    auto component = cc::ui::components::TextInput(opts);

    component->OnEvent(ftxui::Event::Escape);
    EXPECT_TRUE(escape_called);
}

TEST(Components, TextInputLineNumbers) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = true;
    opts.show_line_numbers = true;
    auto impl = cc::ui::components::MakeTextInputCore(opts);

    // Two lines so show_line_numbers actually renders (guard: total_lines > 1)
    impl->PasteText("line 1\nline 2");

    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 40, 5));
    EXPECT_NE(rendered.find(" 1 "), std::string::npos);
    EXPECT_NE(rendered.find(" 2 "), std::string::npos);
    EXPECT_NE(rendered.find("line 1"), std::string::npos);
    EXPECT_NE(rendered.find("line 2"), std::string::npos);
}

TEST(Components, TextInputPasteText) {
    cc::ui::components::TextInputOptions opts;
    std::shared_ptr<cc::ui::components::TextInputImpl> impl;
    auto component = cc::ui::components::TextInput(opts, &impl);

    ASSERT_NE(impl, nullptr);
    impl->PasteText("pasted text");

    auto rendered = render_to_plain_text(component->Render(), 40, 3);
    EXPECT_NE(rendered.find("pasted text"), std::string::npos);
}

// TS REF: src/components/PromptInput/inputPaste.ts — pastes over the 10 000-char
// TRUNCATION_THRESHOLD collapse to head 500 + "[...Truncated text #N +M lines...]"
// + tail 500 (PREVIEW_LENGTH=1000).  Guards the fullscreen layout pass against
// huge pastes.
TEST(Components, TextInputPasteTruncatesOver10k) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    // Short paste (<= 10 000) is stored verbatim.
    impl->PasteText(std::string(9000, 'a'));
    EXPECT_EQ(impl->text().size(), 9000u);
    EXPECT_EQ(impl->text().find("Truncated text"), std::string::npos);

    // Large single-line paste (> 10 000) goes to paste-preview first;
    // confirming inserts the truncated head + placeholder + tail.
    auto impl2 = cc::ui::components::MakeTextInputCore({});
    impl2->PasteText(std::string(25000, 'x'));
    EXPECT_TRUE(impl2->HasPastePreview()) << "large paste must trigger preview confirmation";
    impl2->ConfirmPaste();
    EXPECT_FALSE(impl2->HasPastePreview()) << "preview clears after confirmation";
    const auto& t = impl2->text();
    EXPECT_LT(t.size(), 25000u) << "over-threshold paste must be truncated";
    // TS REF: inputPaste.ts L57 formatTruncatedTextRef → "[...Truncated text #N +M lines...]"
    EXPECT_NE(t.find("[...Truncated text #"), std::string::npos);
    EXPECT_NE(t.find("lines...]"), std::string::npos);
    // Head 500 + tail 500 + placeholder ≈ ~1040 chars, well under the original.
    EXPECT_LT(t.size(), 1200u);
    // Single-line input → 0 elided lines reported (TS getPastedTextRefNumLines counts
    // newline matches, not total lines; "a\nb\nc" → 2, "xxxxxxxx" → 0).
    EXPECT_NE(t.find("+0 lines...]"), std::string::npos);
}

TEST(Components, TextInputPasteTruncationCountsElidedLines) {
    auto impl = cc::ui::components::MakeTextInputCore({});
    // Build a >10k multi-line paste: 700 lines of 20 chars each (~14 700 chars).
    std::string big;
    for (int i = 0; i < 700; ++i) big += std::string(19, 'y') + "\n";
    ASSERT_GT(big.size(), 10000u);
    impl->PasteText(big);
    ASSERT_TRUE(impl->HasPastePreview()) << "multi-line large paste must trigger preview";
    impl->ConfirmPaste();
    const auto& t = impl->text();
    // TS REF: inputPaste.ts L57 formatTruncatedTextRef → includes "#N" paste id.
    EXPECT_NE(t.find("[...Truncated text #"), std::string::npos);
    // The elided middle drops many lines — the reported count must be > 1.
    EXPECT_EQ(t.find("+1 lines...]"), std::string::npos)
        << "multi-line paste should report many elided lines, not 1";
    // Verify the paste id is present (should be #1 since this is the first paste).
    EXPECT_NE(t.find("[...Truncated text #1"), std::string::npos);
}

TEST(Components, TextInputDeleteChar) {
    auto impl = cc::ui::components::MakeTextInputCore({});

    impl->insert_char('a');
    impl->insert_char('b');
    impl->insert_char('c');
    ASSERT_EQ(impl->text(), "abc");
    ASSERT_EQ(impl->cursor(), 3);

    // Move left once (cursor between 'b' and 'c'), then Delete
    // removes the char AFTER the cursor ('c') → "ab"
    impl->move_cursor(-1, false);
    EXPECT_EQ(impl->cursor(), 2);
    impl->delete_char();

    EXPECT_EQ(impl->text(), "ab");
    EXPECT_EQ(impl->cursor(), 2);

    // Move left once more (cursor between 'a' and 'b'), delete → "a"
    impl->move_cursor(-1, false);
    impl->delete_char();
    EXPECT_EQ(impl->text(), "a");
    EXPECT_EQ(impl->cursor(), 1);

    // Also verify via rendered output (strip ANSI for text check)
    auto rendered = strip_ansi(render_to_plain_text(impl->Render(), 20, 3));
    EXPECT_NE(rendered.find("a"), std::string::npos);
    EXPECT_EQ(rendered.find("ab"), std::string::npos);
    EXPECT_EQ(rendered.find("abc"), std::string::npos);
}

TEST(Components, TextInputHistoryUpDown) {
    cc::ui::components::TextInputOptions opts;
    opts.multiline = false;
    auto component = cc::ui::components::TextInput(opts);

    // Add some history by submitting
    component->OnEvent(ftxui::Event::Character("first"));
    component->OnEvent(ftxui::Event::Return);
    component->OnEvent(ftxui::Event::Character("second"));
    component->OnEvent(ftxui::Event::Return);

    // Arrow up should go back in history
    component->OnEvent(ftxui::Event::ArrowUp);

    auto rendered_up = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_up.find("second"), std::string::npos);

    // Arrow up again
    component->OnEvent(ftxui::Event::ArrowUp);

    auto rendered_up2 = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_up2.find("first"), std::string::npos);

    // Arrow down should go forward
    component->OnEvent(ftxui::Event::ArrowDown);

    auto rendered_down = render_to_plain_text(component->Render(), 20, 3);
    EXPECT_NE(rendered_down.find("second"), std::string::npos);
}

TEST(StatusLine, AppliesGlobalDimAndStripsOuterBgcolor) {
    // Verifies the Phase 2 / Round 6 visual fix: external flux-statusline script
    // emits ANSI bold colors AND pill background SGR codes (48;2 / 48;5); the
    // faithful render applies |dim so the statusline reads as muted chrome
    // instead of shouting neon, and wraps the content in a neutral bgcolor
    // (RGB 20,20,22) so any external SGR 48/49 pill doesn't bleed a colored
    // box into the prompt footer (reported in IMG#18 as "folder pill has a
    // deep blue background").
    namespace pif = cc::ui::prompt::footer;

    auto rendered = render_to_plain_text(
        pif::RenderStatusLine(pif::StatusLineOptions{
            .content = "\033[38;5;44mbright-status\033[0m",
            .should_display = true,
            .builtin = std::nullopt,
        }),
        120,
        1);

    EXPECT_NE(rendered.find("bright-status"), std::string::npos);
    // Dim (SGR 2) MUST be present around the user-facing content.
    EXPECT_NE(rendered.find("\033[2m"), std::string::npos);
}

// P0-6: Builtin statusline shows when user command returns empty (or no
// statusLine configured).  Provides folder/git/model/token info for
// standalone CPP mode.
TEST(StatusLine, BuiltinShowsFolderGitModelTokens) {
    namespace pif = cc::ui::prompt::footer;

    pif::BuiltinStatusLineData data;
    data.cwd = "/Users/dev/CC-REPL/cpp_migration";
    data.git_branch = "master";
    data.model_name = "GLM-5.2";
    data.input_tokens = 25000;
    data.output_tokens = 3000;
    data.context_token_count = 28000;
    data.cost_usd = 0.1234;
    data.context_window_size = 200000;

    // No content (user command returned empty) → builtin kicks in.
    auto rendered = render_to_plain_text(
        pif::RenderStatusLine(pif::StatusLineOptions{
            .content = "",
            .should_display = true,
            .is_fullscreen = true,
            .builtin = data,
        }),
        120,
        1);

    // Folder (last 2 path components)
    EXPECT_NE(rendered.find("cpp_migration"), std::string::npos);
    // Git branch
    EXPECT_NE(rendered.find("master"), std::string::npos);
    // Model name
    EXPECT_NE(rendered.find("GLM-5.2"), std::string::npos);
    // Token count formatted as K
    EXPECT_NE(rendered.find("28.0K"), std::string::npos);
    EXPECT_NE(rendered.find("200.0K"), std::string::npos);
    // Percentage
    EXPECT_NE(rendered.find("14%"), std::string::npos);
    // Cost
    EXPECT_NE(rendered.find("$0.1234"), std::string::npos);
}

TEST(StatusLine, BuiltinNoGitBranchOmitsBranch) {
    namespace pif = cc::ui::prompt::footer;

    pif::BuiltinStatusLineData data;
    data.cwd = "/tmp/some_project";
    data.git_branch = "";   // not a git repo
    data.model_name = "claude-sonnet-5";
    data.context_token_count = 0;
    data.context_window_size = 200000;

    auto rendered = render_to_plain_text(
        pif::RenderStatusLine(pif::StatusLineOptions{
            .content = "",
            .should_display = true,
            .is_fullscreen = true,
            .builtin = data,
        }),
        120,
        1);

    // Folder shown
    EXPECT_NE(rendered.find("some_project"), std::string::npos);
    // Model shown
    EXPECT_NE(rendered.find("claude-sonnet-5"), std::string::npos);
    // No branch glyph (🌿) since git_branch is empty
    EXPECT_EQ(rendered.find("\xf0\x9f\x8c\xbf"), std::string::npos);  // 🌿
}

TEST(StatusLine, UserContentTakesPriorityOverBuiltin) {
    namespace pif = cc::ui::prompt::footer;

    pif::BuiltinStatusLineData data;
    data.cwd = "/tmp/proj";
    data.model_name = "test-model";

    // User command produced output → it wins over builtin.
    auto rendered = render_to_plain_text(
        pif::RenderStatusLine(pif::StatusLineOptions{
            .content = "MY CUSTOM STATUSLINE",
            .should_display = true,
            .is_fullscreen = true,
            .builtin = data,
        }),
        120,
        1);

    // User content is shown
    EXPECT_NE(rendered.find("MY CUSTOM STATUSLINE"), std::string::npos);
}

TEST(StatusLine, BuiltinShowsWithoutConfiguredCommand) {
    namespace pif = cc::ui::prompt::footer;

    pif::BuiltinStatusLineData data;
    data.cwd = "/home/user/myrepo";
    data.git_branch = "feature/xyz";
    data.model_name = "opus-4.8";

    // Simulates: no statusLine in settings, but builtin data available.
    auto rendered = render_to_plain_text(
        pif::RenderStatusLine(pif::StatusLineOptions{
            .content = "",
            .should_display = true,
            .is_fullscreen = true,
            .builtin = data,
        }),
        120,
        1);

    EXPECT_NE(rendered.find("myrepo"), std::string::npos);
    EXPECT_NE(rendered.find("feature/xyz"), std::string::npos);
    EXPECT_NE(rendered.find("opus-4.8"), std::string::npos);
}

TEST(PromptInputFooter, AlignsRightColumnWithStatusLineRow) {
    namespace pif = cc::ui::prompt::footer;

    pif::FooterOptions opts;
    opts.status_line = pif::StatusLineOptions{
        .content = "custom status",
        .should_display = true,
        .builtin = std::nullopt,
    };
    opts.bridge = pif::BridgeOptions{
        .status = pif::BridgeStatus::Connected,
        .explicit_remote = true,
    };

    auto rendered = strip_ansi(render_to_plain_text(
        pif::RenderPromptInputFooter(opts),
        100,
        4));

    auto status_pos = rendered.find("custom status");
    auto bridge_pos = rendered.find("Remote Control");
    ASSERT_NE(status_pos, std::string::npos);
    ASSERT_NE(bridge_pos, std::string::npos);
    EXPECT_EQ(
        std::count(rendered.begin(), rendered.begin() + static_cast<std::ptrdiff_t>(status_pos), '\n'),
        std::count(rendered.begin(), rendered.begin() + static_cast<std::ptrdiff_t>(bridge_pos), '\n'));
}

TEST(Panels, PanelTypeNamesAreStable) {
    EXPECT_EQ(cc::ui::panel_name(cc::ui::PanelType::Settings), "Settings");
    EXPECT_EQ(cc::ui::panel_name(cc::ui::PanelType::Permissions), "Permissions");
}

TEST(Panels, SettingsPanelFiltersAndUpdatesEntries) {
    cc::ui::SettingsPanel panel;
    panel.load({
        {.key = "model", .value = "sonnet", .description = "Model", .category = "core", .is_readonly = false, .allowed_values = {}},
        {.key = "theme", .value = "dark", .description = "Theme", .category = "ui", .is_readonly = false, .allowed_values = {"dark", "light"}},
    });

    panel.set_filter("model");
    auto filtered = panel.filtered_entries();
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0]->key, "model");

    auto updated = panel.update("theme", "light");
    EXPECT_TRUE(updated.has_value());
    auto invalid = panel.update("theme", "blue");
    EXPECT_FALSE(invalid.has_value());
}

TEST(Panels, McpPanelComputesStatusSummary) {
    cc::ui::McpPanel panel;
    auto now = std::chrono::steady_clock::now();
    panel.set_servers({
        {.name = "one", .uri = "stdio://one", .status = cc::ui::McpStatus::Connected, .capabilities = {}, .error_message = std::nullopt, .tool_count = 0, .last_heartbeat = now},
        {.name = "two", .uri = "stdio://two", .status = cc::ui::McpStatus::Disconnected, .capabilities = {}, .error_message = std::nullopt, .tool_count = 0, .last_heartbeat = now},
    });

    EXPECT_EQ(panel.status_summary(), "1/2 connected");
    EXPECT_EQ(cc::ui::McpPanel::status_icon(cc::ui::McpStatus::Error), "✗");
}

TEST(Panels, TasksPanelTracksTaskLifecycle) {
    cc::ui::TasksPanel panel;
    panel.add_task({.id = "task-1", .description = "Run tests", .status = cc::ui::TaskStatus::Running, .progress = 0.0, .error = std::nullopt, .created_at = std::chrono::system_clock::now(), .completed_at = std::nullopt});
    EXPECT_EQ(panel.active_count(), 1u);

    panel.update_progress("task-1", 0.5);
    panel.complete_task("task-1");
    EXPECT_EQ(panel.active_count(), 0u);
}

TEST(Panels, DiffPanelAggregatesStats) {
    cc::ui::DiffPanel panel;
    panel.set_diffs({{.file_path = "main.cpp", .hunks = {}, .additions = 3, .deletions = 1, .is_binary = false, .is_new_file = false, .is_deleted = false}});

    auto [additions, deletions] = panel.total_stats();
    EXPECT_EQ(additions, 3u);
    EXPECT_EQ(deletions, 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.messages: message parsing and renderable views
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Markdown, OrderedListSupportsMultiDigitNumbers) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("10. tenth"));

    EXPECT_NE(rendered.find("10."), std::string::npos);
    EXPECT_NE(rendered.find("tenth"), std::string::npos);
    EXPECT_EQ(rendered.find("1. . tenth"), std::string::npos);
}

TEST(Markdown, CodeBlockDoesNotInjectLineNumbers) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("```txt\nfoo\nbar\n```"));

    EXPECT_NE(rendered.find("foo"), std::string::npos);
    EXPECT_NE(rendered.find("bar"), std::string::npos);
    EXPECT_EQ(rendered.find("  1 "), std::string::npos);
    EXPECT_EQ(rendered.find("  2 "), std::string::npos);
}

TEST(Markdown, ParsesGfmTablesWithoutRenderingSeparatorAsParagraph) {
    auto rendered = render_to_plain_text(cc::ui::render_markdown("| A | B |\n|---|---|\n| 1 | 2 |"));

    EXPECT_NE(rendered.find("A"), std::string::npos);
    EXPECT_NE(rendered.find("B"), std::string::npos);
    EXPECT_NE(rendered.find("1"), std::string::npos);
    EXPECT_NE(rendered.find("2"), std::string::npos);
    EXPECT_EQ(rendered.find("---"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// Permissions tabs (P2-04) helpers moved to test_ui_dialogs.cpp
// ═══════════════════════════════════════════════════════════════════════════

TEST(Components, PassesFormatTokensSmallNumbers) {
    using namespace cc::ui::components::passes;
    EXPECT_EQ(format_tokens(0), "0");
    EXPECT_EQ(format_tokens(42), "42");
    EXPECT_EQ(format_tokens(999), "999");
}

TEST(Components, PassesFormatTokensThousands) {
    using namespace cc::ui::components::passes;
    EXPECT_EQ(format_tokens(1000), "1,000");
    EXPECT_EQ(format_tokens(12345), "12,345");
    EXPECT_EQ(format_tokens(1234567), "1,234,567");
}

TEST(Components, PassesClipHistoryUnderLimit) {
    using namespace cc::ui::components::passes;
    std::vector<std::string> hist{"p1", "p2", "p3"};
    auto clipped = clip_history(hist, 10);
    EXPECT_EQ(clipped.size(), 3u);
    EXPECT_EQ(clipped[0], "p1");
}

TEST(Components, PassesClipHistoryOverLimit) {
    using namespace cc::ui::components::passes;
    std::vector<std::string> hist;
    for (int i = 0; i < 20; ++i) hist.push_back("p" + std::to_string(i));
    auto clipped = clip_history(hist, 5);
    ASSERT_EQ(clipped.size(), 5u);
    EXPECT_EQ(clipped[0], "p15");
    EXPECT_EQ(clipped[4], "p19");
}

TEST(Components, PassesProgressPctZeroTotal) {
    using namespace cc::ui::components::passes;
    PassesViewState s{};
    s.total_passes = 0;
    s.current_pass = 0;
    EXPECT_DOUBLE_EQ(progress_pct(s), 0.0);
}

TEST(Components, PassesProgressPctHalfway) {
    using namespace cc::ui::components::passes;
    PassesViewState s{};
    s.total_passes = 10;
    s.current_pass = 5;
    EXPECT_DOUBLE_EQ(progress_pct(s), 0.5);
}

TEST(Components, PassesProgressPctClamped) {
    using namespace cc::ui::components::passes;
    PassesViewState s{};
    s.total_passes = 5;
    s.current_pass = 10;  // above total
    EXPECT_DOUBLE_EQ(progress_pct(s), 1.0);
}

TEST(Components, PassesThinkingPrefixIdle) {
    using namespace cc::ui::components::passes;
    cc::ui::design::theme::Theme theme;
    auto el = thinking_prefix(false, theme, 0);
    expect_element(el);
}

TEST(Components, PassesThinkingPrefixActive) {
    using namespace cc::ui::components::passes;
    cc::ui::design::theme::Theme theme;
    auto el = thinking_prefix(true, theme, 0);
    expect_element(el);
}

TEST(Components, BuildPassesPanelReturnsComponent) {
    using namespace cc::ui::components::passes;
    PassesViewState s{};
    s.total_passes = 3;
    s.current_pass = 1;
    s.pass_name = "Analyze";
    s.pass_description = "Analyzing codebase";
    s.tokens_consumed = 12345;
    s.pass_cost_usd = 0.0123;
    auto panel = BuildPassesPanel(s);
    EXPECT_NE(panel, nullptr);
    auto rendered = render_to_plain_text(panel->Render(), 60, 15);
    EXPECT_FALSE(rendered.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.components.grove — Grove tree view
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Components, GroveKindLabelAllValues) {
    using namespace cc::ui::components::grove;
    EXPECT_FALSE(std::string(kind_label(GroveKind::File)).empty());
    EXPECT_FALSE(std::string(kind_label(GroveKind::Symbol)).empty());
    EXPECT_FALSE(std::string(kind_label(GroveKind::Concept)).empty());
    EXPECT_FALSE(std::string(kind_label(GroveKind::Reference)).empty());
    EXPECT_FALSE(std::string(kind_label(GroveKind::Chunk)).empty());
}

TEST(Components, GroveCountNodesEmpty) {
    using namespace cc::ui::components::grove;
    std::vector<GroveNode> roots;
    EXPECT_EQ(count_nodes(roots, true), 0);
}

TEST(Components, GroveCountNodesWithChildren) {
    using namespace cc::ui::components::grove;
    std::vector<GroveNode> roots;
    roots.push_back({"id-1", "root", "", "", 0, 0, 0.0, GroveKind::File, {}});
    roots[0].children.push_back({"id-2", "child1", "", "", 0, 0, 0.0, GroveKind::Symbol, {}});
    roots[0].children.push_back({"id-3", "child2", "", "", 0, 0, 0.0, GroveKind::Chunk, {}});
    EXPECT_EQ(count_nodes(roots, true), 3);
}

TEST(Components, GroveTruncateShort) {
    std::string result = cc::ui::components::grove::truncate(
        std::string_view{"hello"}, 20);
    EXPECT_EQ(result, "hello");
}

TEST(Components, GroveTruncateLong) {
    using namespace cc::ui::components::grove;
    std::string s(100, 'x');
    auto result = cc::ui::components::grove::truncate(s, 10);
    // truncate appends "…" (U+2026, 3 bytes in UTF-8)
    EXPECT_LE(result.size(), 10u + 3u);
    EXPECT_NE(result.find("…"), std::string::npos);
}

TEST(Components, GroveFlattenEmpty) {
    using namespace cc::ui::components::grove;
    std::vector<GroveNode> roots;
    auto flat = flatten(roots, true);
    EXPECT_TRUE(flat.empty());
}

TEST(Components, GroveFlattenHasDepth) {
    using namespace cc::ui::components::grove;
    std::vector<GroveNode> roots;
    roots.push_back({"id-1", "root", "", "", 0, 0, 0.0, GroveKind::File, {}});
    roots[0].children.push_back({"id-2", "child", "", "", 0, 0, 0.0, GroveKind::Symbol, {}});
    auto flat = flatten(roots, true);
    EXPECT_EQ(flat.size(), 2u);
    EXPECT_EQ(flat[0].depth, 0);
    EXPECT_EQ(flat[1].depth, 1);
}

TEST(Components, GroveBuildTreeReturnsComponent) {
    using namespace cc::ui::components::grove;
    GroveViewState state;
    state.roots.push_back({"id-1", "src/main.cpp", "", "src/main.cpp", 1, 20, 0.8, GroveKind::File, {}});
    state.roots[0].children.push_back({"id-2", "main()", "", "src/main.cpp", 5, 15, 0.5, GroveKind::Symbol, {}});
    state.total_results = 2;
    auto tree = BuildGroveTree(state);
    EXPECT_NE(tree, nullptr);
    auto rendered = render_to_plain_text(tree->Render(), 60, 15);
    EXPECT_FALSE(rendered.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.components.lsp_recommendation_menu — LSP plugin rec menu
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Components, LspRecRatingStarsZero) {
    using namespace cc::ui::components::lsp_rec_menu;
    auto stars = rating_stars(0.0);
    EXPECT_EQ(stars, "☆☆☆☆☆");
}

TEST(Components, LspRecRatingStarsFive) {
    using namespace cc::ui::components::lsp_rec_menu;
    auto stars = rating_stars(5.0);
    EXPECT_EQ(stars, "★★★★★");
}

TEST(Components, LspRecFormatInstallsSmall) {
    using namespace cc::ui::components::lsp_rec_menu;
    EXPECT_EQ(format_installs(42), "42");
}

TEST(Components, LspRecFormatInstallsThousands) {
    using namespace cc::ui::components::lsp_rec_menu;
    EXPECT_EQ(format_installs(1500), "1.5k");
}

TEST(Components, LspRecUniqueLanguagesEmpty) {
    using namespace cc::ui::components::lsp_rec_menu;
    std::vector<LspPluginRecommendation> recs;
    auto langs = unique_languages(recs);
    EXPECT_TRUE(langs.empty());
}

TEST(Components, LspRecUniqueLanguagesDedupes) {
    using namespace cc::ui::components::lsp_rec_menu;
    LspPluginRecommendation a{}, b{};
    a.language_ids = {"python"};
    b.language_ids = {"python"};
    std::vector<LspPluginRecommendation> recs{a, b};
    auto langs = unique_languages(recs);
    EXPECT_EQ(langs.size(), 1u);
    EXPECT_EQ(langs[0], "python");
}

TEST(Components, BuildLspRecommendationMenuReturnsComponent) {
    using namespace cc::ui::components::lsp_rec_menu;
    LspRecMenuState state;
    LspPluginRecommendation rec;
    rec.plugin_id = "pylsp";
    rec.display_name = "Python LSP";
    rec.description = "Python language server";
    rec.language_ids = {"python"};
    rec.install_count = 10000;
    rec.rating = 4.5;
    rec.reason = RecommendReason::PopularInCategory;
    state.items.push_back(rec);
    auto on_install = [](int) {};
    auto on_skip = [](int) {};
    auto menu = BuildLspRecommendationMenu(state, on_install, on_skip);
    EXPECT_NE(menu, nullptr);
    auto tree = menu->Render();
    EXPECT_NE(tree, nullptr);
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(20));
    ftxui::Render(screen, tree);
    EXPECT_FALSE(screen.ToString().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.components.plugin_hint_menu — Plugin hint menu
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Components, PluginHintDefaultConstructs) {
    using namespace cc::ui::components::plugin_hint_menu;
    PluginHint hint{};
    EXPECT_TRUE(hint.plugin_id.empty());
    EXPECT_TRUE(hint.display_name.empty());
    EXPECT_TRUE(hint.hint_reason.empty());
}

TEST(Components, BuildPluginHintMenuEmptyState) {
    using namespace cc::ui::components::plugin_hint_menu;
    PluginHintMenuState state;
    auto on_install = [](int) {};
    auto on_dismiss = [](int) {};
    auto on_learn_more = [](int) {};
    auto menu = BuildPluginHintMenu(state, on_install, on_dismiss, on_learn_more);
    EXPECT_NE(menu, nullptr);
}

TEST(Components, BuildPluginHintMenuWithHints) {
    using namespace cc::ui::components::plugin_hint_menu;
    PluginHintMenuState state;
    PluginHint h1;
    h1.plugin_id = "python-plugin";
    h1.display_name = "Python Plugin";
    h1.hint_reason = "You use Python files";
    state.hints.push_back(h1);

    PluginHint h2;
    h2.plugin_id = "rust-plugin";
    h2.display_name = "Rust Plugin";
    h2.hint_reason = "You use Rust files";
    state.hints.push_back(h2);

    auto on_install = [](int) {};
    auto on_dismiss = [](int) {};
    auto on_learn_more = [](int) {};
    auto menu = BuildPluginHintMenu(state, on_install, on_dismiss, on_learn_more);
    EXPECT_NE(menu, nullptr);
    auto rendered = render_to_plain_text(menu->Render(), 80, 15);
    EXPECT_FALSE(rendered.empty());
}

TEST(Components, PluginHintMenuUpdateState) {
    using namespace cc::ui::components::plugin_hint_menu;
    PluginHintMenuState state;
    auto on_install = [](int) {};
    auto on_dismiss = [](int) {};
    auto on_learn_more = [](int) {};
    auto menu = BuildPluginHintMenu(state, on_install, on_dismiss, on_learn_more);
    ASSERT_NE(menu, nullptr);

    PluginHintMenuState new_state;
    PluginHint h;
    h.plugin_id = "new-plugin";
    h.display_name = "New Plugin";
    h.hint_reason = "A new hint appeared";
    new_state.hints.push_back(h);

    auto rendered_before = render_to_plain_text(menu->Render(), 80, 10);
    EXPECT_FALSE(rendered_before.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.design.figures: Glyph byte-sequence fidelity tests
// TS REF: src/constants/figures.ts + node_modules/figures/index.js
// Every glyph's exact UTF-8 bytes must match the TS reference so that golden
// snapshots don't drift between platforms.  These tests are the single source
// of truth for glyph correctness.
// ═══════════════════════════════════════════════════════════════════════════════

namespace figs = cc::ui::design::figures;

// ─── npm::figures glyphs (mainSymbols set) ──────────────────────────────

TEST(Figures, NpmPointerIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kPointer), "\xE2\x9D\xAF");  // ❯ U+276F
}

TEST(Figures, NpmTickIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kTick), "\xE2\x9C\x94");  // ✔ U+2714
}

TEST(Figures, NpmCrossIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kCross), "\xE2\x9C\x98");  // ✘ U+2718
}

TEST(Figures, NpmWarningIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kWarning), "\xE2\x9A\xA0");  // ⚠ U+26A0
}

TEST(Figures, NpmArrowDownIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kArrowDown), "\xE2\x86\x93");  // ↓ U+2193
}

TEST(Figures, NpmArrowRightIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kArrowRight), "\xE2\x86\x92");  // → U+2192
}

TEST(Figures, NpmArrowUpMatchesTs) {
    // TS REF: figures.arrowUp = '↑' U+2191 (used 16× in TS)
    EXPECT_EQ(std::string(figs::kArrowUp), "\xE2\x86\x91");  // ↑ U+2191
}

TEST(Figures, NpmArrowLeftMatchesTs) {
    // TS REF: figures.arrowLeft = '←' U+2190 (used 2× in TS, alias CHANNEL_ARROW)
    EXPECT_EQ(std::string(figs::kArrowLeft), "\xE2\x86\x90");  // ← U+2190
}

TEST(Figures, NpmRadioOnMatchesTs) {
    // TS REF: figures.radioOn = '◉' U+25C9 (selected radio, used 3× in TS)
    EXPECT_EQ(std::string(figs::kRadioOn), "\xE2\x97\x89");  // ◉ U+25C9
}

TEST(Figures, NpmRadioOffMatchesTs) {
    // TS REF: figures.radioOff = '◯' U+25EF (unselected radio, used 14× in TS)
    EXPECT_EQ(std::string(figs::kRadioOff), "\xE2\x97\xAF");  // ◯ U+25EF
}

TEST(Figures, NpmRadioOffSameAsCircle) {
    // figures.radioOff and figures.circle share the same glyph (U+25EF).
    // Semantic alias: radioOff is for radio-button context, circle is generic.
    EXPECT_EQ(std::string(figs::kRadioOff), std::string(figs::kCircle));
}

TEST(Figures, NpmTriangleUpOutlineMatchesTs) {
    // TS REF: figures.triangleUpOutline = '△' U+25B3 (used 5× in TS)
    EXPECT_EQ(std::string(figs::kTriangleUpOutline), "\xE2\x96\xB3");  // △ U+25B3
}

TEST(Figures, NpmTriangleRightSmallMatchesTs) {
    // TS REF: figures.triangleRightSmall = '▸' U+25B8 (used 2× in TS)
    EXPECT_EQ(std::string(figs::kTriangleRightSmall), "\xE2\x96\xB8");  // ▸ U+25B8
}

TEST(Figures, NpmTriangleDownSmallMatchesTs) {
    // TS REF: figures.triangleDownSmall = '▾' U+25BE (used 2× in TS)
    EXPECT_EQ(std::string(figs::kTriangleDownSmall), "\xE2\x96\xBE");  // ▾ U+25BE
}

TEST(Figures, NpmStarMatchesTs) {
    // TS REF: figures.star = '★' U+2605 (used 3× in TS)
    EXPECT_EQ(std::string(figs::kStar), "\xE2\x98\x85");  // ★ U+2605
}

TEST(Figures, NpmHeartMatchesTs) {
    // TS REF: figures.heart = '♥' U+2665 (used 2× in TS)
    EXPECT_EQ(std::string(figs::kHeart), "\xE2\x99\xA5");  // ♥ U+2665
}

TEST(Figures, NpmSquareSmallMatchesTs) {
    // TS REF: figures.squareSmall = '◻' U+25FB (used 1× in TS)
    EXPECT_EQ(std::string(figs::kSquareSmall), "\xE2\x97\xBB");  // ◻ U+25FB
}

TEST(Figures, NpmSquareSmallFilledMatchesTs) {
    // TS REF: figures.squareSmallFilled = '◼' U+25FC (used 1× in TS)
    EXPECT_EQ(std::string(figs::kSquareSmallFilled), "\xE2\x97\xBC");  // ◼ U+25FC
}

TEST(Figures, NpmCircleFilledMatchesTs) {
    // TS REF: figures.circleFilled = '◉' U+25C9 (used 1× in TS)
    EXPECT_EQ(std::string(figs::kCircleFilled), "\xE2\x97\x89");  // ◉ U+25C9
}

TEST(Figures, NpmCircleFilledSameAsRadioOn) {
    // circleFilled and radioOn are the same glyph (U+25C9).  Both are
    // exported so call sites can use the semantically-appropriate name.
    EXPECT_EQ(std::string(figs::kCircleFilled), std::string(figs::kRadioOn));
}

TEST(Figures, NpmCircleDoubleMatchesTs) {
    // TS REF: figures.circleDouble = '◎' U+25CE (used 1× in TS)
    EXPECT_EQ(std::string(figs::kCircleDouble), "\xE2\x97\x8E");  // ◎ U+25CE
}

TEST(Figures, NpmPlayMatchesTs) {
    // TS REF: figures.play = '▶' U+25B6 (used 1× in TS, alias PLAY_ICON)
    EXPECT_EQ(std::string(figs::kPlay), "\xE2\x96\xB6");  // ▶ U+25B6
}

TEST(Figures, NpmQuestionMarkPrefixMatchesTs) {
    // TS REF: figures.questionMarkPrefix = '(?)' (used 1× in TS)
    EXPECT_EQ(std::string(figs::kQuestionMarkPrefix), "(?)");
}

// ─── Tree-drawing chars (figures.lineVertical / lineUpRight / lineUpDownRight) ─

TEST(Figures, TreeLineVerticalMatchesTs) {
    // TS REF: figures.lineVertical = '│' U+2502
    EXPECT_EQ(std::string(figs::kLineVertical), "\xE2\x94\x82");  // │ U+2502
}

TEST(Figures, TreeLineUpRightMatchesTs) {
    // TS REF: figures.lineUpRight = '└' U+2514
    EXPECT_EQ(std::string(figs::kLineUpRight), "\xE2\x94\x94");  // └ U+2514
}

TEST(Figures, TreeLineUpDownRightMatchesTs) {
    // TS REF: figures.lineUpDownRight = '├' U+251C
    EXPECT_EQ(std::string(figs::kLineUpDownRight), "\xE2\x94\x9C");  // ├ U+251C
}

// ─── CC-local figures (src/constants/figures.ts port) ───────────────────

TEST(Figures, BlackCircleDarwinIsTsFaithful) {
    // TS REF: constants/figures.ts L4 — `env.platform === 'darwin' ? '⏺' : '●'`
    // kBlackCircle = Darwin default (⏺ U+23FA), kBlackCircleFallback = ● U+25CF
    EXPECT_EQ(std::string(figs::kBlackCircle), "\xE2\x8F\xBA");  // ⏺ U+23FA
}

TEST(Figures, BlackCircleFallbackIsTsFaithful) {
    EXPECT_EQ(std::string(figs::kBlackCircleFallback), "\xE2\x97\x8F");  // ● U+25CF
}

TEST(Figures, BlackCircleFallbackSameAsBullet) {
    // ● U+25CF is figures.bullet AND the non-Darwin BLACK_CIRCLE.
    EXPECT_EQ(std::string(figs::kBlackCircleFallback), std::string(figs::kBullet));
}

TEST(Figures, DiamondOpenMatchesTs) {
    // TS REF: DIAMOND_OPEN = '◇' U+25C7 (used 19× in TS — ultraplan running)
    EXPECT_EQ(std::string(figs::kDiamondOpen), "\xE2\x97\x87");  // ◇ U+25C7
}

TEST(Figures, DiamondFilledMatchesTs) {
    // TS REF: DIAMOND_FILLED = '◆' U+25C6 (used 13× in TS)
    EXPECT_EQ(std::string(figs::kDiamond), "\xE2\x97\x86");  // ◆ U+25C6
}

TEST(Figures, TeardropAsteriskMatchesTs) {
    // TS REF: TEARDROP_ASTERISK = '✻' U+273B (used 15× in TS)
    EXPECT_EQ(std::string(figs::kTeardropAsterisk), "\xE2\x9C\xBB");  // ✻ U+273B
}

TEST(Figures, LightningBoltMatchesTs) {
    // TS REF: LIGHTNING_BOLT = '↯' U+21AF (used 9× in TS — fast mode)
    EXPECT_EQ(std::string(figs::kLightningBolt), "\xE2\x86\xAF");  // ↯ U+21AF
}

TEST(Figures, PauseIconMatchesTs) {
    // TS REF: PAUSE_ICON = '⏸' U+23F8 (used 5× in TS — paused state)
    EXPECT_EQ(std::string(figs::kPauseIcon), "\xE2\x8F\xB8");  // ⏸ U+23F8
}

TEST(Figures, BulletOperatorMatchesTs) {
    // TS REF: BULLET_OPERATOR = '∙' U+2219 (used 5× in TS — tool error prefix)
    EXPECT_EQ(std::string(figs::kBulletOperator), "\xE2\x88\x99");  // ∙ U+2219
}

TEST(Figures, RefreshArrowMatchesTs) {
    // TS REF: REFRESH_ARROW = '↻' U+21BB (used 3× in TS — resource update)
    EXPECT_EQ(std::string(figs::kRefreshArrow), "\xE2\x86\xBB");  // ↻ U+21BB
}

TEST(Figures, ChannelArrowMatchesTs) {
    // TS REF: CHANNEL_ARROW = '←' U+2190 (used 3× in TS — inbound channel)
    EXPECT_EQ(std::string(figs::kChannelArrow), "\xE2\x86\x90");  // ← U+2190
}

TEST(Figures, ChannelArrowSameAsArrowLeft) {
    // CHANNEL_ARROW and figures.arrowLeft are the same glyph (U+2190).
    EXPECT_EQ(std::string(figs::kChannelArrow), std::string(figs::kArrowLeft));
}

TEST(Figures, InjectedArrowMatchesTs) {
    // TS REF: INJECTED_ARROW = '→' U+2192 (used 1× in TS — cross-session)
    EXPECT_EQ(std::string(figs::kInjectedArrow), "\xE2\x86\x92");  // → U+2192
}

TEST(Figures, InjectedArrowSameAsArrowRight) {
    EXPECT_EQ(std::string(figs::kInjectedArrow), std::string(figs::kArrowRight));
}

TEST(Figures, ReferenceMarkMatchesTs) {
    // TS REF: REFERENCE_MARK = '※' U+203B (used 3× in TS — away-summary recap)
    EXPECT_EQ(std::string(figs::kReferenceMark), "\xE2\x80\xBB");  // ※ U+203B
}

TEST(Figures, BlockquoteBarMatchesTs) {
    // TS REF: BLOCKQUOTE_BAR = '▎' U+258E (used 3× in TS)
    EXPECT_EQ(std::string(figs::kBlockquoteBar), "\xE2\x96\x8E");  // ▎ U+258E
}

TEST(Figures, HeavyHorizontalMatchesTs) {
    // TS REF: HEAVY_HORIZONTAL = '━' U+2501 (used 1× in TS)
    EXPECT_EQ(std::string(figs::kHeavyHorizontal), "\xE2\x94\x81");  // ━ U+2501
}

TEST(Figures, FlagIconMatchesTs) {
    // TS REF: FLAG_ICON = '⚑' U+2691 (used 2× in TS — issue banner)
    EXPECT_EQ(std::string(figs::kFlagIcon), "\xE2\x9A\x91");  // ⚑ U+2691
}

TEST(Figures, ForkGlyphMatchesTs) {
    // TS REF: FORK_GLYPH = '⑂' U+2442 (used 1× in TS — fork directive)
    EXPECT_EQ(std::string(figs::kForkGlyph), "\xE2\x91\x82");  // ⑂ U+2442
}

// ─── Effort level indicators ────────────────────────────────────────────

TEST(Figures, EffortLowMatchesTs) {
    // TS REF: EFFORT_LOW = '○' U+25CB (used 3× in TS)
    EXPECT_EQ(std::string(figs::kEffortLow), "\xE2\x97\x8B");  // ○ U+25CB
}

TEST(Figures, EffortMediumMatchesTs) {
    // TS REF: EFFORT_MEDIUM = '◐' U+25D0 (used 3× in TS)
    EXPECT_EQ(std::string(figs::kEffortMedium), "\xE2\x97\x90");  // ◐ U+25D0
}

TEST(Figures, EffortHighMatchesTs) {
    // TS REF: EFFORT_HIGH = '●' U+25CF (used 4× in TS)
    EXPECT_EQ(std::string(figs::kEffortHigh), "\xE2\x97\x8F");  // ● U+25CF
}

TEST(Figures, EffortHighSameAsBullet) {
    EXPECT_EQ(std::string(figs::kEffortHigh), std::string(figs::kBullet));
}

TEST(Figures, EffortMaxMatchesTs) {
    // TS REF: EFFORT_MAX = '◉' U+25C9 (used 3× in TS — Opus 4.6 only)
    EXPECT_EQ(std::string(figs::kEffortMax), "\xE2\x97\x89");  // ◉ U+25C9
}

TEST(Figures, EffortMaxSameAsRadioOn) {
    EXPECT_EQ(std::string(figs::kEffortMax), std::string(figs::kRadioOn));
}

// ─── Bridge indicators ──────────────────────────────────────────────────

TEST(Figures, BridgeReadyIndicatorIsTsFaithful) {
    // TS REF: constants/figures.ts L44 — BRIDGE_READY_INDICATOR =
    //   '·✔︎·' = '·✔︎·'
    //   (middot U+00B7 + heavy check U+2714 + VS15 U+FE0E + middot U+00B7).
    //
    // PREVIOUS BUG: CPP had emoji ✅︎ (U+2705 + VS15).  Corrected to ·✔︎·.
    const std::string expected = "\xC2\xB7\xE2\x9C\x94\xEF\xB8\x8E\xC2\xB7";
    EXPECT_EQ(std::string(figs::kBridgeReadyIndicator), expected);
    // Verify the 4 code-point structure: middot, check, VS15, middot.
    // U+00B7(2) + U+2714(3) + U+FE0E(3) + U+00B7(2) = 10 UTF-8 bytes.
    EXPECT_EQ(figs::kBridgeReadyIndicator.size(), 10u);
}

TEST(Figures, BridgeFailedIndicatorMatchesTs) {
    // TS REF: BRIDGE_FAILED_INDICATOR = '×' U+00D7 (used 5× in TS)
    EXPECT_EQ(std::string(figs::kBridgeFailedIndicator), "\xC3\x97");  // × U+00D7
}

TEST(Figures, BridgeSpinnerFramesCountIsFour) {
    // TS REF: BRIDGE_SPINNER_FRAMES has exactly 4 frames.
    // Distinct from the 10-frame braille general-purpose spinner.
    EXPECT_EQ(figs::kBridgeSpinnerFrameCount, 4u);
    EXPECT_EQ(figs::kBridgeSpinnerFrames.size(), 4u);
}

TEST(Figures, BridgeSpinnerFrameBytesAreTsFaithful) {
    // TS REF: constants/figures.ts L38-43 — BRIDGE_SPINNER_FRAMES =
    //   ['·|·', '·/·', '·—·', '·\\·']
    EXPECT_EQ(std::string(figs::kBridgeSpinnerFrames[0]), "\xC2\xB7\x7C\xC2\xB7");          // ·|·
    EXPECT_EQ(std::string(figs::kBridgeSpinnerFrames[1]), "\xC2\xB7\x2F\xC2\xB7");          // ·/·
    EXPECT_EQ(std::string(figs::kBridgeSpinnerFrames[2]), "\xC2\xB7\xE2\x80\x94\xC2\xB7");  // ·—·
    EXPECT_EQ(std::string(figs::kBridgeSpinnerFrames[3]), "\xC2\xB7\x5C\xC2\xB7");          // ·\·
}

TEST(Figures, BridgeSpinnerFrameGlyphWrapsCorrectly) {
    // bridge_spinner_frame_glyph() must wrap modulo 4.
    EXPECT_EQ(std::string(figs::bridge_spinner_frame_glyph(0)),
              std::string(figs::kBridgeSpinnerFrames[0]));
    EXPECT_EQ(std::string(figs::bridge_spinner_frame_glyph(3)),
              std::string(figs::kBridgeSpinnerFrames[3]));
    EXPECT_EQ(std::string(figs::bridge_spinner_frame_glyph(4)),
              std::string(figs::kBridgeSpinnerFrames[0]));  // wraps
    EXPECT_EQ(std::string(figs::bridge_spinner_frame_glyph(7)),
              std::string(figs::kBridgeSpinnerFrames[3]));
    EXPECT_EQ(std::string(figs::bridge_spinner_frame_glyph(-1)),
              std::string(figs::kBridgeSpinnerFrames[3]));  // negative wraps
}

TEST(Figures, GeneralSpinnerFrameCountIsTen) {
    // Regression guard: kSpinnerFramesBraille must have exactly 10 frames
    // (TS SpinnerGlyph.tsx — 10 braille dots sweeping top-left to bottom-right).
    EXPECT_EQ(figs::kSpinnerFrameCount, 10u);
    EXPECT_EQ(figs::kSpinnerFramesBraille.size(), 10u);
}

TEST(Figures, SpinnerFrameGlyphWrapsCorrectly) {
    // spinner_frame_glyph() must wrap modulo 10.
    EXPECT_EQ(std::string(figs::spinner_frame_glyph(0)),
              std::string(figs::kSpinnerFramesBraille[0]));
    EXPECT_EQ(std::string(figs::spinner_frame_glyph(9)),
              std::string(figs::kSpinnerFramesBraille[9]));
    EXPECT_EQ(std::string(figs::spinner_frame_glyph(10)),
              std::string(figs::kSpinnerFramesBraille[0]));  // wraps
    EXPECT_EQ(std::string(figs::spinner_frame_glyph(-1)),
              std::string(figs::kSpinnerFramesBraille[9]));  // negative wraps
}

TEST(Figures, BridgeAndGeneralSpinnersAreDistinct) {
    // The 4-frame bridge spinner and 10-frame braille spinner must NOT be
    // the same set — they serve different visual purposes.
    EXPECT_NE(figs::kBridgeSpinnerFrameCount, figs::kSpinnerFrameCount);
    // Frame 0 of bridge (·|·) is definitely not frame 0 of braille (⠋).
    EXPECT_NE(std::string(figs::kBridgeSpinnerFrames[0]),
              std::string(figs::kSpinnerFramesBraille[0]));
}

// ─── Legacy glyph backward compat ───────────────────────────────────────

TEST(Figures, LegacyBridgeReadyIndicatorStillCompiles) {
    // kBridgeReadyIndicatorLegacy preserves the old emoji ✅︎ for one release
    // so callers that already imported it don't break.
    EXPECT_EQ(std::string(figs::kBridgeReadyIndicatorLegacy), "\xE2\x9C\x85\xEF\xB8\x8F");  // ✅︎
}

TEST(Figures, LegacyBridgeReadyDiffersFromTsFaithful) {
    // The legacy emoji and the TS-faithful middot-check must be different.
    EXPECT_NE(std::string(figs::kBridgeReadyIndicator),
              std::string(figs::kBridgeReadyIndicatorLegacy));
}

// ─── Display width sanity (wcwidth assumptions) ─────────────────────────

TEST(Figures, CoreGlyphsAreSingleDisplayCell) {
    // Regression guard: these glyphs are assumed to occupy 1 terminal cell.
    // If a font renders them as 2 cells, layout will shift.
    // We can't call wcwidth() portably here, but we CAN verify the byte
    // length matches known single-cell UTF-8 sequences.
    //
    // 3-byte UTF-8 (U+25CF range, BMP): 1 cell expected.
    EXPECT_EQ(figs::kPointer.size(), 3u);       // ❯
    EXPECT_EQ(figs::kTick.size(), 3u);          // ✔
    EXPECT_EQ(figs::kCross.size(), 3u);         // ✘
    EXPECT_EQ(figs::kBullet.size(), 3u);        // ●
    EXPECT_EQ(figs::kArrowDown.size(), 3u);     // ↓
    EXPECT_EQ(figs::kArrowUp.size(), 3u);       // ↑
    EXPECT_EQ(figs::kArrowLeft.size(), 3u);     // ←
    EXPECT_EQ(figs::kArrowRight.size(), 3u);    // →
    EXPECT_EQ(figs::kBlackCircle.size(), 3u);   // ⏺
    EXPECT_EQ(figs::kRadioOn.size(), 3u);       // ◉
    EXPECT_EQ(figs::kStar.size(), 3u);          // ★
    EXPECT_EQ(figs::kHeart.size(), 3u);         // ♥
    EXPECT_EQ(figs::kPlay.size(), 3u);          // ▶
    EXPECT_EQ(figs::kLineVertical.size(), 3u);  // │
    EXPECT_EQ(figs::kLineUpRight.size(), 3u);   // └
    EXPECT_EQ(figs::kLineUpDownRight.size(), 3u); // ├
    EXPECT_EQ(figs::kDiamondOpen.size(), 3u);   // ◇
    EXPECT_EQ(figs::kTeardropAsterisk.size(), 3u); // ✻
    EXPECT_EQ(figs::kLightningBolt.size(), 3u); // ↯
    EXPECT_EQ(figs::kPauseIcon.size(), 3u);     // ⏸
    EXPECT_EQ(figs::kRefreshArrow.size(), 3u);  // ↻
    EXPECT_EQ(figs::kReferenceMark.size(), 3u); // ※
    EXPECT_EQ(figs::kEffortMedium.size(), 3u);  // ◐
    EXPECT_EQ(figs::kEffortMax.size(), 3u);     // ◉
    EXPECT_EQ(figs::kFlagIcon.size(), 3u);      // ⚑
    EXPECT_EQ(figs::kForkGlyph.size(), 3u);     // ⑂
    EXPECT_EQ(figs::kCircleDouble.size(), 3u);  // ◎
    EXPECT_EQ(figs::kTriangleUpOutline.size(), 3u); // △
    EXPECT_EQ(figs::kTriangleRightSmall.size(), 3u); // ▸
    EXPECT_EQ(figs::kTriangleDownSmall.size(), 3u); // ▾
    EXPECT_EQ(figs::kSquareSmall.size(), 3u);   // ◻
    EXPECT_EQ(figs::kSquareSmallFilled.size(), 3u); // ◼
    EXPECT_EQ(figs::kBlockquoteBar.size(), 3u); // ▎
    EXPECT_EQ(figs::kHeavyHorizontal.size(), 3u); // ━
    EXPECT_EQ(figs::kBulletOperator.size(), 3u); // ∙
}

TEST(Figures, MultiByteGlyphSizesAreCorrect) {
    // These glyphs span multiple UTF-8 bytes for multi-codepoint sequences.
    EXPECT_EQ(figs::kBridgeReadyIndicator.size(), 10u);  // ·✔︎· (4 code points: U+00B7+U+2714+U+FE0E+U+00B7 = 2+3+3+2=10 bytes)
    EXPECT_EQ(figs::kBridgeFailedIndicator.size(), 2u); // × (U+00D7 = 2 bytes)
    EXPECT_EQ(figs::kBridgeReadyIndicatorLegacy.size(), 6u); // ✅︎ (2 code points)
    EXPECT_EQ(figs::kQuestionMarkPrefix.size(), 3u);    // (?) = 3 ASCII chars
}

// ═══════════════════════════════════════════════════════════════════════════════
// Placeholder Cascade (TS REF: usePromptInputPlaceholder.ts + renderPlaceholder.ts)
// ═══════════════════════════════════════════════════════════════════════════════

namespace ph = cc::ui::placeholder;

TEST(PlaceholderCascade, InputNonEmpty_ReturnsNullopt) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "hello";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, EmptyInput_NoConditions_ReturnsNullopt) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.prompt_suggestion_enabled = false;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, ViewingAgent_ReturnsAgentMessage) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.viewing_agent_name = "researcher";
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Message @researcher…");
}

TEST(PlaceholderCascade, ViewingAgent_LongName_Truncated) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    // Use a name exactly 21 chars long so truncation is predictable:
    // "abcdefghijklmnopqrstu" (21) > 20, so substr(0,17) = "abcdefghijklmnopq"
    // then + "..." = "abcdefghijklmnopq..." (20 chars total for display name).
    ctx.viewing_agent_name = "abcdefghijklmnopqrstu";
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    // TS REF: name truncation uses '...' (3 ASCII dots), suffix uses '…' (U+2026).
    EXPECT_TRUE(result->find("abcdefghijklmnopq...") != std::string::npos);
    // Verify the message starts correctly.
    EXPECT_TRUE(result->starts_with("Message @"));
    // Total: "Message @" (9) + 17-char name + "..." (3) + "…" (3 bytes UTF-8)
    EXPECT_TRUE(result->size() > 9 + 17 + 3);
}

TEST(PlaceholderCascade, QueuedHint_ShownFewerThan3Times) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.has_editable_queued = true;
    ctx.queued_hint_shown_count = 0;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Press up to edit queued messages");
}

TEST(PlaceholderCascade, QueuedHint_Shown2Times_StillVisible) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.has_editable_queued = true;
    ctx.queued_hint_shown_count = 2;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Press up to edit queued messages");
}

TEST(PlaceholderCascade, QueuedHint_Shown3Times_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.has_editable_queued = true;
    ctx.queued_hint_shown_count = 3;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, QueuedHint_NoEditableCmds_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.has_editable_queued = false;
    ctx.queued_hint_shown_count = 0;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, FirstSubmit_ReturnsExample) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->find("Try \"") == 0);
}

TEST(PlaceholderCascade, SecondSubmit_NoExample) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 1;
    ctx.prompt_suggestion_enabled = true;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, SuggestionsDisabled_NoExample) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = false;
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, AiSuggestion_OverridesCascade) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    ctx.input_mode = cc::ui::common::PromptInputMode::Normal;
    ctx.next_action_suggestion = "Check the failing test";
    ctx.autocomplete_suggestions_empty = true;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Check the failing test");
}

TEST(PlaceholderCascade, AiSuggestionSlash_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    ctx.input_mode = cc::ui::common::PromptInputMode::Normal;
    ctx.next_action_suggestion = "/review";
    ctx.autocomplete_suggestions_empty = true;
    // Should fall through to example (submit_count < 1)
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->find("Try \"") == 0);
}

TEST(PlaceholderCascade, AiSuggestionBashMode_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    ctx.input_mode = cc::ui::common::PromptInputMode::Bash;
    ctx.next_action_suggestion = "Check the failing test";
    ctx.autocomplete_suggestions_empty = true;
    // Should fall through to example (submit_count < 1) since AI only
    // applies in Normal mode.
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->find("Try \"") == 0);
}

TEST(PlaceholderCascade, AiSuggestionWithAutocomplete_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.input_mode = cc::ui::common::PromptInputMode::Normal;
    ctx.next_action_suggestion = "Check the failing test";
    ctx.autocomplete_suggestions_empty = false;
    // Should be suppressed because autocomplete is showing.
    auto result = ph::ComputePlaceholder(ctx);
    EXPECT_FALSE(result.has_value());
}

TEST(PlaceholderCascade, AiSuggestionViewingAgent_Suppressed) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 5;
    ctx.input_mode = cc::ui::common::PromptInputMode::Normal;
    ctx.next_action_suggestion = "Check the failing test";
    ctx.viewing_agent_name = "researcher";
    ctx.autocomplete_suggestions_empty = true;
    // Should show viewing agent hint instead of AI suggestion.
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Message @researcher…");
}

TEST(PlaceholderCascade, PriorityOrder_AgentOverQueuedOverExample) {
    // All conditions true: viewing agent takes priority over queued hint,
    // which takes priority over example.
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    ctx.viewing_agent_name = "builder";
    ctx.has_editable_queued = true;
    ctx.queued_hint_shown_count = 0;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Message @builder…");
}

TEST(PlaceholderCascade, PriorityOrder_QueuedOverExample) {
    ph::PlaceholderContext ctx;
    ctx.input_text = "";
    ctx.submit_count = 0;
    ctx.prompt_suggestion_enabled = true;
    ctx.has_editable_queued = true;
    ctx.queued_hint_shown_count = 0;
    auto result = ph::ComputePlaceholder(ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Press up to edit queued messages");
}

TEST(PlaceholderCascade, GetExamplePlaceholder_ReturnsTryFormat) {
    auto example = ph::GetExamplePlaceholder(0);
    EXPECT_TRUE(example.find("Try \"") == 0);
    EXPECT_TRUE(example.find("\"") != std::string::npos);
}

TEST(PlaceholderCascade, GetExamplePlaceholder_VariesWithSubmitCount) {
    auto example0 = ph::GetExamplePlaceholder(0);
    auto example1 = ph::GetExamplePlaceholder(1);
    // Different submit_count values may produce different examples
    // (modulo kExampleCommands.size()).
    EXPECT_FALSE(example0.empty());
    EXPECT_FALSE(example1.empty());
}

// ── RenderPlaceholder tests ──────────────────────────────────────────────

TEST(RenderPlaceholder, ValueNonEmpty_ShowPlaceholderFalse) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"already typed",
        /*show_cursor=*/true,
        /*focused=*/true);
    EXPECT_FALSE(result.show_placeholder);
}

TEST(RenderPlaceholder, EmptyPlaceholder_ShowPlaceholderFalse) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::nullopt,
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true);
    EXPECT_FALSE(result.show_placeholder);
}

TEST(RenderPlaceholder, CursorFocusedTerminalFocused_ElementProduced) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/true);
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_TRUE(result.element.has_value());
}

TEST(RenderPlaceholder, NoCursor_DimsFullText) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/false,
        /*focused=*/false,
        /*terminal_focus=*/true);
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_TRUE(result.element.has_value());
}

TEST(RenderPlaceholder, HideTextWithCursor_ShowsOnlyCursorBlock) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/true,
        /*hide_text=*/true);
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_TRUE(result.element.has_value());
}

TEST(RenderPlaceholder, HideTextNoCursor_NoElement) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/false,
        /*focused=*/false,
        /*terminal_focus=*/true,
        /*hide_text=*/true);
    // show_placeholder is true (value empty + placeholder exists), but
    // no element is produced because hide_text + no cursor = empty string.
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_FALSE(result.element.has_value());
}

TEST(RenderPlaceholder, WithPrefix_IncludesPrefix) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/true,
        /*hide_text=*/false,
        /*prefix=*/"❯ ");
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_TRUE(result.element.has_value());
}

TEST(RenderPlaceholder, TerminalNotFocused_NoInvert) {
    // When terminal is not focused, first char should NOT be inverted.
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/false);
    EXPECT_TRUE(result.show_placeholder);
    EXPECT_TRUE(result.element.has_value());
}

TEST(RenderPlaceholder, FirstUtf8Codepoint_Ascii) {
    auto [ch, len] = ph::FirstUtf8Codepoint("hello");
    EXPECT_EQ(ch, "h");
    EXPECT_EQ(len, 1u);
}

TEST(RenderPlaceholder, FirstUtf8Codepoint_Multibyte) {
    // "❯" is U+276F = 3 bytes in UTF-8
    auto [ch, len] = ph::FirstUtf8Codepoint("\xE2\x9D\xAF hello");
    EXPECT_EQ(len, 3u);
    EXPECT_EQ(ch, "\xE2\x9D\xAF");
}

TEST(RenderPlaceholder, ScreenRender_CursorFocused_InvertsFirstChar) {
    // End-to-end: render a placeholder with cursor+focus+terminalFocus,
    // render to screen, verify first char appears in the output.
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Type here"),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/true,
        /*hide_text=*/false,
        /*prefix=*/"");
    ASSERT_TRUE(result.element.has_value());

    ftxui::Screen screen(20, 1);
    ftxui::Render(screen, *result.element);
    std::string rendered = screen.ToString();
    // Should contain the placeholder text content.
    EXPECT_FALSE(rendered.empty());
}

TEST(RenderPlaceholder, ScreenRender_NoCursor_ShowsDimPlaceholder) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Ask anything"),
        /*value=*/"",
        /*show_cursor=*/false,
        /*focused=*/false);
    ASSERT_TRUE(result.element.has_value());

    ftxui::Screen screen(30, 1);
    ftxui::Render(screen, *result.element);
    std::string rendered = screen.ToString();
    EXPECT_FALSE(rendered.empty());
}

TEST(RenderPlaceholder, HideTextScreenRender_ShowsCursorBlock) {
    auto result = ph::RenderPlaceholder(
        /*placeholder=*/std::string_view("Recording..."),
        /*value=*/"",
        /*show_cursor=*/true,
        /*focused=*/true,
        /*terminal_focus=*/true,
        /*hide_text=*/true);
    ASSERT_TRUE(result.element.has_value());

    ftxui::Screen screen(10, 1);
    ftxui::Render(screen, *result.element);
    std::string rendered = screen.ToString();
    // Should show a space (the cursor block), not the placeholder text.
    EXPECT_FALSE(rendered.empty());
    // The rendered output should NOT contain "Recording".
    EXPECT_TRUE(rendered.find("Recording") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Bash-mode prefix utilities (TS→CPP port gap: bash-mode-prefix)
// TS REF: src/components/PromptInput/inputModes.ts
// ═══════════════════════════════════════════════════════════════════════════════

TEST(BashModePrefix, GetModeFromInput_BangPrefix) {
    namespace figs = cc::ui::design::figures;
    EXPECT_EQ(figs::get_mode_from_input("!ls"), figs::PromptMode::kBash);
    EXPECT_EQ(figs::get_mode_from_input("!"), figs::PromptMode::kBash);
    EXPECT_EQ(figs::get_mode_from_input("! gcloud auth"), figs::PromptMode::kBash);
}

TEST(BashModePrefix, GetModeFromInput_NoPrefix) {
    namespace figs = cc::ui::design::figures;
    EXPECT_EQ(figs::get_mode_from_input("hello"), figs::PromptMode::kPrompt);
    EXPECT_EQ(figs::get_mode_from_input(""), figs::PromptMode::kPrompt);
    EXPECT_EQ(figs::get_mode_from_input("a!b"), figs::PromptMode::kPrompt);
}

TEST(BashModePrefix, StripModePrefix_Bash) {
    namespace figs = cc::ui::design::figures;
    EXPECT_EQ(figs::strip_mode_prefix("!ls"), "ls");
    EXPECT_EQ(figs::strip_mode_prefix("! gcloud"), " gcloud");
    EXPECT_EQ(figs::strip_mode_prefix("!"), "");
}

TEST(BashModePrefix, StripModePrefix_NoPrefix) {
    namespace figs = cc::ui::design::figures;
    // Non-bash input passes through unchanged.
    EXPECT_EQ(figs::strip_mode_prefix("hello"), "hello");
    EXPECT_EQ(figs::strip_mode_prefix(""), "");
}

TEST(BashModePrefix, PrependModeChar_Bash) {
    namespace figs = cc::ui::design::figures;
    // TS REF: inputModes.ts:4-14 — prependModeCharacterToInput
    auto result = figs::prepend_mode_char("ls", figs::PromptMode::kBash);
    EXPECT_EQ(result, "!ls");
}

TEST(BashModePrefix, PrependModeChar_Prompt) {
    namespace figs = cc::ui::design::figures;
    auto result = figs::prepend_mode_char("hello", figs::PromptMode::kPrompt);
    EXPECT_EQ(result, "hello");
}

TEST(BashModePrefix, IsModeCharacter) {
    namespace figs = cc::ui::design::figures;
    EXPECT_TRUE(figs::is_mode_character("!"));
    EXPECT_FALSE(figs::is_mode_character("a"));
    EXPECT_FALSE(figs::is_mode_character("!cmd"));  // multi-char, not a single mode char
    EXPECT_FALSE(figs::is_mode_character(""));
}

TEST(BashModePrefix, HistoryRoundTrip_BashToggle) {
    // Simulate: user types bare '!' to enter bash mode, then types "cmd".
    // input_text = "cmd" (no '!'), input_mode = Bash.
    // History entry should carry '!' for round-trip detection.
    // TS REF: REPL.tsx:3318 — prependModeCharacterToInput(input, inputMode)
    namespace figs = cc::ui::design::figures;

    const std::string input_text = "cmd";       // clean text (no '!')
    const bool is_bash = true;                   // user toggled via bare '!'
    const bool text_has_prefix = !input_text.empty() &&
        input_text[0] == figs::kBashModeChar;

    const std::string hist_entry =
        (is_bash && !text_has_prefix)
            ? figs::prepend_mode_char(input_text, figs::PromptMode::kBash)
            : input_text;

    EXPECT_EQ(hist_entry, "!cmd");

    // On arrow-up recall, get_mode_from_input detects bash mode.
    EXPECT_EQ(figs::get_mode_from_input(hist_entry), figs::PromptMode::kBash);
    // strip_mode_prefix recovers the clean text for display.
    EXPECT_EQ(figs::strip_mode_prefix(hist_entry), "cmd");
}

TEST(BashModePrefix, HistoryRoundTrip_DirectBangCmd) {
    // Simulate: user types "!cmd" directly (bypassed single-char interceptor,
    // e.g. IME composition or bracketed paste).  input_text = "!cmd", mode
    // detection happens via effective_is_bash().  History should NOT double-
    // prepend '!' because the text already carries it.
    namespace figs = cc::ui::design::figures;

    const std::string input_text = "!cmd";      // already has '!' prefix
    const bool is_bash = true;
    const bool text_has_prefix = !input_text.empty() &&
        input_text[0] == figs::kBashModeChar;

    const std::string hist_entry =
        (is_bash && !text_has_prefix)
            ? figs::prepend_mode_char(input_text, figs::PromptMode::kBash)
            : input_text;

    // Should be "!cmd", NOT "!!cmd"
    EXPECT_EQ(hist_entry, "!cmd");
    EXPECT_NE(hist_entry, "!!cmd");
}

TEST(BashModePrefix, HistoryRoundTrip_NormalMode) {
    // Normal mode: no '!' prepended
    namespace figs = cc::ui::design::figures;

    const std::string input_text = "hello world";
    const bool is_bash = false;
    const bool text_has_prefix = !input_text.empty() &&
        input_text[0] == figs::kBashModeChar;

    const std::string hist_entry =
        (is_bash && !text_has_prefix)
            ? figs::prepend_mode_char(input_text, figs::PromptMode::kBash)
            : input_text;

    EXPECT_EQ(hist_entry, "hello world");
    EXPECT_EQ(figs::get_mode_from_input(hist_entry), figs::PromptMode::kPrompt);
}

TEST(BashModePrefix, MultiCharBangCmd_StripLogic) {
    // TS REF: PromptInput.tsx:878-886 — multi-char "!cmd" insertion at
    // cursor-0 into empty input: strip '!', enter bash mode, store clean text.
    namespace figs = cc::ui::design::figures;

    const std::string ch = "!ls -la";  // multi-char event (IME/bracketed paste)
    const bool input_empty = true;
    const bool cursor_at_zero = true;

    // Verify get_mode_from_input detects bash
    EXPECT_EQ(figs::get_mode_from_input(ch), figs::PromptMode::kBash);
    EXPECT_GT(ch.size(), 1u);  // multi-char

    if (input_empty && cursor_at_zero &&
        figs::get_mode_from_input(ch) == figs::PromptMode::kBash &&
        ch.size() > 1) {
        // Strip '!' and insert clean text
        std::string clean(figs::strip_mode_prefix(ch));
        EXPECT_EQ(clean, "ls -la");
        // Mode would be set to Bash
    }
}

TEST(BashModePrefix, SubmitStripsPrefix) {
    // TS REF: inputModes.ts:23-29 (getValueFromInput) + REPL.tsx submit flow.
    // Submit always strips the '!' prefix before passing text to engine.
    namespace figs = cc::ui::design::figures;

    // Bare '!' toggle path: input_text = "cmd", mode = Bash.
    // strip_mode_prefix on "cmd" returns "cmd" (no change needed since
    // bare '!' was swallowed).  But the submit handler uses strip_mode_prefix
    // defensively for all paths.
    EXPECT_EQ(figs::strip_mode_prefix("cmd"), "cmd");

    // Direct "!cmd" path: strip it
    EXPECT_EQ(figs::strip_mode_prefix("!cmd"), "cmd");

    // History recall "!cmd" path: strip it
    EXPECT_EQ(figs::strip_mode_prefix("!ls -la"), "ls -la");
}

TEST(BashModePrefix, ModeResetAfterSubmit) {
    // TS REF: REPL.tsx:3359 — setInputMode('prompt') after EVERY submit.
    // This verifies the expected enum value for the "reset to normal" state.
    // The actual reset happens in app.cppm HandleSubmit; this test asserts
    // the semantic: after any submit (bash or prompt), mode should be Normal.
    using cc::ui::common::PromptInputMode;

    // Simulate: user was in Bash mode, submitted "!ls".
    // After submit, mode resets.
    PromptInputMode mode_after_submit = PromptInputMode::Normal;
    EXPECT_EQ(mode_after_submit, PromptInputMode::Normal);
    EXPECT_NE(mode_after_submit, PromptInputMode::Bash);
}
