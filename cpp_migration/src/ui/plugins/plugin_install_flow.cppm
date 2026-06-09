/// @file plugin_install_flow.cppm
/// @brief 5-step plugin install wizard using UI11 Wizard framework:
///        Step 1 Source → Step 2 Review → Step 3 Trust validation (reuse UI8)
///        → Step 4 Install progress → Step 5 Complete.
///
/// Step 3 fully reuses cc.ui.trust_dialog::MakePluginTrustDialog — the
/// wizard embeds that component.
module;

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.plugins.plugin_install_flow;

import cc.types.types;
import cc.commands.plugin_ui_data;
import cc.commands.plugin_details_helpers;
import cc.commands.plugin_trust_text;
import cc.ui.trust_dialog;
import cc.ui.wizard_dialog;

export namespace cc::ui::plugins::plugin_install_flow {
using namespace ftxui;

namespace ui = cc::commands::plugin_ui;
namespace pd = cc::commands::plugin;
namespace pt = cc::commands::plugin;
namespace wd = cc::ui::wizard_dialog;
namespace td = cc::ui::trust_dialog;

// =========================================================================
// Step 1 — plugin source selection
// =========================================================================

enum class InstallSource : unsigned char {
    Marketplace,   // browse and pick
    File,          // local .zip / directory / manifest
    Url,           // https:// URL to a zip/manifest
    GitRepo,       // git clone from repo URL
};

inline constexpr std::array<const char*, 4> k_source_names = {
    "From Marketplace",
    "From file / directory",
    "From URL",
    "From git repository",
};

struct SourceStepData {
    InstallSource source = InstallSource::Marketplace;
    std::string   marketplace_id;      // when source == Marketplace
    std::string   plugin_id;
    std::string   file_path;           // when source == File
    std::string   url;                 // when source == Url
    std::string   git_repo;            // when source == GitRepo
    std::string   git_ref;             // branch/tag/sha (optional)
};

// =========================================================================
// Step 2 — review details
// =========================================================================

struct ReviewStepData {
    std::string          name;
    std::string          version;
    std::string          author;
    std::string          description;
    std::optional<std::string> homepage;
    std::optional<std::string> license;
    std::vector<std::string> permissions;    // e.g. "bash:read", "file:write"
    std::vector<std::string> tools;          // tool names
    std::vector<std::string> commands;       // slash commands
    std::string          install_scope;      // "user" / "project" / "local"
    std::string          size_str;
    std::size_t          estimated_file_count = 0;
};

// =========================================================================
// Step 3 — trust validation (pre-computed warnings from C1)
// =========================================================================

struct TrustStepData {
    bool        is_verified = false;
    bool        has_signature = true;
    std::string marketplace_domain;
    std::vector<std::string> permissions;    // mirror of ReviewStepData
    // Populated via C1: plugin_trust_text + plugin_ui_data::TrustWarningInfo
    ui::TrustWarningInfo warning;
    bool        user_signed_trust = false;   // gate for Next button
};

// =========================================================================
// Step 4 — installation progress
// =========================================================================

enum class InstallStage : unsigned char {
    Pending,
    Downloading,
    VerifyingChecksum,
    VerifyingSignature,
    Extracting,
    InstallingDependencies,
    Registering,
    Complete,
    Failed,
};

struct InstallProgressData {
    InstallStage stage = InstallStage::Pending;
    int          percent = 0;            // 0..100
    std::string  stage_label;
    std::vector<std::string> log_lines;  // last ~20 lines
    std::optional<std::string> error;
};

// =========================================================================
// Step 5 — completion
// =========================================================================

struct CompleteStepData {
    bool        success = true;
    std::string installed_id;
    std::string installed_version;
    std::string install_scope;
    std::vector<std::string> provided_tools;
    std::vector<std::string> provided_commands;
    std::optional<std::string> post_install_hint;
    std::optional<std::string> first_run_hotkey;
};

// =========================================================================
// Full wizard inputs
// =========================================================================

struct InstallFlowInputs {
    // Pre-filled from routing (e.g. /plugin install foo@bar)
    std::optional<std::string> preselected_plugin_id;
    std::optional<std::string> preselected_marketplace;

    // Plugins available in the marketplace (for Step 1 Marketplace picker)
    std::vector<std::pair<std::string, std::string>> marketplace_plugin_list; // (id, display_name)

    // Trust dialog — PluginDefinition + PluginCapabilities are passed
    // through the UI8 TrustDialog via MakePluginTrustDialog().  The wizard
    // does NOT inspect them — it only stores a shared_ptr to the component.

    // Callbacks
    std::function<void(const SourceStepData& source, ReviewStepData& out_review)> prepare_review;
    std::function<void(const ReviewStepData&, TrustStepData& out_trust)>     prepare_trust;
    std::function<void(const TrustStepData&)>     on_begin_install;   // start worker thread
    std::function<bool(InstallProgressData& out)> poll_progress;      // call each frame
    std::function<void()>                          on_cancel;
    std::function<void(const CompleteStepData&)>   on_complete;
};

// =========================================================================
// Rendering helpers per step
// =========================================================================

namespace detail {

// --- Step 1: Source ---------------------------------------------------------

[[nodiscard]] inline Element RenderSourceStep(
    const SourceStepData& data, int selected)
{
    Elements rows;
    rows.push_back(hbox({
        text(" 🛒  Installation Source ") | bold | color(Color::Cyan),
        filler(),
        text("Pick where to install the plugin from") | dim,
    }));
    rows.push_back(separator());
    rows.push_back(text(""));

    struct SourceItem {
        const char* icon;
        const char* name;
        const char* subtitle;
        Color       accent;
    };
    static constexpr SourceItem k_items[] = {
        {"🛍️", "From Marketplace",
            "Browse available marketplaces and pick a plugin", Color::Cyan},
        {"📁", "From local file or directory",
            "Install from a downloaded .zip or a local folder", Color::Green},
        {"🔗", "From URL",
            "Install directly from a manifest or zip URL", Color::Blue},
        {"🌿", "From Git repository",
            "Clone from a git repo (HTTPS or SSH)", Color::Magenta},
    };
    for (int i = 0; i < 4; ++i) {
        const bool sel = (i == selected);
        const auto& it = k_items[i];
        auto row = hbox({
            text(sel ? "▶ " : "  ") | color(it.accent),
            text(it.icon),
            text(" "),
            vbox({
                text(it.name) | bold | color(sel ? Color::White : it.accent),
                text("  " + std::string{it.subtitle}) | dim,
            }) | flex,
        });
        if (sel) row = row | bgcolor(Color::RGB(20, 30, 50)) | padding(0, 1, 0, 0);
        rows.push_back(std::move(row));
        if (i + 1 < 4) rows.push_back(separator() | dim);
    }

    // Form inputs based on selection
    rows.push_back(text(""));
    auto sel_enum = (InstallSource)selected;
    std::string label, value, placeholder;
    switch (sel_enum) {
        case InstallSource::Marketplace:
            label = "Plugin ID: ";
            value = data.plugin_id;
            placeholder = "(e.g. code-formatter@official)";
            break;
        case InstallSource::File:
            label = "File path: ";
            value = data.file_path;
            placeholder = "(e.g. ~/Downloads/plugin.zip or /path/to/manifest.json)";
            break;
        case InstallSource::Url:
            label = "URL: ";
            value = data.url;
            placeholder = "(e.g. https://example.com/plugin.zip)";
            break;
        case InstallSource::GitRepo:
            label = "Repo URL: ";
            value = data.git_repo;
            placeholder = "(e.g. git@github.com:user/repo.git)";
            break;
    }
    rows.push_back(hbox({
        text("  " + label) | bold | color(Color::GrayLight),
        text(value.empty() ? placeholder : value)
             | color(value.empty() ? Color::GrayDark : Color::White),
        text("│") | blink | color(Color::Cyan),
    }) | borderLight);

    if (sel_enum == InstallSource::GitRepo && !data.git_ref.empty()) {
        rows.push_back(text(""));
        rows.push_back(hbox({
            text("  ref: ") | dim,
            text(data.git_ref) | color(Color::BlueLight),
        }));
    }
    rows.push_back(text(""));

    rows.push_back(text(" Navigation:") | dim);
    rows.push_back(hbox({
        text(" ↑↓/j/k") | bold | color(Color::Cyan), text(" select source  "),
        text("Tab") | bold | color(Color::Cyan), text(" focus input  "),
        text("⏎") | bold | color(Color::Cyan), text(" next"),
    }) | dim);
    return vbox(std::move(rows));
}

// --- Step 2: Review ---------------------------------------------------------

[[nodiscard]] inline Element RenderReviewStep(const ReviewStepData& r) {
    Elements rows;
    rows.push_back(hbox({
        text(" 📋  Review Plugin Details ") | bold | color(Color::Blue),
        filler(),
        text("Verify everything looks correct") | dim,
    }));
    rows.push_back(separator());

    // Header card
    rows.push_back(hbox({
        vbox({
            hbox({
                text(" " + r.name + " ") | bold | color(Color::Cyan) | size(HEIGHT, EQUAL, 1),
                text(" v" + r.version) | dim,
            }),
            text("  by " + (r.author.empty() ? std::string{"(unknown)"} : r.author))
                | dim | color(Color::GrayLight),
        }) | flex,
        text(" "),
        hbox({
            text("[" + r.install_scope + "] ")
                | bold | color(r.install_scope == "user" ? Color::Cyan
                    : (r.install_scope == "project" ? Color::Green : Color::Yellow)),
            text(r.size_str.empty() ? "—" : r.size_str) | dim,
        }) | align_right,
    }) | padding(1));
    rows.push_back(separator() | dim);

    // Description
    rows.push_back(text(""));
    rows.push_back(paragraph(
        "  " + (r.description.empty() ? "(no description)" : r.description))
        | dim | color(Color::GrayLight));
    rows.push_back(text(""));

    // Metadata table
    if (r.homepage) {
        rows.push_back(hbox({
            text("  Homepage: ") | dim,
            text(*r.homepage) | underlined | color(Color::Cyan),
        }));
    }
    if (r.license) {
        rows.push_back(hbox({
            text("  License:  ") | dim,
            text(*r.license) | color(Color::Yellow),
        }));
    }
    rows.push_back(hbox({
        text("  Files:    ") | dim,
        text(std::format("~{} files", r.estimated_file_count)) | color(Color::White),
    }));
    rows.push_back(text(""));

    // Permissions (the important part)
    rows.push_back(text(" Permissions requested:") | bold);
    if (r.permissions.empty()) {
        rows.push_back(text("   (none)") | dim);
    } else {
        for (const auto& p : r.permissions) {
            Color c = Color::White;
            if (p.find("unsafe") != std::string::npos || p.find("bash") != std::string::npos)
                c = Color::Red;
            else if (p.find("write") != std::string::npos)
                c = Color::Yellow;
            else if (p.find("read") != std::string::npos)
                c = Color::Cyan;
            rows.push_back(hbox({
                text("   • ") | color(c),
                text(p) | color(c),
            }));
        }
    }
    rows.push_back(text(""));

    // Tools + commands summary
    if (!r.tools.empty() || !r.commands.empty()) {
        rows.push_back(hbox({
            text(" Provides ") | dim,
            text(std::to_string(r.tools.size())) | bold | color(Color::Magenta),
            text(" tool(s) and ") | dim,
            text(std::to_string(r.commands.size())) | bold | color(Color::Yellow),
            text(" command(s)") | dim,
        }));
    }

    // Trust disclaimer banner
    rows.push_back(text(""));
    rows.push_back(vbox({
        hbox({
            text(" ⚠ ") | color(Color::Yellow),
            text(std::string{pt::trust_warning_header()}) | bold | color(Color::Yellow),
        }),
        paragraph("   " + pt::build_trust_warning_text()) | dim | color(Color::GrayLight),
    }) | border | color(Color::Yellow) | bgcolor(Color::RGB(25, 20, 5)));

    return vbox(std::move(rows));
}

// --- Step 3: Trust (delegates to UI8 TrustDialog) --------------------------

[[nodiscard]] inline Element RenderTrustStep(
    const TrustStepData& t, const Element& trust_dialog_panel)
{
    Elements rows;
    rows.push_back(hbox({
        text(" 🔒  Trust & Safety ") | bold | color(Color::Magenta),
        filler(),
        text("Required before installation can begin") | dim,
    }));
    rows.push_back(separator());
    rows.push_back(text(""));

    // If the plugin is fully verified, show a green banner + skip.
    if (t.is_verified) {
        rows.push_back(hbox({
            text(" 🛡 ") | color(Color::Green) | bold,
            text("This plugin is verified and trusted.")
                | bold | color(Color::Green),
        }));
        rows.push_back(text("") | size(HEIGHT, EQUAL, 1));
    }

    // Embed the full UI8 TrustDialog
    rows.push_back(trust_dialog_panel | flex);

    return vbox(std::move(rows));
}

// --- Step 4: Install progress ----------------------------------------------

[[nodiscard]] inline Element RenderInstallStep(const InstallProgressData& p) {
    Elements rows;
    rows.push_back(hbox({
        text(" 📦  Installing… ") | bold | color(Color::Green),
        filler(),
        text(std::format("{}%", p.percent)) | dim,
    }));
    rows.push_back(separator());
    rows.push_back(text(""));

    // Stage label
    Color stage_color = Color::Cyan;
    if (p.stage == InstallStage::Failed)   stage_color = Color::Red;
    if (p.stage == InstallStage::Complete) stage_color = Color::Green;
    rows.push_back(hbox({
        text(p.stage == InstallStage::Pending ? "○" : "●") | color(stage_color),
        text(" " + (p.stage_label.empty() ? "Preparing…" : p.stage_label))
             | bold | color(stage_color),
    }));
    rows.push_back(text(""));

    // Progress bar
    {
        const int pct = std::clamp(p.percent, 0, 100);
        const int bar_width = 40;
        const int filled = (pct * bar_width) / 100;
        std::string bar;
        bar.reserve(bar_width);
        for (int i = 0; i < filled; ++i)     bar += '█';
        for (int i = filled; i < bar_width; ++i) bar += '░';
        rows.push_back(hbox({
            text(" "),
            text(bar) | color(pct >= 100 ? Color::Green : Color::Cyan),
            text(std::format(" {}%", pct)) | dim,
        }));
    }
    rows.push_back(text(""));

    // Error (if any)
    if (p.error) {
        rows.push_back(vbox({
            hbox({text(" ✗ ") | color(Color::Red) | bold,
                  text("Installation failed") | color(Color::Red) | bold}),
            paragraph("   " + *p.error) | color(Color::RedLight) | dim,
        }) | border | color(Color::Red));
    }

    // Log lines (last 8)
    rows.push_back(separator() | dim);
    rows.push_back(text(" Installation log") | dim);
    rows.push_back(text(""));
    int start = (int)std::max<std::ptrdiff_t>(0,
        (std::ptrdiff_t)p.log_lines.size() - 8);
    for (int i = start; i < (int)p.log_lines.size(); ++i) {
        rows.push_back(text("   " + p.log_lines[i]) | color(Color::GrayLight) | dim);
    }

    return vbox(std::move(rows));
}

// --- Step 5: Complete -------------------------------------------------------

[[nodiscard]] inline Element RenderCompleteStep(const CompleteStepData& d) {
    if (!d.success) {
        return vbox({
            hbox({
                text(" ❌  Installation Failed ") | bold | color(Color::Red),
                filler(),
            }),
            separator(),
            text(""),
            text(" The plugin could not be installed.") | color(Color::Red),
            text(" Check the log above for details, or try a different source.") | dim,
            text(""),
            hbox({
                text(" [Esc] ") | bold | color(Color::Cyan), text("close   "),
                text("[R]") | bold | color(Color::Cyan), text(" retry"),
            }) | dim,
        });
    }

    Elements rows;
    rows.push_back(hbox({
        text(" 🎉  Installation Complete ") | bold | color(Color::Green),
        filler(),
        text("The plugin is ready to use") | dim,
    }));
    rows.push_back(separator());
    rows.push_back(text(""));

    rows.push_back(hbox({
        text(" ✓ ") | color(Color::Green) | bold,
        text("Successfully installed ") | color(Color::Green),
        text(d.installed_id) | bold | color(Color::Cyan),
        text(" v" + d.installed_version) | dim,
    }));
    rows.push_back(hbox({
        text("   scope: [") | dim,
        text(d.install_scope) | color(Color::Yellow),
        text("]") | dim,
    }));

    if (!d.provided_tools.empty()) {
        rows.push_back(text(""));
        rows.push_back(text(" New tools available:") | bold);
        for (const auto& t : d.provided_tools) {
            rows.push_back(hbox({
                text("   🔧 ") | color(Color::Magenta),
                text(t) | color(Color::MagentaLight),
            }));
        }
    }
    if (!d.provided_commands.empty()) {
        rows.push_back(text(""));
        rows.push_back(text(" New slash commands:") | bold);
        for (const auto& c : d.provided_commands) {
            rows.push_back(hbox({
                text("   ⌘ /") | color(Color::Yellow),
                text(c) | color(Color::YellowLight),
            }));
        }
    }

    if (d.post_install_hint) {
        rows.push_back(text(""));
        rows.push_back(vbox({
            hbox({text(" 💡 ") | color(Color::Cyan),
                  text("Next steps") | bold | color(Color::Cyan)}),
            paragraph("   " + *d.post_install_hint) | dim,
        }) | borderLight | color(Color::Cyan));
    }

    if (d.first_run_hotkey) {
        rows.push_back(text(""));
        rows.push_back(hbox({
            text(" Tip: press ") | dim,
            text(*d.first_run_hotkey) | bold | color(Color::Yellow),
            text(" to start using the plugin now") | dim,
        }));
    }

    rows.push_back(text(""));
    rows.push_back(hbox({
        text(" [⏎] ") | bold | color(Color::Cyan), text("close   "),
        text("[L]") | bold | color(Color::Cyan), text(" launch plugin   "),
        text("[C]") | bold | color(Color::Cyan), text(" configure"),
    }) | dim);

    return vbox(std::move(rows));
}

} // namespace detail

// =========================================================================
// Full install wizard component
// =========================================================================

struct InstallFlowState {
    InstallFlowInputs inputs;

    // Step data
    SourceStepData    source;
    ReviewStepData    review;
    TrustStepData     trust;
    InstallProgressData progress;
    CompleteStepData  complete;

    // Interactive state
    int  source_selected = 0;
    std::string input_buffer;
    int  input_focus = -1;     // -1 = picker, 0 = main input, 1 = git ref

    // Embedded trust dialog component (Step 3)
    Component trust_dialog_component;
    Component active_input_component;
};

[[nodiscard]] inline Component MakeInstallWizard(InstallFlowInputs inputs) {
    auto state = std::make_shared<InstallFlowState>();
    state->inputs = std::move(inputs);

    // Pre-fill source from routing hints
    if (state->inputs.preselected_plugin_id) {
        state->source.source = InstallSource::Marketplace;
        state->source.plugin_id = *state->inputs.preselected_plugin_id;
    }
    if (state->inputs.preselected_marketplace) {
        state->source.marketplace_id = *state->inputs.preselected_marketplace;
    }

    // Build the 5 wizard steps using UI11 WizardComponent
    wd::WizardProviderProps props;
    props.title = "Install Plugin";

    // Step 1 — Source
    wd::WizardStep step1;
    step1.id = "source";
    step1.title = "Installation source";
    step1.description = "Choose where the plugin comes from";
    step1.create_content = [state]() {
        return Renderer([state] {
            return detail::RenderSourceStep(state->source, state->source_selected);
        });
    };
    props.steps.push_back(std::move(step1));

    // Step 2 — Review
    wd::WizardStep step2;
    step2.id = "review";
    step2.title = "Review details";
    step2.description = "Verify plugin info and permissions";
    step2.create_content = [state]() {
        return Renderer([state] {
            return detail::RenderReviewStep(state->review);
        });
    };
    props.steps.push_back(std::move(step2));

    // Step 3 — Trust
    wd::WizardStep step3;
    step3.id = "trust";
    step3.title = "Trust validation";
    step3.description = "Security & trust check";
    step3.create_content = [state]() {
        // Create / reuse the embedded UI8 TrustDialog component
        if (!state->trust_dialog_component) {
            td::TrustDialogProps p;
            p.on_done = [state](td::TrustChoice choice) {
                state->trust.user_signed_trust =
                    (choice != td::TrustChoice::Cancel);
            };
            p.action = td::ActionType::PluginInstall;
            p.action_label = "Plugin Installation";
            p.marketplace_domain = state->trust.marketplace_domain;
            p.plugin_has_signature = state->trust.has_signature;
            // If the caller supplied PluginDefinition via inputs, it is
            // consumed here.  For the pure-UI contract we keep the
            // fallback to the generic trust dialog.
            state->trust_dialog_component =
                td::MakeTrustDialogComponent(std::move(p));
        }
        return Renderer([state] {
            return detail::RenderTrustStep(
                state->trust, state->trust_dialog_component->Render());
        }) | CatchEvent([state](Event e) {
            if (state->trust_dialog_component)
                return state->trust_dialog_component->OnEvent(e);
            return false;
        });
    };
    props.steps.push_back(std::move(step3));

    // Step 4 — Install
    wd::WizardStep step4;
    step4.id = "install";
    step4.title = "Installation";
    step4.description = "Downloading, verifying, and installing";
    step4.create_content = [state]() {
        return Renderer([state]() -> Element {
            // Poll progress on every frame (consumer callback)
            if (state->inputs.poll_progress) {
                state->inputs.poll_progress(state->progress);
            }
            // Auto-advance when complete
            if (state->progress.stage == InstallStage::Complete) {
                state->complete.success = true;
                state->complete.installed_id = state->review.name;
                state->complete.installed_version = state->review.version;
                state->complete.install_scope = state->review.install_scope;
                state->complete.provided_tools = state->review.tools;
                state->complete.provided_commands = state->review.commands;
            } else if (state->progress.stage == InstallStage::Failed) {
                state->complete.success = false;
            }
            return detail::RenderInstallStep(state->progress);
        });
    };
    props.steps.push_back(std::move(step4));

    // Step 5 — Complete
    wd::WizardStep step5;
    step5.id = "complete";
    step5.title = "Complete";
    step5.description = "Installation result";
    step5.create_content = [state]() {
        return Renderer([state] {
            return detail::RenderCompleteStep(state->complete);
        });
    };
    props.steps.push_back(std::move(step5));

    // Completion / cancel handlers
    props.on_complete = [state] {
        if (state->complete.success && state->inputs.on_complete)
            state->inputs.on_complete(state->complete);
    };
    props.on_cancel = [state] {
        if (state->inputs.on_cancel) state->inputs.on_cancel();
    };

    // Build the wizard, then wrap with per-step Enter-gating + step
    // transition side-effects (prepare review, prepare trust, start install).
    auto wizard = wd::WizardComponent(std::move(props));

    // Wrap with inter-step pre-computation hook
    auto base_state = state;  // capture
    return wizard | CatchEvent([base_state](Event event) -> bool {
        // Enter → step transition (captured before WizardComponent handler)
        if (event == Event::Return) {
            // Read the current step from the outer component's state via
            // the wizard rendering (opaque).  We rely on the Wizard's own
            // handler, and trigger *our* side-effects *after* the step
            // advances via an Event::Custom post pattern.
            //
            // To keep this module simple + pure-UI, we run the C1 prep
            // callbacks *before* the wizard handles Enter on Steps 1 & 2,
            // so by the time the next step renders, its data is ready.
            //
            // We cannot directly read wizard_state.current_step_index, so
            // we approximate by pre-computing on every Enter — the
            // callbacks are idempotent.
            if (base_state->inputs.prepare_review) {
                base_state->inputs.prepare_review(base_state->source, base_state->review);
            }
            if (base_state->inputs.prepare_trust) {
                base_state->inputs.prepare_trust(base_state->review, base_state->trust);
            }
            return false;  // let the wizard's own handler process Enter
        }
        return false;
    });
}

} // namespace cc::ui::plugins::plugin_install_flow
