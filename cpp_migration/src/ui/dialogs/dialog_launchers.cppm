/// @file dialog_launchers.cppm
/// @brief Standalone dialog launcher functions — faithful port of TS
///        dialogLaunchers.tsx + showSetupDialog pattern.
///
/// MODULE:   cc.ui.dialogs.launchers
/// LICENCE:  Exported.  Imported by app/engine code that needs to show
///           a full-screen modal dialog and wait for a result.
///
/// TS REFERENCE:
///   src/utils/dialogLaunchers.tsx
///   src/components/setup/showSetupDialog.tsx
///
/// PATTERN:
///   Each launcher takes (context, props) and returns a result value.
///   They run their own FTXUI ScreenInteractive loop and block until
///   the user dismisses the dialog.
///
///   For non-blocking / REPL-integrated dialogs, use DialogQueue instead
///   (push into the queue, let the REPL render loop handle it).
module;

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.dialogs.launchers;

import cc.ui.dialogs.system;
import cc.ui.design.theme;

export namespace cc::ui::dialogs::launchers {

using namespace ftxui;
using Theme = cc::ui::design::theme::Theme;
namespace dsys = cc::ui::dialogs::system;

// ============================================================
// show_dialog_standalone — generic runner
// ============================================================

/// Run a standalone dialog component and return its result.
/// Blocks the calling thread until the dialog is dismissed.
///
/// Mirrors TS `showDialog<T>(root, renderer) -> Promise<T>`.
template <typename TResult>
[[nodiscard]] TResult show_dialog_standalone(
    ScreenInteractive& screen,
    std::function<Component(std::function<void(TResult)>)> builder)
{
    TResult result{};

    auto done_fn = [&](TResult r) {
        result = std::move(r);
        screen.ExitLoopClosure()();
    };

    auto component = builder(done_fn);
    screen.Loop(component);

    return result;
}

/// Simple yes/no dialog result.
enum class SimpleDialogResult : std::uint8_t {
    Yes,
    No,
    Cancel,
};

/// Show a simple confirmation dialog (yes / no / cancel).
/// Blocks until user makes a choice.
[[nodiscard]] inline SimpleDialogResult show_confirm_dialog(
    ScreenInteractive& screen,
    std::string_view title,
    std::string_view message,
    std::string_view yes_label = "Yes",
    std::string_view no_label = "No",
    bool show_cancel = true)
{
    return show_dialog_standalone<SimpleDialogResult>(
        screen,
        [&](std::function<void(SimpleDialogResult)> done) -> Component {
            auto state = std::make_shared<int>(0); // 0=Yes, 1=No, 2=Cancel

            auto render = [=]() -> Element {
                Elements buttons;
                auto btn_yes = hbox({
                    text(" "),
                    text(std::string{yes_label}) | bold,
                    text(" "),
                }) | borderStyled(Color::Green) | color(Color::Green);
                if (*state == 0) btn_yes = btn_yes | inverted;
                buttons.push_back(btn_yes);
                buttons.push_back(text("  "));

                auto btn_no = hbox({
                    text(" "),
                    text(std::string{no_label}),
                    text(" "),
                }) | borderStyled(Color::Red) | color(Color::Red);
                if (*state == 1) btn_no = btn_no | inverted;
                buttons.push_back(btn_no);

                if (show_cancel) {
                    buttons.push_back(text("  "));
                    auto btn_cancel = hbox({
                        text(" "),
                        text("Cancel") | dim,
                        text(" "),
                    }) | borderStyled(Color::GrayDark);
                    if (*state == 2) btn_cancel = btn_cancel | inverted;
                    buttons.push_back(btn_cancel);
                }

                return window(
                    text(" " + std::string{title} + " ") | bold,
                    vbox({
                        text(std::string{message}) | center,
                        text(""),
                        hbox(buttons) | center,
                        text(""),
                        text("Enter = select, Tab/Arrow = move, Esc = cancel") | dim | center,
                    }) | size(WIDTH, GREATER_THAN, 40)
                ) | color(Color::Cyan);
            };

            return Renderer(render) | CatchEvent([=](Event event) -> bool {
                int max_buttons = show_cancel ? 3 : 2;

                if (event == Event::ArrowLeft || event == Event::TabReverse) {
                    *state = (*state - 1 + max_buttons) % max_buttons;
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Tab) {
                    *state = (*state + 1) % max_buttons;
                    return true;
                }
                if (event == Event::Return) {
                    switch (*state) {
                        case 0: done(SimpleDialogResult::Yes); return true;
                        case 1: done(SimpleDialogResult::No); return true;
                        case 2: done(SimpleDialogResult::Cancel); return true;
                    }
                    return true;
                }
                if (event == Event::Escape) {
                    done(SimpleDialogResult::Cancel);
                    return true;
                }
                if (event == Event::Character('y') || event == Event::Character('Y')) {
                    done(SimpleDialogResult::Yes);
                    return true;
                }
                if (event == Event::Character('n') || event == Event::Character('N')) {
                    done(SimpleDialogResult::No);
                    return true;
                }
                return false;
            });
        }
    );
}

// ============================================================
// show_info_dialog — simple informational message
// ============================================================

/// Show a simple informational dialog with an OK button.
inline void show_info_dialog(
    ScreenInteractive& screen,
    std::string_view title,
    std::string_view message)
{
    [[maybe_unused]] bool _ = show_dialog_standalone<bool>(
        screen,
        [&](std::function<void(bool)> done) -> Component {
            auto render = [=]() -> Element {
                auto btn = hbox({
                    text(" OK "),
                }) | borderStyled(Color::Cyan) | color(Color::Cyan) | inverted;

                return window(
                    text(" " + std::string{title} + " ") | bold,
                    vbox({
                        text(std::string{message}) | center,
                        text(""),
                        btn | center,
                    }) | size(WIDTH, GREATER_THAN, 40)
                ) | color(Color::Cyan);
            };

            return Renderer(render) | CatchEvent([=](Event event) -> bool {
                if (event == Event::Return || event == Event::Escape ||
                    event == Event::Character(' ')) {
                    done(true);
                    return true;
                }
                return false;
            });
        }
    );
}

// ============================================================
// KeybindingSetup — setup-dialog input bindings
// ============================================================
//
// TS REFERENCE: src/keybindings/KeybindingProviderSetup.tsx
// Faithful port of the standard setup-dialog key bindings:
//   Tab / Shift-Tab / ArrowUp / ArrowDown → cycle focus between components
//   Enter / Return → activate primary (select current focus)
//   Esc → cancel / dismiss (routes to done callback with default result)
//   Space → activate focused component
//
// Wraps a user-provided Component (typically a Container of controls) and
// routes these keys.  FTXUI's built-in Container::Tab handles focus cycling
// via Tab; this wrapper adds the Esc → default semantics on top.

/// Configuration for KeybindingSetup — default values mirror TS.
struct KeybindingSetupOptions {
    /// True = Esc closes the dialog by invoking the done callback with a
    /// default-constructed TResult (e.g. empty optional or Cancel enum).
    /// Set to false for "must choose" dialogs (Esc swallowed, ignored).
    bool esc_dismisses = true;
};

/// Wrap a setup dialog Component with standard setup-dialog keybindings.
/// The builder lambda receives a done<T> callback plus a focus-aware
/// root Component (Container::Tab) it should populate.
template <typename TResult>
[[nodiscard]] Component KeybindingSetup(
    Components children,
    std::function<void(TResult)> done,
    const KeybindingSetupOptions& opts = KeybindingSetupOptions{})
{
    auto selector = std::make_shared<int>(0);
    auto tab = Container::Tab(std::move(children), selector.get());

    // Keep selector alive for the lifetime of tab — capture shared_ptr in the
    // event-handler closure.  FTXUI's Container::Tab just stores the raw
    // pointer; without a GC we need to own the int through the capture.
    return tab | CatchEvent([done, opts, selector](Event event) -> bool {
        // Esc → dismiss with default-constructed result
        if (opts.esc_dismisses && event == Event::Escape) {
            done(TResult{});
            return true;
        }
        // All other events handled by the tab container (Tab navigation,
        // Enter/Space activation via each child's own event handlers).
        return false;
    });
}

// ============================================================
// AppStateBundle — lightweight context for setup dialogs
// ============================================================
//
// TS REFERENCE: AppStateProvider React context.
// Minimal setup-time context bundle that setup dialogs can read to match
// current app chrome (theme, configured model name, user display name,
// version).  Deliberately small — setup dialogs MUST NOT access the full
// AppState (would pull in huge module dependencies into the launcher path).

struct AppStateBundle {
    std::string app_version = "0.0.0";
    std::string configured_model;        // e.g. "claude-opus-4-8"
    std::string user_display_name;       // empty = anonymous
    const Theme* theme = nullptr;        // optional borrowed pointer
    /// True = status-line user setting is enabled (setup dialogs may show
    /// an "Enable status line" checkbox seeded from this).
    bool status_line_enabled = false;
};

// ============================================================
// show_setup_dialog — setup/config dialog runner
// ============================================================
//
// TS REFERENCE: src/components/setup/showSetupDialog.tsx
//                src/utils/interactiveHelpers.js (showSetupDialog impl)
//
// Faithful port pattern:
//   1. Wraps user component in KeybindingSetup (standard nav keys)
//   2. Provides access to an AppStateBundle so dialogs can render with the
//      same theme/user context as the rest of the app
//   3. Runs the FTXUI interactive loop and blocks until the dialog closes
//
// The builder lambda receives (done, state_bundle) and MUST return a
// Component.  Use Components.push_back() to build the children list,
// then pass to Container::Vertical or Container::Tab as you prefer.
//
// Example:
//   auto choice = show_setup_dialog<std::optional<std::string>>(
//       screen, bundle,
//       [](auto done, const AppStateBundle& bundle, Components& children) {
//           auto input = Input(&bundle.configured_model, "model:");
//           auto ok = Button(" OK ", [done, input]{ done(input->content); });
//           children = { input, ok };
//       });
template <typename TResult>
[[nodiscard]] TResult show_setup_dialog(
    ScreenInteractive& screen,
    const AppStateBundle& bundle,
    std::function<void(
        std::function<void(TResult)> done,
        const AppStateBundle& state_bundle,
        Components& out_children)> builder,
    const KeybindingSetupOptions& kb_opts = KeybindingSetupOptions{})
{
    return show_dialog_standalone<TResult>(
        screen,
        [&](std::function<void(TResult)> done) -> Component {
            Components children;
            builder(done, bundle, children);
            // Wrap in KeybindingSetup so Tab/Esc/Enter all work.
            return KeybindingSetup<TResult>(std::move(children), done, kb_opts);
        }
    );
}

/// Convenience overload: builder only takes (done, out_children) and does
/// not need AppStateBundle.  Uses an empty default bundle.
template <typename TResult>
[[nodiscard]] TResult show_setup_dialog(
    ScreenInteractive& screen,
    std::function<void(
        std::function<void(TResult)> done,
        Components& out_children)> builder,
    const KeybindingSetupOptions& kb_opts = KeybindingSetupOptions{})
{
    AppStateBundle empty_bundle;
    return show_setup_dialog<TResult>(
        screen, empty_bundle,
        [&](std::function<void(TResult)> done, const AppStateBundle& /*unused*/,
            Components& out_children) {
            builder(done, out_children);
        },
        kb_opts
    );
}

} // namespace cc::ui::dialogs::launchers
