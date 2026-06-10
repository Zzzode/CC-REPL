/**
 * CC-REPL UI bootstrap (TU2 — Phase C-c split)
 *
 * Translation unit that owns the named-module imports for the UI surface.
 * Goal: keep each individual TU under ~20 transitive module imports so
 * Clang's Source Manager does not run out of source-location IDs (the
 * "ran out of source locations" segfault seen with the monolithic TU).
 *
 * This TU imports:
 *   - cc.config.config                 (ConfigManager cold start)
 *   - cc.ui.design.theme               (set_theme / UI20 palette)
 *   - cc.ui.repl_screen                (ReplScreen component + dialogs)
 *   - ftxui ScreenInteractive headers
 *
 * The entry point is exposed with C linkage so the slim main.cc (TU1) can
 * call it without importing any C++ named modules.
 */

// ── FTXUI / standard headers must come before module imports in C++23 ──
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

// ── Named C++23 modules (kept intentionally small for this TU) ─────────
import cc.config.config;
import cc.ui.design.theme;
import cc.ui.repl_screen;

extern "C" int run_repl_ui(
    int /*argc*/,
    char** /*argv*/,
    bool dry_run,
    const char* model_override
) {
    using namespace ftxui;
    namespace theme_ns = cc::ui::design::theme;

    // ── 1. ConfigManager cold start ────────────────────────────────────
    // Default ctor resolves global + project config paths automatically.
    // We do not require load() to succeed; we fall back to defaults so
    // --dry-run always works on a clean system.
    cc::core::ConfigManager cfg_mgr;
    {
        auto load_result = cfg_mgr.load();
        if (!load_result) {
            std::fprintf(stderr,
                "[cc-repl][WARN] config load failed: %s — using defaults\n",
                load_result.error().message.c_str());
        }
    }
    const auto& cfg = cfg_mgr.settings();

    // Determine model name: CLI override > config default > built-in.
    const std::string model_name = [&] {
        if (model_override && *model_override) {
            return std::string(model_override);
        }
        if (!cfg.model.default_model.empty()) {
            return cfg.model.default_model;
        }
        return std::string("claude-sonnet-4-20250514");
    }();

    // ── 2. Theme initialization (UI20 theme_provider) ──────────────────
    {
        auto variant = theme_ns::parse_variant(cfg.display.theme);
        theme_ns::Accessibility a11y{};
        // Phase C-c: NO_COLOR / TERM=dumb → force_monochrome for forward
        // compat with TU1's --no-color flag.
        if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) {
            a11y.force_monochrome = true;
        }
        theme_ns::set_theme(variant, a11y);
    }
    const auto active_theme = theme_ns::parse_variant(cfg.display.theme);

    // ── 3. Non-interactive / dry-run / no-TTY fast path ────────────────
    const bool has_tty = [] {
#ifndef _WIN32
        return ::isatty(fileno(stdin)) == 1 && ::isatty(fileno(stdout)) == 1;
#else
        return true;
#endif
    }();

    if (dry_run || !has_tty) {
        // Touch the dialog router table to prove the wiring compiles.
        using cc::ui::repl_screen::ReplMode;
        using cc::ui::repl_screen::dialog_stubs::get_builder;
        static constexpr ReplMode kTouch[] = {
            ReplMode::Normal,
            ReplMode::SettingsView,
            ReplMode::HelpView,
            ReplMode::QuickOpen,
            ReplMode::ModelSwitch,
            ReplMode::McpServerList,
            ReplMode::AgentsView,
            ReplMode::TasksView,
            ReplMode::TeamsView,
            ReplMode::Doctor,
            ReplMode::Resume,
        };
        int wired = 0, missing = 0;
        for (auto m : kTouch) {
            auto b = get_builder(m);
            if (b) ++wired; else ++missing;
        }

        // Also construct the top-level ReplScreen component with empty
        // callbacks (engine callbacks injected in Phase D).  We do NOT
        // call .Render() — construction alone is enough to verify the
        // component tree assembles.
        cc::ui::repl_screen::ReplScreenCallbacks empty_cbs{};
        auto repl = cc::ui::repl_screen::ReplScreen(
            std::make_shared<cc::ui::repl_screen::ReplScreenState>(),
            std::move(empty_cbs),
            &cfg_mgr);
        (void)repl;

        std::cout
            << "[cc-repl] Component tree: ReplScreen built (dialog router wired="
            << wired << " / missing=" << missing << ")\n"
            << "[cc-repl] StatusBar: session=new"
            << " / model=" << model_name
            << " / theme=" << theme_ns::variant_name(active_theme)
            << "\n"
            << "[cc-repl] Config: global="
            << cfg_mgr.global_config_path().string()
            << "  project=" << cfg_mgr.project_config_path().string()
            << "\n"
            << "[cc-repl] Dialog router: " << wired << " wired, "
            << missing << " missing\n"
            << "[cc-repl] Prompt: (empty) — Press Esc (Escape) to exit\n";
        return EXIT_SUCCESS;
    }

    // ── 4. Interactive path: FTXUI ScreenInteractive + ReplScreen ──────
    auto repl_state = std::make_shared<cc::ui::repl_screen::ReplScreenState>();
    repl_state->status_bar.model_name = model_name;

    cc::ui::repl_screen::ReplScreenCallbacks cbs{};
    // Phase C-c: only the on_exit callback is wired so Esc / Ctrl+D
    // terminates cleanly.  All other callbacks are filled in by the
    // engine layer during Phase D integration.
    static bool g_should_exit = false;
    cbs.on_exit = [] { g_should_exit = true; };

    auto screen = ScreenInteractive::Fullscreen();
    auto root = cc::ui::repl_screen::ReplScreen(
        repl_state, std::move(cbs), &cfg_mgr);
    screen.Loop(root);

    std::cout << "[cc-repl] terminated cleanly\n";
    return EXIT_SUCCESS;
}
