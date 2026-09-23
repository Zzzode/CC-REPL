// app_run.cpp — impl unit for RunApp() and the extern "C" bridge.
// Kept out of app.cppm so cc.hooks.tool_permissions and the FTXUI
// screen-interactive / termios closure stay out of the interface BMI.
module;

#include <termios.h>  // tcgetattr/tcsetattr/termios/VLNEXT
#include <unistd.h>   // STDIN_FILENO

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

module cc.ui.app.app;

import cc.query.query_engine;
import cc.commands.registry;
import cc.utils.session_storage;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;

namespace cc::ui {

namespace {
[[nodiscard]] int RunApp(
    core::QueryEngine& engine,
    cc::commands::AppCommandRegistry& cmd_registry,
    utils::SessionStorage& storage,
    cc::hooks::ToolPermissionHook* permission_hook,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks
) {
    // Use the alternate-screen fullscreen like TS (AlternateScreen) - the REPL owns the terminal.
    auto screen = ScreenInteractive::Fullscreen();

    // ── macOS/BSD line-discipline workaround: disable VLNEXT ─────────────
    // VLNEXT (the "literal-next" char, Ctrl+V by default) is processed by the
    // terminal line discipline EVEN in non-canonical mode (ICANON off) on
    // macOS/BSD. FTXUI puts the terminal in non-canonical mode (ICANON|ECHO
    // off) but does NOT clear c_cc[VLNEXT], so every Ctrl+V the user presses
    // gets consumed as an lnext escape: a pair of \x16 bytes collapses into a
    // single literal \x16. Net effect: pressing Ctrl+V 8× registers only 4×
    // (floor(N/2)) — half the image-paste keystrokes are silently dropped
    // before FTXUI's event loop ever sees them.
    //
    // Fix: clear VLNEXT ourselves before entering the loop. We do this BEFORE
    // screen.Loop() because FTXUI's Install() (called inside Loop) does
    // tcgetattr()+save-then-restore: it will read our VLNEXT=0, preserve it
    // for the session, and restore that same value on exit. To still give the
    // parent shell back its original Ctrl+V lnext on exit, we snapshot the
    // true original termios here and re-apply it after Loop() returns.
    //
    // Verified: sending N×\x16 through a pty with VLNEXT=0 delivers all N
    // bytes; with VLNEXT at its default, only floor(N/2) arrive. This is
    // independent of the osascript/clipboard path (setsid/closefrom there
    // remain good hygiene but were NOT the cause of keystroke loss).
#if defined(__APPLE__) || defined(__linux__)
    struct termios orig_termios;
    const bool have_orig = (tcgetattr(STDIN_FILENO, &orig_termios) == 0);
    if (have_orig) {
        struct termios t = orig_termios;
        t.c_cc[VLNEXT] = 0;  // 0 == _POSIX_VDISABLE: disable literal-next
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
#endif

    bool should_exit = false;

    auto app = Make<AppAdapter>(
        &engine,
        lifecycle_hooks,
        &cmd_registry,
        &storage,
        [&screen, &should_exit]() {
            should_exit = true;
            screen.Exit();
        }
    );

    app->set_screen(&screen);

    if (permission_hook && !permission_hook->is_auto_approve_mode()) {
        auto ui_callback = app->get_permission_callback();
        permission_hook->set_ask_user_fn(
            [ui_callback](const cc::hooks::PermissionContext& ctx) -> cc::hooks::PermissionDecision {
                bool allowed = ui_callback(ctx.tool_name, ctx.args);
                return allowed ? cc::hooks::PermissionDecision::allow
                               : cc::hooks::PermissionDecision::deny;
            }
        );
    }

    app->SyncState();

    screen.Loop(app);

    // Restore the parent shell's original termios (FTXUI's on_exit restored
    // what IT read, which carries VLNEXT=0; re-apply the true original so
    // Ctrl+V lnext works again in the user's shell after loom exits).
#if defined(__APPLE__) || defined(__linux__)
    if (have_orig) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
#endif

    return should_exit ? 0 : 1;
}


}  // namespace

extern "C" int cc_ui_run_app_bridge(
    cc::core::QueryEngine* engine,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
    cc::commands::AppCommandRegistry* cmd_registry,
    cc::utils::SessionStorage* storage,
    cc::hooks::ToolPermissionHook* permission_hook
) {
    return cc::ui::RunApp(*engine, *cmd_registry, *storage, permission_hook, lifecycle_hooks);
}

}  // namespace cc::ui
