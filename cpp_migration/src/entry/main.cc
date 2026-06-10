/**
 * CC-REPL Main Entry Point (TU1 — Phase C-c split)
 *
 * Minimal entry TU: zero C++ named-module imports.  Only standard-library
 * headers are used here to keep the clang Source Manager source-location
 * budget well under its limit.
 *
 * Responsibilities:
 *   - Parse the tiny subset of CLI flags we actually need in Phase C-c.
 *   - Dispatch to TU2 (run_repl_ui) or TU3 (run_cli_bootstrap) via the
 *     `extern "C"` symbols so neither translation unit imports the other.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ─── Cross-TU entry points ──────────────────────────────────────────────────
// Implemented in run_repl_ui.cc (TU2) and run_cli_bootstrap.cc (TU3).
// Using C linkage so the symbols are trivial to resolve between TUs without
// any module BMI visibility requirements.
extern "C" int run_repl_ui(
    int argc,
    char** argv,
    bool dry_run,
    const char* model_override
);
extern "C" int run_cli_bootstrap(int argc, char** argv);

namespace {

constexpr std::string_view kVersion = "0.1.0";

void print_usage() {
    std::cout
        << "CC-REPL: Claude REPL (C++23 Modules version)\n"
        << "Usage: cc-repl [options]\n"
        << "\n"
        << "Options:\n"
        << "  --model <model>   Set the default LLM model to use\n"
        << "  --version, -v     Print version and exit\n"
        << "  --help, -h        Show this help message and exit\n"
        << "  --dry-run         Initialize UI subsystems and exit without\n"
        << "                    opening ScreenInteractive (CI / no-TTY)\n"
        << "  --no-color        Reserved for future use (suppress ANSI color)\n"
        << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    bool dry_run = false;
    bool show_version = false;
    bool show_help = false;
    std::string model_override_storage;
    const char* model_override = nullptr;

    // ── Minimal hand-written argument parser (Phase C-c scope) ─────────
    // We intentionally avoid Commander / argparse style libraries and the
    // <print>/<format> heavy hitters so this TU stays well under the
    // sloc budget.  A simple forward scan is plenty for 5 flags.
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        if (arg == "--version" || arg == "-v") {
            show_version = true;
        } else if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--no-color") {
            // Phase C-c: accepted for forward-compat; actual handling
            // lives in the theme TU (TU2) via NO_COLOR env check.
        } else if (arg == "--model") {
            if (i + 1 < argc) {
                model_override_storage = argv[++i];
                model_override = model_override_storage.c_str();
            } else {
                std::cerr << "Error: --model requires a value\n";
                return EXIT_FAILURE;
            }
        } else if (arg.starts_with("--model=")) {
            model_override_storage = std::string(
                arg.substr(std::string_view("--model=").size()));
            model_override = model_override_storage.c_str();
        } else if (arg.starts_with('-')) {
            std::cerr << "Warning: unknown option '" << arg
                      << "' (ignored in Phase C-c slim entry)\n";
        }
        // Non-option arguments are silently forwarded to run_repl_ui via
        // argc/argv so future phases can extend parsing there.
    }

    // Fast-path exits that never need to touch any named C++ module.
    if (show_version) {
        std::cout << "cc-repl " << kVersion << "\n";
        return EXIT_SUCCESS;
    }
    if (show_help) {
        print_usage();
        return EXIT_SUCCESS;
    }

    // ── Dispatch ───────────────────────────────────────────────────────
    // Phase C-c goal: get the UI path compiling under sloc budget.
    // Non-UI services (daemon / server / bridge / headless) are deferred
    // to run_cli_bootstrap which, in this phase, is a stub return 0.
    const int ui_rc = run_repl_ui(argc, argv, dry_run, model_override);
    if (ui_rc != 0) return ui_rc;

    // Placeholder call for Phase 3 service-layer bootstrap.
    return run_cli_bootstrap(argc, argv);
}
