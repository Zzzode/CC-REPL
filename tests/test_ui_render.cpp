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
import cc.ui.screens.repl_screen;

namespace {
namespace fs = std::filesystem;
}


// ═══════════════════════════════════════════════════════════════════════════════
// cc.ui.chrome.terminal: FTXUI terminal controller and common widgets
// ═══════════════════════════════════════════════════════════════════════════════


TEST(ReplScreen, CustomStatusLineSuppressesDefaultHintAndNativeStatusBar) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.status_line_enabled = true;
    state.status_line_command = ":";
    state.status_line_text = "custom status";
    state.status_bar.model_name = "native-status-model";
    state.status_bar.cost_usd = 0.1234;

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state),
        120,
        30));

    EXPECT_NE(rendered.find("custom status"), std::string::npos);
    EXPECT_EQ(rendered.find("? for shortcuts"), std::string::npos);
    EXPECT_EQ(rendered.find("native-status-model"), std::string::npos);
    EXPECT_EQ(rendered.find("$0.1234"), std::string::npos);
}


TEST(ReplScreen, CustomStatusLineOnlyRendersInPromptMode) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.input_mode = repl::InputMode::SlashCommand;
    state.status_line_enabled = true;
    state.status_line_command = ":";
    state.status_line_text = "custom status";

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state),
        120,
        30));

    EXPECT_EQ(rendered.find("custom status"), std::string::npos);
}


TEST(ReplScreen, PastingIndicatorShowsForBatchAndNotSingleKeystroke) {
    namespace repl = cc::ui::repl_screen;

    // A terminal paste arrives as one multi-char event; a single keystroke
    // (incl. a 3-byte CJK char) must not trigger the hint.
    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/tmp/cpp_migration";

    EXPECT_FALSE(state.pasting_since.has_value());

    // Simulate a paste batch by stamping as the CatchEvent handler does,
    // then render: the footer must contain "Pasting text…".
    state.pasting_since = std::chrono::steady_clock::now();
    auto pasting = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_NE(pasting.find("Pasting text"), std::string::npos);

    // After the 100ms window the hint disappears (event-driven re-render).
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto settled = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_EQ(settled.find("Pasting text"), std::string::npos);
    EXPECT_FALSE(state.pasting_since.has_value())
        << "stale timestamp should be cleared on render";
}


TEST(ReplScreen, CtrlLRedrawsWithoutMutatingInput) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";
    state->input_text = "hello";
    state->input_cursor = 5;
    state->autocomplete_suggestions = {
        {.display_text = "/help", .description = "help",
         .insert_text = "/help "},
        {.display_text = "/hi", .description = "hi",
         .insert_text = "/hi "},
    };
    state->autocomplete_index = 1;

    bool redraw = false;
    repl::ReplScreenCallbacks callbacks;
    callbacks.on_redraw = [&] { redraw = true; };
    auto component = repl::ReplScreen(state, std::move(callbacks));

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Character("\x0C")));
    EXPECT_TRUE(redraw) << "Ctrl+L must invoke the on_redraw callback";
    EXPECT_EQ(state->input_text, "hello")
        << "redraw must never clear the input";
    EXPECT_EQ(state->input_cursor, 5u);
    EXPECT_EQ(state->autocomplete_suggestions.size(), 2u);
    EXPECT_EQ(state->autocomplete_index, 1);

    const auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_NE(rendered.find("hello"), std::string::npos);

    // Global redraw works even while a tool-permission panel is open
    // (defaultBindings.ts:42 global context); the panel must not swallow it.
    auto pstate = std::make_shared<repl::ReplScreenState>();
    pstate->app_version = "9.9.9-test";
    pstate->model_display_name = "GLM-5.2";
    pstate->cwd = "/tmp/cpp_migration";
    pstate->mode = repl::ReplMode::ToolPermission;
    repl::PermissionRequestInfo pinfo;
    pinfo.tool_name = "Bash";
    pinfo.description = "rm -rf";
    pstate->permission_request = pinfo;
    bool panel_redraw = false;
    repl::ReplScreenCallbacks pcbs;
    pcbs.on_redraw = [&] { panel_redraw = true; };
    auto pcomp = repl::ReplScreen(pstate, std::move(pcbs));
    EXPECT_TRUE(pcomp->OnEvent(ftxui::Event::Character("\x0C")));
    EXPECT_TRUE(panel_redraw)
        << "Ctrl+L must redraw even over an open permission panel";
}


TEST(ReplScreen, EscapeDoublePressClearsInputAndSavesToHistory) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";

    std::string saved;
    repl::ReplScreenCallbacks callbacks;
    callbacks.on_save_to_history = [&](const std::string& text) {
        saved = text;
    };
    auto component = repl::ReplScreen(state, std::move(callbacks));

    for (char c : std::string("discard me")) {
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character(c)));
    }
    ASSERT_EQ(state->input_text, "discard me");

    // First Esc arms: text retained, "Esc again to clear" hint shown.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_EQ(state->input_text, "discard me");
    const auto armed = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_NE(armed.find("Esc again to clear"), std::string::npos);
    EXPECT_TRUE(saved.empty());

    // Second immediate Esc: persist then clear.
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_TRUE(state->input_text.empty());
    EXPECT_EQ(state->input_cursor, std::string::npos);
    EXPECT_EQ(state->history_index, std::string::npos);
    EXPECT_EQ(saved, "discard me");
    const auto cleared = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_EQ(cleared.find("Esc again to clear"), std::string::npos);
}


TEST(ReplScreen, EscapeDoublePressExpiresAfterWindowAndRearms) {
    namespace repl = cc::ui::repl_screen;

    auto state = std::make_shared<repl::ReplScreenState>();
    state->app_version = "9.9.9-test";
    state->model_display_name = "GLM-5.2";
    state->cwd = "/tmp/cpp_migration";

    auto component = repl::ReplScreen(state, repl::ReplScreenCallbacks{});
    for (char c : std::string("persistent text")) {
        ASSERT_TRUE(component->OnEvent(ftxui::Event::Character(c)));
    }

    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_EQ(state->input_text, "persistent text");
    const auto armed = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_NE(armed.find("Esc again to clear"), std::string::npos);

    // Past the 800ms double-press window the second Esc re-arms instead
    // of clearing (TS useDoublePress DOUBLE_PRESS_TIMEOUT_MS = 800).
    std::this_thread::sleep_for(std::chrono::milliseconds(850));
    EXPECT_TRUE(component->OnEvent(ftxui::Event::Escape));
    EXPECT_EQ(state->input_text, "persistent text")
        << "an expired first press must not clear on the next Esc";
    const auto rearmed = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_NE(rearmed.find("Esc again to clear"), std::string::npos)
        << "re-arming must show the hint with a fresh 1000ms timeout";

    // The notification itself expires after its 1000ms timeout
    // (event-driven QueueAdvance on render, no ticker).
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    const auto expired = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(*state), 120, 30));
    EXPECT_EQ(expired.find("Esc again to clear"), std::string::npos);
}


TEST(ReplScreen, BridgeStatusPillReflectsProjectionState) {
    namespace repl = cc::ui::repl_screen;

    // TS REF: PromptInputFooter.tsx BridgeStatusIndicator +
    // bridgeStatusUtil.ts:124 getBridgeStatus.
    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/tmp/cpp_migration";

    // Explicit remote, connected → "Remote Control" visible.
    state.bridge_enabled = true;
    state.bridge_explicit_remote = true;
    state.bridge_connected = true;
    auto connected = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_NE(connected.find("Remote Control"), std::string::npos)
        << "explicit connected bridge should show the status pill";

    // Reconnecting takes priority and is visible even for implicit remote.
    state.bridge_connected = false;
    state.bridge_explicit_remote = false;
    state.bridge_reconnecting = true;
    auto reconnecting = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_NE(reconnecting.find("Remote Control reconnecting"),
              std::string::npos)
        << "implicit remote still surfaces the reconnecting state";

    // Implicit remote that is merely connected/disconnected is hidden by
    // RenderBridgeStatus (matches the TS !explicit gate).
    state.bridge_reconnecting = false;
    auto hidden = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_EQ(hidden.find("Remote Control"), std::string::npos)
        << "non-reconnecting implicit remote must not show the pill";

    // Disabled bridge renders no pill at all.
    state.bridge_enabled = false;
    state.bridge_explicit_remote = true;
    auto disabled = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state), 120, 30));
    EXPECT_EQ(disabled.find("Remote Control"), std::string::npos);
}


TEST(ReplScreen, FreshScreenDoesNotRenderLegacyEmptyState) {
    namespace repl = cc::ui::repl_screen;

    repl::ReplScreenState state;
    state.app_version = "9.9.9-test";
    state.model_display_name = "GLM-5.2";
    state.cwd = "/tmp/cpp_migration";

    auto rendered = strip_ansi(render_to_plain_text(
        repl::RenderReplScreen(state),
        120,
        32));

    EXPECT_NE(rendered.find("Loom"), std::string::npos);
    EXPECT_EQ(rendered.find("Type a message to begin."), std::string::npos);
    EXPECT_EQ(rendered.find("/help    -- list commands"), std::string::npos);
    EXPECT_EQ(rendered.find("/model   -- change model"), std::string::npos);
    EXPECT_EQ(rendered.find("/config  -- open settings"), std::string::npos);

    // Regression guard (P0 layout): no blank row between the welcome header
    // and the prompt.  TS LogoV2 has no trailing padding; the header slot
    // height must stay dynamic.  Previously `size(HEIGHT, EQUAL, 4)` padded
    // a 3-row condensed logo up to 4, leaving a visible blank line above the
    // prompt input (the user-reported "blank line below logo").
    {
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (pos <= rendered.size()) {
            const auto nl = rendered.find('\n', pos);
            lines.emplace_back(rendered.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        const auto is_blank = [](const std::string& l) {
            return l.find_first_not_of(' ') == std::string::npos;
        };
        const auto header_it = std::find_if(lines.begin(), lines.end(),
            [](const std::string& l) { return l.find("Loom") != std::string::npos; });
        const auto prompt_it = std::find_if(lines.begin(), lines.end(),
            [](const std::string& l) { return l.find("\xE2\x9D\xAF") != std::string::npos; });  // ❯ glyph
        ASSERT_NE(header_it, lines.end());
        ASSERT_NE(prompt_it, lines.end());
        ASSERT_LT(header_it, prompt_it);
        for (auto it = header_it + 1; it < prompt_it; ++it) {
            EXPECT_FALSE(is_blank(*it))
                << "blank row between welcome header and prompt at line "
                << std::distance(lines.begin(), it);
        }
    }
}
