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

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

} // namespace cc::ui::dialogs::launchers
