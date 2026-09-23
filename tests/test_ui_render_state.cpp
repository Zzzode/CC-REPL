/// @file test_ui_runtime.cpp
/// @brief Split from test_ui.cpp - AppRuntime, E2E_Gate, FullscreenLayout, LogoV2, PromptInput, ReplScreen (SLOC budget fix)

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

import cc.ui.screens.repl_screen;
import cc.ui.prompt.prompt_input;
import cc.ui.prompt.prompt_input_footer;
import cc.ui.prompt.voice_indicator;
import cc.ui.foundation.logo_v2;
import cc.ui.chrome.fullscreen_layout;
import cc.ui.chrome.panels;
import cc.ui.messages.message_image;
import cc.ui.messages.virtual_list;
import cc.constants.constants;
import cc.ui.foundation.design_tokens;
import cc.ui.foundation.design_figures;
import cc.ui.foundation.theme_provider;
import cc.ui.widgets.components;
import cc.ui.widgets.all_components;
import cc.ui.messages.messages;
import cc.ui.messages.message_pipeline;
import cc.ui.messages.messages_list;
import cc.ui.messages.message_row;
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.foundation.declared_cursor;
import cc.ui.prompt.autocomplete_sources;
import cc.ui.features.teams.live_teammates;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.chrome.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════




TEST(ReplScreen, SubmitsUtf8PromptOnReturn) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    std::optional<std::string> submitted;

    repl::ReplScreenCallbacks callbacks;
    callbacks.on_submit = [&](const std::string& text, repl::InputMode mode) {
        submitted = text;
        EXPECT_EQ(mode, repl::InputMode::Normal);
    };

    auto component = repl::ReplScreen(state, std::move(callbacks));
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("你")));
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("好")));
    EXPECT_EQ(state->input_text, "你好");

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Return));
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(*submitted, "你好");
    EXPECT_TRUE(state->input_text.empty());
}



TEST(ReplScreen, TabAcceptsSelectedSlashSuggestion) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->input_text = "/a";
    state->autocomplete_suggestions = {
        {.display_text = "/add-dir", .description = "Add a working directory",
         .insert_text = "/add-dir ", .replacement_start = 0, .replacement_end = 2,
         .submit_on_return = true},
        {.display_text = "/agents", .description = "Manage agent configurations",
         .insert_text = "/agents ", .replacement_start = 0, .replacement_end = 2,
         .submit_on_return = true},
    };
    state->autocomplete_index = 1;

    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Tab));

    EXPECT_EQ(state->input_text, "/agents ");
    EXPECT_TRUE(state->autocomplete_suggestions.empty());
    EXPECT_EQ(state->autocomplete_index, -1);
}



TEST(ReplScreen, ReturnSubmitsSelectedSlashSuggestion) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->input_text = "/a";
    state->autocomplete_suggestions = {
        {.display_text = "/add-dir", .description = "Add a working directory",
         .insert_text = "/add-dir ", .replacement_start = 0, .replacement_end = 2,
         .submit_on_return = true},
        {.display_text = "/agents", .description = "Manage agent configurations",
         .insert_text = "/agents ", .replacement_start = 0, .replacement_end = 2,
         .submit_on_return = true},
    };
    state->autocomplete_index = 1;

    std::optional<std::string> submitted;
    repl::ReplScreenCallbacks callbacks;
    callbacks.on_submit = [&](const std::string& text, repl::InputMode mode) {
        submitted = text;
        EXPECT_EQ(mode, repl::InputMode::Normal);
    };

    auto component = repl::ReplScreen(state, std::move(callbacks));
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Return));

    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(*submitted, "/agents ");
    EXPECT_TRUE(state->input_text.empty());
    EXPECT_TRUE(state->autocomplete_suggestions.empty());
    EXPECT_EQ(state->autocomplete_index, -1);
}



TEST(ReplScreen, CtrlNCtrlPNavigateAutocompleteWithWrapping) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";
    state->input_text = "/a";
    state->autocomplete_suggestions = {
        {.display_text = "/alpha", .description = "alpha",
         .insert_text = "/alpha",
         .replacement_start = 0, .replacement_end = 2},
        {.display_text = "/bravo", .description = "bravo",
         .insert_text = "/bravo",
         .replacement_start = 0, .replacement_end = 2},
        {.display_text = "/charlie", .description = "charlie",
         .insert_text = "/charlie",
         .replacement_start = 0, .replacement_end = 2},
    };
    state->autocomplete_index = -1;

    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});

    // Ctrl+P (\x10) from -1 wraps to the last suggestion (length-1 = 2),
    // mirroring handleAutocompletePrevious (useTypeahead.tsx:1242-1247).
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x10")));
    EXPECT_EQ(state->autocomplete_index, 2);
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x10")));
    EXPECT_EQ(state->autocomplete_index, 1);
    // Ctrl+N (\x0e) moves forward.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x0e")));
    EXPECT_EQ(state->autocomplete_index, 2);
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x0e")));
    EXPECT_EQ(state->autocomplete_index, 0)
        << "Ctrl+N wraps from last suggestion back to 0";
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x0e")));
    EXPECT_EQ(state->autocomplete_index, 1);

    // With zero suggestions Ctrl+N/Ctrl+P fall through unhandled and must
    // not touch input_text (TS useTypeahead.tsx:1341 early return).
    auto empty = std::make_shared<repl::ReplScreenState>();
    empty->input_text = "keep me";
    auto empty_component =
        repl::ReplScreen(empty, repl::ReplScreenCallbacks{});
    EXPECT_FALSE(empty_component->OnEvent(ftxui::Event::Character("\x0e")));
    EXPECT_FALSE(empty_component->OnEvent(ftxui::Event::Character("\x10")));
    EXPECT_EQ(empty->input_text, "keep me");
}



TEST(ReplScreen, EscapeDoublePressOnWhitespaceOnlyClearsWithoutHistory) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";

    std::string saved = "untouched";
    repl::ReplScreenCallbacks callbacks;
    callbacks.on_save_to_history = [&](const std::string& text) { saved = text; };
    auto component = repl::ReplScreen(state, std::move(callbacks));

    for (char c : std::string("   ")) {
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character(c)));
    }
    ASSERT_EQ(state->input_text, "   ");

    // TS addToHistory guard (useTextInput.ts:145): whitespace-only input is
    // cleared on double-Esc but NOT appended to history.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_TRUE(state->input_text.empty());
    EXPECT_EQ(saved, "untouched")
        << "whitespace-only text must not be saved to history";
}



TEST(ReplScreen, EscapeDismissesPopupThenArmsThenClears) {
    namespace repl = cc::ui::repl_screen;
    namespace pif = cc::ui::prompt::footer;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";
    state->input_text = "query";
    state->autocomplete_suggestions = {
        {.display_text = "query one", .description = "one",
         .insert_text = "query one"},
        {.display_text = "query two", .description = "two",
         .insert_text = "query two"},
    };

    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});

    // TS PromptInput.tsx disableEscapeDoublePress = suggestions.length>0:
    // Esc #1 dismisses the popup without arming.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_TRUE(state->autocomplete_suggestions.empty());
    EXPECT_EQ(state->autocomplete_index, -1);
    EXPECT_EQ(state->input_text, "query");
    // Esc #2 arms the clear hint.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_EQ(state->input_text, "query");
    // Esc #3 clears.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_TRUE(state->input_text.empty());

    // With empty input and no popup, a single Esc enqueues no hint.
    auto idle = std::make_shared<repl::ReplScreenState>();
    auto idle_component = repl::ReplScreen(idle, repl::ReplScreenCallbacks{});
    (void)idle_component->OnEvent(ftxui::Event::Escape);
    bool has_escape_hint = false;
    if (idle->footer_notification_queue.current) {
        has_escape_hint =
            idle->footer_notification_queue.current->key ==
            "escape-again-to-clear";
    }
    EXPECT_FALSE(has_escape_hint);
    (void)pif::NotificationPriority::Immediate;
}



// Bug: "输入感叹号之后就没法退出这个 bash mode" — after typing '!' to enter bash
// mode (empty input), Backspace/Escape/Delete/Ctrl+U at cursor position 0 must
// exit back to Prompt mode.
// TS REF: src/components/PromptInput/PromptInput.tsx:1904-1908 —
//   `if (cursorOffset === 0 && (key.escape || key.backspace || key.delete ||
//        (key.ctrl && char === 'u'))) { onModeChange('prompt'); }`
TEST(ReplScreen, BashModeExitsOnBackspaceAtStart) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});

    // Type '!' into empty input → swallowed, flips to Bash mode (TS parity).
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("!")));
    EXPECT_EQ(state->input_mode, repl::InputMode::Bash);
    EXPECT_TRUE(state->input_text.empty());  // '!' is a mode trigger, not stored

    // Backspace at cursor 0 must exit bash mode back to Prompt.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Backspace));
    EXPECT_EQ(state->input_mode, repl::InputMode::Normal);
}



TEST(ReplScreen, BashModeExitsOnEscapeAndDeleteAndCtrlUAtStart) {
    namespace repl = cc::ui::repl_screen;

    // Escape exits bash mode.
    {
        auto state = std::make_shared<repl::ReplScreenState>();
        auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("!")));
        ASSERT_EQ(state->input_mode, repl::InputMode::Bash);
        component->OnEvent(ftxui::Event::Escape);
        EXPECT_EQ(state->input_mode, repl::InputMode::Normal);
    }
    // Delete exits bash mode.
    {
        auto state = std::make_shared<repl::ReplScreenState>();
        auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("!")));
        ASSERT_EQ(state->input_mode, repl::InputMode::Bash);
        component->OnEvent(ftxui::Event::Delete);
        EXPECT_EQ(state->input_mode, repl::InputMode::Normal);
    }
    // Ctrl+U (\x15) exits bash mode.
    {
        auto state = std::make_shared<repl::ReplScreenState>();
        auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("!")));
        ASSERT_EQ(state->input_mode, repl::InputMode::Bash);
        component->OnEvent(ftxui::Event::Character("\x15"));
        EXPECT_EQ(state->input_mode, repl::InputMode::Normal);
    }
}



TEST(ReplScreen, BashModeBackspaceMidTextDoesNotExitMode) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});

    // Enter bash mode, then type a command so cursor is NOT at 0.
    ASSERT_TRUE(component->OnEvent(ftxui::Event::Character("!")));
    ASSERT_EQ(state->input_mode, repl::InputMode::Bash);
    component->OnEvent(ftxui::Event::Character("l"));
    component->OnEvent(ftxui::Event::Character("s"));
    ASSERT_EQ(state->input_text, "ls");

    // Backspace mid-text deletes a char and stays in bash mode (cursor != 0).
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Backspace));
    EXPECT_EQ(state->input_text, "l");
    EXPECT_EQ(state->input_mode, repl::InputMode::Bash);
}



TEST(ReplScreen, MouseWheelScrollsTranscript) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->viewport_height_lines = 8;

    repl::MessageDisplayEntry message;
    message.is_local_command_output = true;
    for (int i = 0; i < 40; ++i) {
        message.content_preview += std::format("line-{:02}", i);
        if (i != 39) message.content_preview += '\n';
    }
    state->messages.push_back(std::move(message));

    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
    ftxui::Mouse wheel;
    wheel.button = ftxui::Mouse::WheelDown;

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Mouse("", wheel)));
    EXPECT_GT(state->scroll_offset, 0);
    EXPECT_FALSE(state->scroll_pinned_to_bottom);

    wheel.button = ftxui::Mouse::WheelUp;
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Mouse("", wheel)));
    EXPECT_EQ(state->scroll_offset, 0);
}
