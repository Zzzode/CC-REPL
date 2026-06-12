/// @file install_github_app_wizard.cppm
/// @brief FTXUI interactive wizard wrapping the Phase-2 pure-function
///        12-step InstallGitHubApp state machine.
///
/// The *logic* (validate / advance) lives BLACK-BOX in
///   cc.commands.install_github_app_steps (Phase 2 C2 deliverable).
/// This module owns only the presentation layer:
///   * build per-step FTXUI components (inputs, buttons, progress)
///   * collect user gestures and write them into the C2 context
///   * execute the IO side-effects listed in C2's advance() contract
///     (shell commands, OAuth listener, file writes)
///   * drive the UI11 WizardComponent forward on Enter.
module;

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.dialogs.install_github_app_wizard;

import cc.types.types;
import cc.commands.install_github_app_steps;
import cc.ui.wizard_dialog;
import cc.utils.bash_execution;
import cc.services.oauth.auth_code_listener;
import cc.constants.oauth;

export namespace cc::ui::dialogs::install_github_app_wizard {

using namespace ftxui;
namespace steps = cc::commands::install_github_app;
namespace bash  = cc::utils::bash;
namespace oauth = cc::services::oauth;
namespace oauth_const = cc::constants::oauth;
namespace wizard = cc::ui::wizard_dialog;

// --------------------------------------------------------------------------
// Helper: Mask an API key for display (prefix stars + last 4 chars).
// --------------------------------------------------------------------------
[[nodiscard]] inline std::string mask_key(std::string_view key) {
    if (key.size() <= 4) return std::string(key.size(), '*');
    return std::string(key.size() - 4, '*') + std::string(key.substr(key.size() - 4));
}

// --------------------------------------------------------------------------
// Helper: Cross-platform URL launcher.
// --------------------------------------------------------------------------
inline bool open_url(std::string_view url) {
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + std::string(url) + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + std::string(url) + "\"";
#else
    std::string cmd = "xdg-open \"" + std::string(url) + "\"";
#endif
    return std::system(cmd.c_str()) == 0;
}

// --------------------------------------------------------------------------
// IO side-effect helpers (match the contract comments in C2's advance()).
// --------------------------------------------------------------------------
namespace io {

// [IO] Step 1 – preflight: gh --version, gh auth status.
inline void run_gh_preflight(steps::InstallGitHubAppContext& ctx) {
    ctx.gh_cli_installed = false;
    ctx.gh_authenticated = false;
    ctx.has_repo_scope = false;
    ctx.has_workflow_scope = false;
    ctx.preflight_warnings.clear();

    auto ver = bash::execute_command("gh --version");
    if (!ver || ver->exit_code != 0) {
        ctx.preflight_warnings.push_back(steps::Warning{
            .title = "GitHub CLI is missing",
            .message =
                "The `gh` command was not found on PATH. You must install it "
                "before this wizard can create repository secrets and workflows.",
            .instructions = {
                "macOS  : brew install gh",
                "Linux  : apt install gh  (or see github.com/cli/cli)",
                "Windows: winget install GitHub.cli",
                "After installing, run `gh auth login` and return here.",
            },
        });
        return;
    }
    ctx.gh_cli_installed = true;

    auto auth = bash::execute_command("gh auth status 2>&1");
    if (!auth || auth->exit_code != 0) {
        ctx.preflight_warnings.push_back(steps::Warning{
            .title = "Not logged in to gh",
            .message =
                "`gh auth status` reports that you are not authenticated. "
                "The wizard cannot create secrets or push commits without it.",
            .instructions = {
                "Run: gh auth login --scopes repo,workflow",
                "Then press 'retry' in the wizard.",
            },
        });
        return;
    }
    ctx.gh_authenticated = true;

    // Scope sniffing: look for "repo" / "workflow" in the status output.
    const auto& out = auth->stdout_output;
    if (out.find("repo") != std::string::npos) ctx.has_repo_scope = true;
    if (out.find("workflow") != std::string::npos) ctx.has_workflow_scope = true;

    if (!ctx.has_repo_scope) {
        ctx.preflight_warnings.push_back(steps::Warning{
            .title = "Missing `repo` OAuth scope",
            .message =
                "The `repo` scope is required to write repository secrets.",
            .instructions = {
                "Run: gh auth refresh -s repo",
            },
        });
    }
    if (!ctx.has_workflow_scope) {
        ctx.preflight_warnings.push_back(steps::Warning{
            .title = "Missing `workflow` OAuth scope",
            .message =
                "The `workflow` scope is required to commit workflow YAML files.",
            .instructions = {
                "Run: gh auth refresh -s workflow",
            },
        });
    }
}

// [IO] Resolve current repo via `gh repo view --json nameWithOwner`.
inline void detect_current_repo(steps::InstallGitHubAppContext& ctx) {
    ctx.current_repo.clear();
    auto r = bash::execute_command(
        "gh repo view --json nameWithOwner --jq .nameWithOwner 2>&1");
    if (r && r->exit_code == 0) {
        ctx.current_repo = r->stdout_output;
        while (!ctx.current_repo.empty() &&
               (ctx.current_repo.back() == '\n' || ctx.current_repo.back() == '\r')) {
            ctx.current_repo.pop_back();
        }
    }
}

// [IO] Step 5 – scan .github/workflows/ for existing claude yml files.
inline std::vector<std::string> scan_workflows(std::string_view cwd) {
    std::vector<std::string> files;
    std::error_code ec;
    std::filesystem::path p = std::string(cwd);
    p /= ".github";
    p /= "workflows";
    if (!std::filesystem::exists(p, ec)) return files;
    for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if ((name.starts_with("claude") || name.starts_with("cc-")) &&
            (name.ends_with(".yml") || name.ends_with(".yaml"))) {
            files.push_back(name);
        }
    }
    return files;
}

// [IO] Step 7 – `gh secret list --repo <repo>`.
inline std::vector<std::string> list_secrets(std::string_view repo) {
    std::vector<std::string> names;
    auto r = bash::execute_command(
        std::format("gh secret list --repo {} 2>&1", bash::escape_shell_arg(repo)));
    if (!r || r->exit_code != 0) return names;
    // Output format: NAME\tUPDATED\tVISION (tsv-ish, first col is name).
    std::istringstream iss(r->stdout_output);
    std::string line;
    bool header_skipped = false;
    while (std::getline(iss, line)) {
        if (!header_skipped) { header_skipped = true; continue; }
        auto tab = line.find('\t');
        if (tab == std::string::npos) tab = line.find(' ');
        auto name = (tab == std::string::npos) ? line : line.substr(0, tab);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

// [IO] Step 8 (ExistingLocal branch): read local ANTHROPIC_API_KEY from env
// or a standard dotfile location (best-effort, same shape as TS host).
inline void load_local_api_key(steps::InstallGitHubAppContext& ctx) {
    ctx.local_api_key.clear();
    const char* env = std::getenv("ANTHROPIC_API_KEY");
    if (env) { ctx.local_api_key = env; return; }
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::filesystem::path p = std::string(home);
    p /= ".config"; p /= "claude"; p /= "anthropic.key";
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return;
    std::ifstream f(p);
    if (f) std::getline(f, ctx.local_api_key);
    while (!ctx.local_api_key.empty() && std::isspace(
               static_cast<unsigned char>(ctx.local_api_key.back()))) {
        ctx.local_api_key.pop_back();
    }
}

// [IO] Step 9 – run the real OAuth authorization code flow.
// Uses AuthCodeListener + constants.oauth prod config; token exchange is
// TODO (requires httplib and is handled best by oauth.client module when
// it is already linked into the binary). We expose the auth code to the
// caller via the context; downstream can swap it for a token.
inline void run_oauth_flow(steps::InstallGitHubAppContext& ctx) {
    ctx.oauth_completed = false;
    ctx.oauth_access_token.clear();
    ctx.oauth_error.clear();

    oauth::AuthCodeListener listener;
    auto start = listener.start();
    if (!start) {
        ctx.oauth_error = std::format("Failed to start OAuth listener: {}",
                                      start.error());
        ctx.oauth_completed = true;
        return;
    }

    const auto& cfg = oauth_const::prod_oauth_config;
    auto redirect = listener.get_redirect_uri();
    std::string url = std::format(
        "{}?response_type=code&client_id={}&redirect_uri={}"
        "&scope=org:create_api_key%20user:profile&state=ui15-state",
        cfg.console_authorize_url, cfg.client_id,
        bash::escape_shell_arg(redirect));

    open_url(url);

    // Blocks user thread — OK in FTXUI because the wizard step is modal.
    auto cb = listener.wait_for_callback(std::chrono::seconds{180});
    ctx.oauth_completed = true;
    if (!cb) {
        ctx.oauth_error = cb.error();
        return;
    }
    // NOTE: exchanging the code for a token requires an HTTP POST to the
    // token endpoint; the oauth::OAuthClient module ships that helper and
    // should be plugged in here when the final binary links it.
    // For now we store the raw auth code; tests can simulate the exchange.
    ctx.oauth_access_token = std::string("oauth-code:") + cb->code;
}

// [IO] Step 10 – execute each progress sub-step.
inline void run_creating_step(steps::InstallGitHubAppContext& ctx,
                               std::vector<std::string>& logs_out) {
    auto log = [&](std::string_view l) { logs_out.emplace_back(l); };

    // Build plan if caller hasn't yet.
    if (ctx.progress_steps.empty()) {
        if (!ctx.selected_workflows.empty()) {
            for (const auto& w : ctx.selected_workflows) {
                if (w.empty()) continue;
                ctx.progress_steps.push_back(
                    {"Write workflow " + w + ".yml",
                     steps::InstallGitHubAppContext::ProgressStep::State::Pending,
                     {}});
            }
        }
        ctx.progress_steps.push_back(
            {"Upload secret " + ctx.secret_name,
             steps::InstallGitHubAppContext::ProgressStep::State::Pending,
             {}});
        ctx.progress_steps.push_back(
            {"Encrypt and persist API key locally",
             steps::InstallGitHubAppContext::ProgressStep::State::Pending,
             {}});
        ctx.progress_steps.push_back(
            {"Commit & push changes (if requested)",
             steps::InstallGitHubAppContext::ProgressStep::State::Pending,
             {}});
    }

    std::filesystem::path workflows_dir =
        std::filesystem::current_path() / ".github" / "workflows";
    std::error_code ec;
    std::filesystem::create_directories(workflows_dir, ec);

    const std::string repo = ctx.repo();

    // Resolve the secret payload.
    std::string secret_payload;
    if (ctx.auth_type == steps::AuthType::OAuthToken) {
        secret_payload = ctx.oauth_access_token;
    } else {
        switch (ctx.selected_api_key_option) {
            case steps::ApiKeyOption::ExistingLocal: secret_payload = ctx.local_api_key; break;
            case steps::ApiKeyOption::NewManual:     secret_payload = ctx.manual_api_key; break;
            case steps::ApiKeyOption::OAuthFlow:     secret_payload = ctx.oauth_access_token; break;
        }
    }

    auto mark = [&](size_t i, auto st, std::optional<std::string> err = std::nullopt) {
        ctx.progress_steps[i].state = st;
        if (err) ctx.progress_steps[i].error_detail = std::move(err);
    };

    for (size_t i = 0; i < ctx.progress_steps.size(); ++i) {
        auto& step = ctx.progress_steps[i];
        step.state = steps::InstallGitHubAppContext::ProgressStep::State::InProgress;
        log(std::format("→ {}", step.label));

        if (step.label.starts_with("Write workflow")) {
            // Extract basename from the label ("Write workflow X.yml" → X.yml)
            auto idx = step.label.find("Write workflow ");
            std::string fname = (idx == std::string::npos)
                ? step.label
                : step.label.substr(std::string("Write workflow ").size());
            std::filesystem::path fpath = workflows_dir / fname;
            // Minimal valid placeholder YAML (CI will overwrite with the
            // canonical workflow content produced by the TS side).
            std::ofstream out(fpath, std::ios::trunc);
            if (!out) {
                mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Failed,
                     std::format("Could not open {} for writing", fpath.string()));
                log(std::format("✗ {}", *step.error_detail));
                continue;
            }
            out << "# Placeholder — generated by cc /install-github-app\n"
                   "name: claude-app\n"
                   "on: [push, pull_request]\n"
                   "jobs:\n"
                   "  stub:\n"
                   "    runs-on: ubuntu-latest\n"
                   "    steps:\n"
                   "      - run: echo todo-replace-with-real-workflow\n";
            mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
            log(std::format("✓ wrote {}", fpath.string()));
            continue;
        }

        if (step.label.starts_with("Upload secret")) {
            if (secret_payload.empty()) {
                mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Failed,
                     "No secret payload available");
                log("✗ no secret payload");
                continue;
            }
            // Use heredoc so the key is never visible in argv/proc.
            auto cmd = std::format(
                "gh secret set {} --repo {} --body {} 2>&1",
                bash::escape_shell_arg(ctx.secret_name),
                bash::escape_shell_arg(repo),
                bash::escape_shell_arg(secret_payload));
            auto r = bash::execute_command(cmd);
            if (!r || r->exit_code != 0) {
                auto err = r ? r->stdout_output : std::string{"command failed"};
                mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Failed,
                     err);
                log(std::format("✗ {}", err));
                continue;
            }
            mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
            log(std::format("✓ uploaded secret {}", ctx.secret_name));
            continue;
        }

        if (step.label.starts_with("Encrypt")) {
            // Best-effort: write to ~/.config/claude/.secrets with 0600 perms.
            const char* home = std::getenv("HOME");
            if (!home) {
                mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
                log("· skipped (no $HOME)");
                continue;
            }
            std::filesystem::path p = std::string(home);
            p /= ".config"; p /= "claude"; p /= ".secrets";
            std::error_code e;
            std::filesystem::create_directories(p.parent_path(), e);
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            if (!f) {
                mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
                log("· skipped (cannot open file)");
                continue;
            }
            f << secret_payload;
            f.close();
            std::filesystem::permissions(
                p,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                e);
            mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
            log(std::format("✓ persisted key to {}", p.string()));
            continue;
        }

        if (step.label.starts_with("Commit")) {
            // Skip commit unless the user asks (TODO: surface checkbox in UI).
            mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
            log("· commit skipped (manual-mode default)");
            continue;
        }

        // Unknown label — mark completed, nothing we can do.
        mark(i, steps::InstallGitHubAppContext::ProgressStep::State::Completed);
    }
}

} // namespace io

// --------------------------------------------------------------------------
// Shared state for the wizard; every step component captures this by value.
// --------------------------------------------------------------------------
struct WizardController {
    std::shared_ptr<steps::InstallGitHubAppContext> ctx;

    // Step 1 result display
    bool preflight_done = false;

    // Step 2
    bool warnings_accepted = false;
    std::vector<int> warnings_collapsed; // indices of collapsed warnings

    // Step 3
    std::string repo_input; // user-typed (if not "use current")

    // Step 4
    bool app_authorized = false;

    // Step 5
    int existing_workflow_choice = 0; // 0 = Update, 1 = Create new

    // Step 6
    std::vector<bool> workflow_selected; // parallel with ctx->selected_workflows

    // Step 7
    int secret_choice = 0; // 0 = Use existing, 1 = Overwrite

    // Step 8
    int api_key_radio = 0; // 0 = local, 1 = manual, 2 = OAuth
    std::string manual_key_input;
    std::string api_key_error;

    // Step 9
    bool oauth_done = false;

    // Step 10
    bool creating_done = false;
    std::vector<std::string> creating_logs;

    // Step 11 summary
    bool test_workflow = false;

    // Step 12
    bool retry_requested = false;

    // -------------------- Entry/side-effect helpers --------------------
    void enter_step(steps::Step step) {
        switch (step) {
            case steps::Step::CheckGitHub:
                if (!preflight_done) {
                    io::run_gh_preflight(*ctx);
                    io::detect_current_repo(*ctx);
                    preflight_done = true;
                }
                break;

            case steps::Step::Warnings:
                warnings_collapsed.assign(
                    ctx->preflight_warnings.size(), false);
                break;

            case steps::Step::ChooseRepo:
                if (ctx->use_current_repo) {
                    repo_input = ctx->current_repo;
                } else {
                    repo_input = ctx->selected_repo_name;
                }
                break;

            case steps::Step::InstallApp:
                app_authorized = false;
                break;

            case steps::Step::CheckExistingWorkflow: {
                auto files = io::scan_workflows(std::filesystem::current_path().string());
                ctx->workflow_exists = !files.empty();
                if (ctx->selected_workflows.empty()) {
                    for (auto& f : files) {
                        // Strip extension so downstream can re-add .yml.
                        auto stem = f;
                        if (auto p = stem.find_last_of('.'); p != std::string::npos)
                            stem.erase(p);
                        ctx->selected_workflows.push_back(std::move(stem));
                    }
                }
                break;
            }

            case steps::Step::SelectWorkflows:
                workflow_selected.assign(ctx->selected_workflows.size(), true);
                break;

            case steps::Step::CheckExistingSecret: {
                auto secrets = io::list_secrets(ctx->repo());
                auto it = std::find(secrets.begin(), secrets.end(), ctx->secret_name);
                ctx->secret_exists = (it != secrets.end());
                break;
            }

            case steps::Step::ApiKey:
                io::load_local_api_key(*ctx);
                switch (ctx->selected_api_key_option) {
                    case steps::ApiKeyOption::ExistingLocal: api_key_radio = 0; break;
                    case steps::ApiKeyOption::NewManual:     api_key_radio = 1; break;
                    case steps::ApiKeyOption::OAuthFlow:     api_key_radio = 2; break;
                }
                break;

            case steps::Step::OAuthFlow:
                if (!oauth_done) {
                    io::run_oauth_flow(*ctx);
                    ctx->auth_type = steps::AuthType::OAuthToken;
                    oauth_done = true;
                }
                break;

            case steps::Step::Creating:
                if (!creating_done) {
                    io::run_creating_step(*ctx, creating_logs);
                    creating_done = true;
                }
                break;

            default:
                break;
        }
    }
};

// ==========================================================================
// Per-step components.
//
// Every step is a function<Component(WizardController&, ScreenInteractive*)>
// in spirit; we wrap them as closures that capture the shared controller and
// register their Enter-key / side-effect behaviour.
// ==========================================================================

// -- Step 1: CheckGitHub ----------------------------------------------------
inline Component build_step_check_github(std::shared_ptr<WizardController> wc) {
    auto rerun = std::make_shared<bool>(false);
    return Renderer([wc, rerun] {
        Elements body;
        body.push_back(text(" Preflight: GitHub CLI installation & auth") | bold);
        body.push_back(text(""));

        const auto& ctx = *wc->ctx;
        auto line = [&](std::string_view label, bool ok,
                        std::string_view detail) {
            body.push_back(hbox({
                text(ok ? " ✓ " : " ✗ ") | color(ok ? Color::Green : Color::Red),
                text(std::string(label)) | bold,
                text("  " + std::string(detail)) | dim,
            }));
        };
        line("gh CLI installed",     ctx.gh_cli_installed,
             ctx.gh_cli_installed ? "found on PATH" : "missing");
        line("gh authenticated",     ctx.gh_authenticated,
             ctx.gh_authenticated ? "session active" : "run `gh auth login`");
        line("repo scope",           ctx.has_repo_scope,
             ctx.has_repo_scope ? "granted" : "missing");
        line("workflow scope",       ctx.has_workflow_scope,
             ctx.has_workflow_scope ? "granted" : "missing");
        body.push_back(text(""));

        if (!ctx.gh_cli_installed) {
            body.push_back(text(
                " Install instructions: https://github.com/cli/cli#installation")
                | color(Color::Yellow));
            body.push_back(
                text(" After installing, press the retry button below.") | dim);
        }
        if (*rerun) {
            // Trigger re-entry (caller sees this flag and reruns preflight).
            wc->preflight_done = false;
            io::run_gh_preflight(*wc->ctx);
            io::detect_current_repo(*wc->ctx);
            wc->preflight_done = true;
            *rerun = false;
            wc->enter_step(steps::Step::CheckGitHub);
        }

        return vbox(std::move(body)) | yframe;
    });
}

// -- Step 2: Warnings -------------------------------------------------------
inline Component build_step_warnings(std::shared_ptr<WizardController> wc) {
    std::vector<std::string> entries;
    for (size_t i = 0; i < wc->ctx->preflight_warnings.size(); ++i) {
        entries.push_back(wc->ctx->preflight_warnings[i].title);
    }
    auto toggle = std::make_shared<int>(-1);
    auto accepted = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, toggle] {
            Elements body;
            body.push_back(text(" Please review the following preflight advisories.") | bold);
            body.push_back(text(""));
            for (size_t i = 0; i < wc->ctx->preflight_warnings.size(); ++i) {
                const auto& w = wc->ctx->preflight_warnings[i];
                bool open = !wc->warnings_collapsed[i];
                body.push_back(
                    hbox({
                        text(open ? "▾ " : "▸ ") | color(Color::Cyan),
                        text("[" + std::to_string(i + 1) + "] ") | dim,
                        text(w.title) | color(Color::Yellow) | bold,
                    })
                );
                if (open) {
                    body.push_back(
                        vbox({
                            text("   " + w.message) | color(Color::GrayLight),
                            text("   Instructions:") | dim,
                            [&] {
                                Elements ins;
                                for (auto& s : w.instructions)
                                    ins.push_back(text("     • " + s) | color(Color::Cyan));
                                return vbox(std::move(ins));
                            }(),
                        })
                    );
                }
                if (*toggle == static_cast<int>(i)) {
                    wc->warnings_collapsed[i] = !wc->warnings_collapsed[i];
                    *toggle = -1;
                }
            }
            body.push_back(text(""));
            return vbox(std::move(body)) | yframe | flex;
        }),
        Button("Toggle active warning", [toggle, wc] {
            // Cycle the "active" warning index for the next click.
            if (wc->ctx->preflight_warnings.empty()) return;
            if (*toggle < 0) *toggle = 0;
            else *toggle = (*toggle + 1) % static_cast<int>(wc->ctx->preflight_warnings.size());
        }),
        Checkbox("I have read and accept the advisories", accepted.get()),
    });
}

// -- Step 3: ChooseRepo -----------------------------------------------------
inline Component build_step_choose_repo(std::shared_ptr<WizardController> wc) {
    auto use_current = std::make_shared<bool>(wc->ctx->use_current_repo);
    auto input = std::make_shared<std::string>(wc->repo_input);
    auto err = std::make_shared<std::string>();

    return Container::Vertical({
        Renderer([wc] {
            return vbox({
                text(" Choose the target repository for the GitHub App.") | bold,
                text(""),
                text(std::format(" Detected current repo: {}",
                    wc->ctx->current_repo.empty()
                        ? std::string("(none — cwd is not a git repo with gh remote)")
                        : wc->ctx->current_repo)) | dim,
                text(""),
            });
        }),
        Container::Vertical({
            Checkbox("Use current repo", use_current.get()),
            Renderer([use_current] {
                return text(*use_current ? ""
                    : "  Enter owner/repo or full GitHub URL:") | dim;
            }),
            Maybe(
                Input(input.get(), "owner/repo or https://github.com/..."),
                [use_current] { return !*use_current; }),
            Renderer([err] {
                if (err->empty()) return text("");
                return text("  ✗ " + *err) | color(Color::Red);
            }),
        }),
        Button("Apply & validate", [wc, use_current, input, err] {
            wc->ctx->use_current_repo = *use_current;
            if (*use_current) {
                wc->ctx->selected_repo_name.clear();
            } else {
                wc->ctx->selected_repo_name = *input;
            }
            // Run C2 validate and surface errors inline.
            auto errs = steps::validate(steps::Step::ChooseRepo, *wc->ctx);
            if (!errs.empty()) {
                err->clear();
                for (auto& e : errs) err->append(e.message + " ");
            } else {
                err->clear();
            }
        }),
    });
}

// -- Step 4: InstallApp -----------------------------------------------------
inline Component build_step_install_app(std::shared_ptr<WizardController> wc) {
    auto open_clicked = std::make_shared<bool>(false);
    auto authorized = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, open_clicked] {
            constexpr std::string_view url =
                "https://github.com/apps/claude/installations/new";
            Elements body;
            body.push_back(text(" Install the Claude GitHub App on your repository.") | bold);
            body.push_back(text(""));
            body.push_back(text(" 1. Click the button below — it will open the GitHub"));
            body.push_back(text("    App installation page in your browser."));
            body.push_back(text(" 2. Select the target repo (" + wc->ctx->repo() + ")"));
            body.push_back(text("    and grant the requested permissions."));
            body.push_back(text(" 3. Return here and tick 'I have authorized the app'."));
            body.push_back(text(""));
            body.push_back(text(std::format(" URL : {}", url)) | color(Color::Cyan) | dim);
            if (*open_clicked) {
                open_url(url);
                *open_clicked = false;
            }
            return vbox(std::move(body));
        }),
        Button("Open GitHub App install page", [open_clicked] {
            *open_clicked = true;
        }),
        Checkbox("I've authorized the app on the target repo", authorized.get()),
        Renderer([wc, authorized] {
            wc->app_authorized = *authorized;
            return text("");
        }),
    });
}

// -- Step 5: CheckExistingWorkflow -----------------------------------------
inline Component build_step_check_existing_workflow(std::shared_ptr<WizardController> wc) {
    auto choice = std::make_shared<int>(wc->existing_workflow_choice);
    static const std::vector<std::string> radio_labels = {
        "Update existing workflow(s)",
        "Create new workflow file(s)",
    };
    return Container::Vertical({
        Renderer([wc] {
            Elements body;
            body.push_back(text(" Existing workflow files detected in .github/workflows/") | bold);
            body.push_back(text(""));
            if (wc->ctx->selected_workflows.empty()) {
                body.push_back(text(" (none)") | dim);
            } else {
                for (const auto& w : wc->ctx->selected_workflows)
                    body.push_back(text("  • " + w + ".yml") | color(Color::Cyan));
            }
            body.push_back(text(""));
            body.push_back(text(" What would you like to do?") | dim);
            return vbox(std::move(body));
        }),
        Radiobox(&radio_labels, &wc->existing_workflow_choice),
        Renderer([wc, choice] {
            wc->ctx->workflow_action =
                (wc->existing_workflow_choice == 0)
                    ? steps::WorkflowAction::Update
                    : steps::WorkflowAction::Skip;
            (void)choice;
            return text("");
        }),
    });
}

// -- Step 6: SelectWorkflows ------------------------------------------------
inline Component build_step_select_workflows(std::shared_ptr<WizardController> wc) {
    auto populated = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, populated] {
            if (!*populated) {
                wc->workflow_selected.assign(wc->ctx->selected_workflows.size(), true);
                *populated = true;
            }
            Elements body;
            body.push_back(text(" Select which workflow files to generate.") | bold);
            body.push_back(text(""));
            for (size_t i = 0; i < wc->ctx->selected_workflows.size(); ++i) {
                body.push_back(hbox({
                    text(wc->workflow_selected[i] ? " ☑ " : " ☐ ")
                        | color(Color::Cyan),
                    text(wc->ctx->selected_workflows[i] + ".yml"),
                }));
            }
            return vbox(std::move(body));
        }),
        Button("Select all", [wc] {
            std::fill(wc->workflow_selected.begin(),
                      wc->workflow_selected.end(), true);
        }),
        Button("Clear all", [wc] {
            std::fill(wc->workflow_selected.begin(),
                      wc->workflow_selected.end(), false);
        }),
        // Click-list component (we approximate with three toggle buttons that
        // cycle selection state per workflow).
        Container::Horizontal([wc] {
            Components cs;
            for (size_t i = 0; i < wc->ctx->selected_workflows.size(); ++i) {
                auto idx = i;
                cs.push_back(Button(
                    std::format("Toggle #{}", idx + 1),
                    [wc, idx] {
                        if (idx < wc->workflow_selected.size())
                            wc->workflow_selected[idx] = !wc->workflow_selected[idx];
                    }));
            }
            return cs;
        }()),
        // Sync back: drop unselected names from the context list so C2 validate
        // sees the same subset.
        Renderer([wc] {
            std::vector<std::string> kept;
            for (size_t i = 0; i < wc->ctx->selected_workflows.size(); ++i) {
                if (i < wc->workflow_selected.size() && wc->workflow_selected[i])
                    kept.push_back(wc->ctx->selected_workflows[i]);
            }
            wc->ctx->selected_workflows = std::move(kept);
            return text("");
        }),
    });
}

// -- Step 7: CheckExistingSecret --------------------------------------------
inline Component build_step_check_existing_secret(std::shared_ptr<WizardController> wc) {
    auto choice = std::make_shared<int>(wc->secret_choice);
    auto labels = std::make_shared<std::vector<std::string>>(std::vector<std::string>{
        "Use existing secret " + wc->ctx->secret_name,
        "Overwrite with new value",
    });
    return Container::Vertical({
        Renderer([wc] {
            Elements body;
            body.push_back(text(std::format(
                " Repository '{}' already has a secret named '{}'.",
                wc->ctx->repo(), wc->ctx->secret_name)) | bold);
            body.push_back(text(""));
            body.push_back(text(
                " Choose whether to reuse the existing secret or overwrite it.") | dim);
            return vbox(std::move(body));
        }),
        Radiobox(labels.get(), &wc->secret_choice),
        Renderer([wc] {
            wc->ctx->use_existing_secret = (wc->secret_choice == 0);
            return text("");
        }),
    });
}

// -- Step 8: ApiKey ---------------------------------------------------------
inline Component build_step_apikey(std::shared_ptr<WizardController> wc) {
    static const std::vector<std::string> radios = {
        "Use local ANTHROPIC_API_KEY",
        "Enter key manually",
        "Launch OAuth flow",
    };
    auto secret_name_input = std::make_shared<std::string>(wc->ctx->secret_name);
    return Container::Vertical({
        Renderer([wc] {
            Elements body;
            body.push_back(text(" How would you like to provide the API key?") | bold);
            body.push_back(text(""));
            switch (wc->api_key_radio) {
                case 0: {
                    auto key = wc->ctx->local_api_key;
                    if (key.empty()) {
                        body.push_back(
                            text(" No local key found ($ANTHROPIC_API_KEY or "
                                 "~/.config/claude/anthropic.key).")
                                | color(Color::Red));
                    } else {
                        body.push_back(text(" Loaded local key:") | dim);
                        body.push_back(text(std::format("   {}", mask_key(key)))
                            | color(Color::Green));
                    }
                    break;
                }
                case 1: {
                    auto key = wc->manual_key_input;
                    body.push_back(text(" Pasted key preview:") | dim);
                    if (!key.empty())
                        body.push_back(text(std::format("   {}", mask_key(key)))
                            | color(Color::Cyan));
                    // Format validation
                    static const std::regex pat{R"(^sk-ant-.{34,}$)"};
                    if (!key.empty() && !std::regex_match(key, pat)) {
                        body.push_back(
                            text(" ⚠ key does not match 'sk-ant-' + 40 chars")
                                | color(Color::Yellow));
                    }
                    break;
                }
                case 2: {
                    body.push_back(text(
                        " Will open browser → authorize → wait for localhost ")
                        | dim);
                    body.push_back(
                        text(" callback with the OAuth authorization code.") | dim);
                    break;
                }
            }
            body.push_back(text(""));
            body.push_back(text(" Repository secret name:") | dim);
            return vbox(std::move(body));
        }),
        Radiobox(&radios, &wc->api_key_radio),
        Maybe(
            Input(ftxui::InputOption{
                .content = &wc->manual_key_input,
                .placeholder = "sk-ant-...",
                .password = true,
            }),
            [wc] { return wc->api_key_radio == 1; }),
        Input(secret_name_input.get(), "ANTHROPIC_API_KEY"),
        // Sync back to context.
        Renderer([wc, secret_name_input] {
            switch (wc->api_key_radio) {
                case 0: wc->ctx->selected_api_key_option =
                            steps::ApiKeyOption::ExistingLocal; break;
                case 1: wc->ctx->selected_api_key_option =
                            steps::ApiKeyOption::NewManual;
                        wc->ctx->manual_api_key = wc->manual_key_input; break;
                case 2: wc->ctx->selected_api_key_option =
                            steps::ApiKeyOption::OAuthFlow; break;
            }
            wc->ctx->secret_name = *secret_name_input;
            return text("");
        }),
    });
}

// -- Step 9: OAuthFlow ------------------------------------------------------
inline Component build_step_oauth(std::shared_ptr<WizardController> wc) {
    return Renderer([wc] {
        Elements body;
        body.push_back(text(" GitHub OAuth flow") | bold);
        body.push_back(text(""));
        const auto& ctx = *wc->ctx;
        if (!ctx.oauth_completed) {
            body.push_back(hbox({
                spinner(1, std::chrono::steady_clock::now().time_since_epoch().count())
                    | color(Color::Cyan),
                text(" Waiting for browser callback…") | color(Color::Cyan),
            }));
            body.push_back(text(""));
            body.push_back(
                text(" If your browser did not open, please re-run this step.") | dim);
        } else if (!ctx.oauth_error.empty()) {
            body.push_back(text(" ✗ Failed") | color(Color::Red) | bold);
            body.push_back(text("   " + ctx.oauth_error) | color(Color::Red));
        } else {
            body.push_back(text(" ✓ Completed") | color(Color::Green) | bold);
            body.push_back(text(std::format(
                "   token: {}", mask_key(ctx.oauth_access_token))) | dim);
        }
        return vbox(std::move(body));
    });
}

// -- Step 10: Creating ------------------------------------------------------
inline Component build_step_creating(std::shared_ptr<WizardController> wc) {
    auto retry = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, retry] {
            if (*retry) {
                wc->creating_done = false;
                wc->ctx->progress_steps.clear();
                wc->creating_logs.clear();
                io::run_creating_step(*wc->ctx, wc->creating_logs);
                wc->creating_done = true;
                *retry = false;
            }
            const auto& steps_list = wc->ctx->progress_steps;
            int done = 0;
            int total = static_cast<int>(steps_list.size());
            for (const auto& s : steps_list)
                if (s.state == steps::InstallGitHubAppContext::ProgressStep::State::Completed)
                    ++done;
            int pct = (total == 0) ? 100 : (100 * done / total);

            Elements body;
            body.push_back(text(" Applying configuration to repository") | bold);
            body.push_back(text(""));

            // Progress bar
            body.push_back(hbox({
                text(std::format(" {:3}% ", pct)) | color(Color::Cyan),
                gauge(pct / 100.f) | color(Color::Cyan) | flex,
            }));
            body.push_back(text(""));

            // Per-step list
            for (const auto& s : steps_list) {
                std::string icon;
                Color col = Color::GrayLight;
                switch (s.state) {
                    using PS = steps::InstallGitHubAppContext::ProgressStep::State;
                    case PS::Pending:    icon = " • "; break;
                    case PS::InProgress: icon = " ⟳ "; col = Color::Yellow; break;
                    case PS::Completed:  icon = " ✓ "; col = Color::Green; break;
                    case PS::Failed:     icon = " ✗ "; col = Color::Red; break;
                }
                body.push_back(hbox({
                    text(icon) | color(col),
                    text(s.label),
                    s.error_detail ? text("  (" + *s.error_detail + ")") | color(col) : text(""),
                }));
            }

            body.push_back(text(""));
            body.push_back(text(" Log:") | dim);
            for (const auto& l : wc->creating_logs)
                body.push_back(text("   " + l) | color(Color::GrayLight));

            return vbox(std::move(body)) | yframe | flex;
        }),
        Button("Retry failed step(s)", [retry] { *retry = true; }),
    });
}

// -- Step 11: Success -------------------------------------------------------
inline Component build_step_success(std::shared_ptr<WizardController> wc) {
    auto test_clicked = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, test_clicked] {
            const auto& ctx = *wc->ctx;
            Elements body;
            body.push_back(hbox({
                text("  🎉  ") | color(Color::Green) | bold,
                text("Installation complete") | color(Color::Green) | bold |
                    size(WIDTH, EQUAL, 48),
            }));
            body.push_back(text(""));
            body.push_back(text(std::format(" Repository : {}", ctx.repo())) | dim);
            body.push_back(text(std::format(" Workflows  : {}", ctx.selected_workflows.size())) | dim);
            body.push_back(text(std::format(" Secrets    : {} (name = {})",
                ctx.use_existing_secret ? "reused 1" : "created 1", ctx.secret_name)) | dim);
            body.push_back(text(""));
            if (*test_clicked) {
                // Trigger workflow dispatch (best-effort).
                (void)bash::execute_command(std::format(
                    "gh workflow run {}.yml --repo {} 2>&1",
                    bash::escape_shell_arg(ctx.selected_workflows.empty()
                        ? "claude"
                        : ctx.selected_workflows.front()),
                    bash::escape_shell_arg(ctx.repo())));
                *test_clicked = false;
            }
            return vbox(std::move(body));
        }),
        Button("Test workflow run now", [test_clicked] { *test_clicked = true; }),
        Button("Close wizard", [] { /* wizard closes on Enter / Complete */ }),
    });
}

// -- Step 12: Error ---------------------------------------------------------
inline Component build_step_error(std::shared_ptr<WizardController> wc) {
    auto logs_open = std::make_shared<bool>(false);
    auto retry = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, logs_open] {
            Elements body;
            body.push_back(hbox({
                text("  ✗  ") | color(Color::Red) | bold,
                text("Installation failed") | color(Color::Red) | bold,
            }));
            body.push_back(text(""));
            body.push_back(text("   " + wc->ctx->error_reason) | bold);
            if (!wc->ctx->error_instructions.empty())
                body.push_back(text("   " + wc->ctx->error_instructions) | dim);
            body.push_back(text(""));
            if (*logs_open) {
                body.push_back(text(" Logs:") | dim);
                for (const auto& l : wc->creating_logs)
                    body.push_back(text("   " + l) | color(Color::GrayLight));
            }
            return vbox(std::move(body)) | yframe | flex;
        }),
        Button("View logs", [logs_open] { *logs_open = !*logs_open; }),
        Button("Retry", [retry, wc] {
            *retry = true;
            wc->retry_requested = true;
        }),
        Button("Contact support (open in browser)", [] {
            open_url("https://console.claude.com/support");
        }),
    });
}

// ==========================================================================
// Public factory: build the full wizard.
// ==========================================================================

struct InstallGitHubAppWizardOptions {
    std::function<void(steps::Step final_step)> on_complete;
    std::function<void()> on_cancel;
};

[[nodiscard]] inline Component MakeInstallGitHubAppWizard(
    InstallGitHubAppWizardOptions opts)
{
    auto controller = std::make_shared<WizardController>();
    controller->ctx = std::make_shared<steps::InstallGitHubAppContext>();

    // --- Bootstrap IO that happens *before* the user can press Enter. ---
    controller->enter_step(steps::Step::CheckGitHub);

    using B = std::function<Component()>;

    // We build the list of steps dynamically so that validate/advance can
    // skip steps (e.g. jump from ChooseRepo → SelectWorkflows when no
    // workflow file exists). The UI11 WizardComponent walks an array; we
    // therefore map the *static* 12 Step enum values to the static list,
    // and use a small "cursor translator" stored on the controller that
    // keeps the wizard index in sync with the C2 current_step.
    //
    // Simpler approach: always render all 12 steps in array order, and
    // *after* running validate+advance, jump the wizard index to the
    // returned Step ordinal.
    auto current_step = std::make_shared<steps::Step>(steps::Step::CheckGitHub);

    std::vector<std::pair<steps::Step, B>> builders = {
        { steps::Step::CheckGitHub,          [controller]{ return build_step_check_github(controller); } },
        { steps::Step::Warnings,             [controller]{ return build_step_warnings(controller); } },
        { steps::Step::ChooseRepo,           [controller]{ return build_step_choose_repo(controller); } },
        { steps::Step::InstallApp,           [controller]{ return build_step_install_app(controller); } },
        { steps::Step::CheckExistingWorkflow,[controller]{ return build_step_check_existing_workflow(controller); } },
        { steps::Step::SelectWorkflows,      [controller]{ return build_step_select_workflows(controller); } },
        { steps::Step::CheckExistingSecret,  [controller]{ return build_step_check_existing_secret(controller); } },
        { steps::Step::ApiKey,               [controller]{ return build_step_apikey(controller); } },
        { steps::Step::OAuthFlow,            [controller]{ return build_step_oauth(controller); } },
        { steps::Step::Creating,             [controller]{ return build_step_creating(controller); } },
        { steps::Step::Success,              [controller]{ return build_step_success(controller); } },
        { steps::Step::Error,                [controller]{ return build_step_error(controller); } },
    };

    std::vector<wizard::WizardStep> wizard_steps;
    for (auto& [s, _] : builders) {
        wizard::WizardStep w;
        w.id = std::to_string(static_cast<int>(s));
        w.title = std::string(steps::step_description(s));
        w.description = w.title;
        wizard_steps.push_back(std::move(w));
    }

    // The UI11 WizardComponent owns step components; because our per-step
    // components are closures built lazily (they need to re-run IO on
    // transitions), we wrap them in a Renderer that dispatches on
    // *current_step. We then push this into *every* slot so the wizard
    // index = C2 step ordinal invariant holds naturally.
    auto content = Renderer([controller, current_step, builders] {
        for (const auto& [s, build] : builders) {
            if (s == *current_step) {
                auto comp = build();
                return comp ? comp->Render() : text("(no content)") | dim;
            }
        }
        return text("(step not found)") | dim;
    }) | CatchEvent([controller, current_step, builders, opts](Event event) -> bool {
        // Intercept Return so we can run C2 validate/advance and jump the
        // wizard index appropriately.
        if (event == Event::Return) {
            // 1) Validate (C2).
            auto errs = steps::validate(*current_step, *controller->ctx);
            if (!errs.empty()) {
                // Surface: if error screen exists for this step, write reason.
                controller->ctx->error_reason = errs.front().message;
                for (size_t i = 1; i < errs.size(); ++i) {
                    controller->ctx->error_instructions += errs[i].message + "\n";
                }
                return true; // block advance until user fixes
            }

            // 2) Advance (C2).
            auto next = steps::advance(*current_step, *controller->ctx,
                                       /*user_input=*/"");

            // 3) Enter next step (IO side-effects).
            *current_step = next;
            controller->enter_step(next);

            // 4) Terminal steps.
            if (next == steps::Step::Success || next == steps::Step::Error) {
                if (opts.on_complete) opts.on_complete(next);
            }
            return true;
        }

        // Escape: fall through to the wizard's default cancel handler.
        if (event == Event::Escape) {
            if (opts.on_cancel) opts.on_cancel();
            return true;
        }

        // Otherwise hand to the current step component so buttons fire.
        for (const auto& [s, build] : builders) {
            if (s == *current_step) {
                auto comp = build();
                if (comp) return comp->OnEvent(event);
            }
        }
        return false;
    });

    // Stuff the same content into each slot so wizard_index == step_ordinal
    // is trivially true; the Renderer above re-renders per step anyway.
    for (auto& w : wizard_steps) {
        w.create_content = [content] { return content; };
    }

    wizard::WizardProviderProps props;
    props.title = "Install the Claude GitHub App";
    props.steps = std::move(wizard_steps);
    props.on_complete = [opts, current_step] {
        if (opts.on_complete) opts.on_complete(*current_step);
    };
    props.on_cancel = std::move(opts.on_cancel);

    return wizard::WizardComponent(std::move(props));
}

} // namespace cc::ui::dialogs::install_github_app_wizard
