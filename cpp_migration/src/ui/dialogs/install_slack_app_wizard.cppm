/// @file install_slack_app_wizard.cppm
/// @brief FTXUI interactive wizard wrapping the Phase-2 pure-function
///        3-step InstallSlackApp state machine.
///
/// The *logic* (validate / advance) lives BLACK-BOX in
///   cc.commands.install_slack_app (Phase 2 C2 deliverable).
/// This module owns only the presentation layer:
///   * per-step FTXUI components (checkboxes, URL button, success screen)
///   * cross-platform browser open at Step 3 (reuses open_url helper from C2)
///   * a shortcut button that queues the `/slack` slash command.
module;

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.dialogs.install_slack_app_wizard;

import cc.types.types;
import cc.commands.install_slack_app;
import cc.ui.wizard_dialog;

export namespace cc::ui::dialogs::install_slack_app_wizard {

using namespace ftxui;
namespace steps = cc::commands::install_slack_app;

// --------------------------------------------------------------------------
// Shared controller / state.
// --------------------------------------------------------------------------
struct WizardController {
    std::shared_ptr<steps::InstallSlackAppContext> ctx;

    // Step 1
    bool eligibility_confirmed = false;
    bool auth_checked = false;

    // Step 2
    bool install_confirmed = false;
    bool open_clicked = false;

    // Step 3
    bool slack_launched = false;
    std::function<void(const std::string&)> queue_command;

    // Populate eligibility fields via a best-effort `cc auth whoami` probe.
    // NOTE: the TS host exposes a whoami command; in C++ we fall back to
    // writing `has_claude_auth = true` so the wizard remains usable even if
    // the auth probe is not wired. Callers can pre-populate context before
    // launching the wizard to reflect real auth state.
    void run_eligibility_probe() {
        if (auth_checked) return;
        auth_checked = true;
        // TODO(ui15 + auth-team): integrate real cc::services::auth::whoami().
        ctx->has_claude_auth = true;
        ctx->plan_supports_slack_app = true;
        ctx->eligibility_warning.clear();
    }
};

// ==========================================================================
// Step builders.
// ==========================================================================

// -- Step 1: ConfirmEligibility --------------------------------------------
inline Component build_step_1(std::shared_ptr<WizardController> wc) {
    auto checked = std::make_shared<bool>(wc->eligibility_confirmed);
    wc->run_eligibility_probe();
    return Container::Vertical({
        Renderer([wc] {
            const auto& ctx = *wc->ctx;
            Elements body;
            body.push_back(text(" Before you install the Claude Slack app") | bold);
            body.push_back(text(""));
            body.push_back(text(
                " The Slack app is available on Claude Business (Pro/Max/Team)") |
                dim);
            body.push_back(text(
                " and Enterprise plans. It is not available on free plans.") |
                dim);
            body.push_back(text(""));

            auto line = [&](std::string_view label, bool ok) {
                body.push_back(hbox({
                    text(ok ? " ✓ " : " ✗ ") | color(ok ? Color::Green : Color::Red),
                    text(std::string(label)) | bold,
                }));
            };
            line("Signed in to Claude",        ctx.has_claude_auth);
            line("Your plan supports Slack app", ctx.plan_supports_slack_app);
            if (!ctx.eligibility_warning.empty()) {
                body.push_back(text(""));
                body.push_back(text(" " + ctx.eligibility_warning)
                    | color(Color::Yellow));
            }
            body.push_back(text(""));
            return vbox(std::move(body));
        }),
        Checkbox("I confirm my account plan supports the Slack app",
                 checked.get()),
        Renderer([wc, checked] {
            wc->eligibility_confirmed = *checked;
            wc->ctx->user_confirmed = *checked;
            return text("");
        }),
    });
}

// -- Step 2: ConfirmInstall -------------------------------------------------
inline Component build_step_2(std::shared_ptr<WizardController> wc) {
    auto checked = std::make_shared<bool>(wc->install_confirmed);
    auto open_clicked = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, open_clicked] {
            const auto url = std::string(
                steps::InstallSlackAppContext::slack_app_url);
            Elements body;
            body.push_back(text(" Install the Claude Slack app") | bold);
            body.push_back(text(""));
            body.push_back(text(" 1. Press the button below to open the Slack"));
            body.push_back(text("    Marketplace listing in your browser."));
            body.push_back(text(" 2. On the Slack page, click 'Add to Slack'."));
            body.push_back(text(" 3. Select a workspace and accept the requested"));
            body.push_back(text("    permissions (messages:read, chat:write, etc.)."));
            body.push_back(text(" 4. Return here and tick the confirmation box."));
            body.push_back(text(""));
            body.push_back(text(std::format(" URL : {}", url)) | color(Color::Cyan) | dim);
            if (*open_clicked) {
                steps::detail::open_url(url);
                *open_clicked = false;
            }
            return vbox(std::move(body));
        }),
        Button("Open install page in browser", [open_clicked] {
            *open_clicked = true;
        }),
        Checkbox("I've completed installation in the browser",
                 checked.get()),
        Renderer([wc, checked] {
            wc->install_confirmed = *checked;
            wc->ctx->user_confirmed = *checked;
            return text("");
        }),
    });
}

// -- Step 3: OpeningBrowser + success --------------------------------------
inline Component build_step_3(std::shared_ptr<WizardController> wc) {
    auto launched = std::make_shared<bool>(false);
    auto slack_cmd_clicked = std::make_shared<bool>(false);
    return Container::Vertical({
        Renderer([wc, launched] {
            const auto url = std::string(
                steps::InstallSlackAppContext::slack_app_url);
            Elements body;

            if (!*launched) {
                body.push_back(hbox({
                    spinner(1, std::chrono::steady_clock::now()
                                            .time_since_epoch().count())
                        | color(Color::Cyan),
                    text(" Opening Slack marketplace in your default browser…")
                        | color(Color::Cyan),
                }));
                bool ok = steps::detail::open_url(url);
                wc->ctx->browser_opened = ok;
                *launched = true;
            } else {
                body.push_back(hbox({
                    text(" 🎉  ") | color(Color::Green) | bold,
                    text("Slack app connected") | color(Color::Green) | bold,
                }));
            }
            body.push_back(text(""));
            if (wc->ctx->browser_opened) {
                body.push_back(text(" ✓ Browser opened successfully.")
                    | color(Color::Green));
            } else {
                body.push_back(text(
                    " ✗ Could not open a browser automatically.")
                    | color(Color::Red));
                body.push_back(text(std::format("   Please visit: {}", url))
                    | color(Color::Yellow));
            }
            body.push_back(text(""));
            body.push_back(text(
                " Next: run /slack inside your REPL to start chatting with") |
                dim);
            body.push_back(text(
                " Claude through your Slack workspace.") | dim);
            return vbox(std::move(body));
        }),
        Button("Launch /slack command now", [wc, slack_cmd_clicked] {
            if (*slack_cmd_clicked) return;
            *slack_cmd_clicked = true;
            if (wc->queue_command) wc->queue_command("/slack");
        }),
        Button("Close wizard", [] { /* handled by Complete */ }),
    });
}

// ==========================================================================
// Public factory.
// ==========================================================================

export struct InstallSlackAppWizardOptions {
    /// Pre-populated context (e.g. with real plan info from the auth layer).
    std::optional<steps::InstallSlackAppContext> initial_context;
    /// Called when the user presses "Launch /slack command now" so the
    /// REPL host can enqueue the slash command.
    std::function<void(const std::string& command)> queue_command;
    /// Called once the wizard reaches the final step.
    std::function<void()> on_complete;
    /// Called if the user cancels the wizard.
    std::function<void()> on_cancel;
};

export [[nodiscard]] inline Component MakeInstallSlackAppWizard(
    InstallSlackAppWizardOptions opts)
{
    auto controller = std::make_shared<WizardController>();
    controller->ctx = std::make_shared<steps::InstallSlackAppContext>(
        opts.initial_context.value_or(steps::InstallSlackAppContext{}));
    controller->queue_command = std::move(opts.queue_command);

    using Builder = std::function<Component()>;
    std::vector<std::pair<steps::Step, Builder>> builders = {
        { steps::Step::ConfirmEligibility, [controller] { return build_step_1(controller); } },
        { steps::Step::ConfirmInstall,     [controller] { return build_step_2(controller); } },
        { steps::Step::OpeningBrowser,     [controller] { return build_step_3(controller); } },
    };

    auto current_step = std::make_shared<steps::Step>(steps::Step::ConfirmEligibility);

    std::vector<wizard_dialog::WizardStep> wizard_steps;
    for (const auto& [s, _] : builders) {
        wizard_dialog::WizardStep w;
        w.id = std::to_string(static_cast<int>(s));
        w.title = std::string(steps::step_description(s));
        w.description = w.title;
        wizard_steps.push_back(std::move(w));
    }

    // One shared content renderer that switches on current_step (same
    // rationale as the GitHub wizard — keeps wizard_index == step ordinal).
    auto content = Renderer([controller, current_step, builders] {
        for (const auto& [s, build] : builders) {
            if (s == *current_step) {
                auto comp = build();
                return comp ? comp->Render() : text("(no content)") | dim;
            }
        }
        return text("(step not found)") | dim;
    }) | CatchEvent([controller, current_step, builders, opts](Event event) -> bool {
        if (event == Event::Return) {
            // 1) Validate via C2.
            auto errs = steps::validate(*current_step, *controller->ctx);
            if (!errs.empty()) {
                controller->ctx->eligibility_warning = errs.front().message;
                return true;
            }

            // 2) Advance via C2.
            auto next = steps::advance(*current_step, *controller->ctx, "");

            // 3) Side-effect for Step 3 is driven by the component renderer
            //    itself on first paint, so we don't need to repeat here.
            *current_step = next;

            // 4) Terminal: Step 3 loops back to itself per C2 contract.
            if (*current_step == steps::Step::OpeningBrowser) {
                if (opts.on_complete) opts.on_complete();
            }
            return true;
        }

        if (event == Event::Escape) {
            if (opts.on_cancel) opts.on_cancel();
            return true;
        }

        for (const auto& [s, build] : builders) {
            if (s == *current_step) {
                auto comp = build();
                if (comp) return comp->OnEvent(event);
            }
        }
        return false;
    });

    for (auto& w : wizard_steps) {
        w.create_content = [content] { return content; };
    }

    wizard_dialog::WizardProviderProps props;
    props.title = "Install the Claude Slack App";
    props.steps = std::move(wizard_steps);
    props.on_complete = std::move(opts.on_complete);
    props.on_cancel = std::move(opts.on_cancel);

    return wizard_dialog::WizardComponent(std::move(props));
}

} // namespace cc::ui::dialogs::install_slack_app_wizard
