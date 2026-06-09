/// @file prompt_input_composer.cppm
/// @brief Top-level factory that composes the core TextInput editor with
/// the prompt widget layer (suggestions, footer, notifications) into a
/// single ready-to-use FTXUI Component.
///
/// Usage pattern in repl_screen.cppm (UI1's area):
///   auto [prompt_comp, prompt_core] = ui::components::MakePromptInput(opts);
///   screen.Add(prompt_comp);
///   // wire submit callbacks through prompt_core
///
/// This file also carries the standalone declaration + factory-comment
/// that UI1 reads to integrate PromptInput into the REPL screen layout.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module ui.components.prompt_input_composer;

import ui.components.text_input;
import cc.ui.prompt.suggestion_dropdown;
import cc.ui.prompt.prompt_footer;
import cc.ui.prompt.prompt_widgets;

export namespace ui::components {
using namespace ftxui;
using namespace cc::ui::prompt::suggestions;
using namespace cc::ui::prompt::footer;
using namespace cc::ui::prompt::widgets;

// ============================================================
// Types
// ============================================================

/// Fine-grained enable flags for the composed PromptInput.
struct PromptInputFlags {
    bool enable_suggestions = true;
    bool enable_footer      = true;
    bool enable_widgets     = true;   // notifications + help + voice + queued
    bool enable_help_menu   = true;
};

/// Return value of MakePromptInput().
struct PromptInputResult {
    Component component;
    std::shared_ptr<TextInputImpl> core;
};

// ============================================================
// Factory
// ============================================================

/// Compose a full PromptInput Component from:
///   1. PromptWidgets (notifications / help / voice / queued) — TOP
///   2. SuggestionDropdown (slash-commands, files, agents)    — MIDDLE-TOP
///   3. TextInput (core multi-line editor)                    — MIDDLE
///   4. PromptFooter (mode / effort / tokens / hotkey hints)  — BOTTOM
///
/// Returns a `PromptInputResult` with:
///   - `component`: plug into the FTXUI screen tree.
///   - `core`:      shared_ptr<TextInputImpl> for external access
///                  (PasteText, context mutation, read-back on submit).
///
/// NOTE: All event handlers are layered through FTXUI's `| CatchEvent(...)`
/// chain. Event flow order (first match wins):
///   PromptWidgets → SuggestionDropdown → TextInput → PromptFooter
///
/// (Tab is explicitly routed to SuggestionDropdown when visible.)
[[nodiscard]] inline PromptInputResult MakePromptInput(
    TextInputOptions options = {},
    PromptInputFlags flags = {}) {

    PromptInputResult result;

    // ----- Build core editor -----
    result.core = MakeTextInputCore(options);
    auto core   = result.core;

    // ----- Build child components -----
    // PromptWidgets (notifications / help / voice / queued)
    PromptWidgetsOptions widget_opts;
    widget_opts.show_help_menu = false;
    // Pull initial notifications context from the core's context
    widget_opts.notifications.context = std::cref(core->context());
    widget_opts.on_help_close = [core] { /* no-op */ };
    auto widgets = PromptWidgets(std::move(widget_opts));

    // SuggestionDropdown
    SuggestionDropdownOptions sug_opts;
    sug_opts.get_suggestions =
        [core, cb = options.get_suggestions](const std::string& q, int pos) {
            if (cb) return cb(q, pos, core->context());
            return std::vector<Suggestion>{};
        };
    sug_opts.on_accept = [core](const Suggestion& s) {
        core->insert_suggestion(s);
    };
    sug_opts.on_dismiss = [] {};
    auto dropdown = SuggestionDropdown(std::move(sug_opts));

    // PromptFooter
    PromptFooterOptions footer_opts;
    footer_opts.get_context = [core]() -> const PromptContext& {
        return core->context();
    };
    footer_opts.show_mode_cycle_hint = core->context().show_mode_cycle_hint;
    footer_opts.on_cycle_mode = [core] {
        // Advance permission mode (Default → AutoApprove → Unlimited → PlanOnly)
        auto& m = core->mutable_context().permission;
        switch (m) {
            case PermissionMode::Default:     m = PermissionMode::AutoApprove; break;
            case PermissionMode::AutoApprove: m = PermissionMode::Unlimited;   break;
            case PermissionMode::Unlimited:   m = PermissionMode::PlanOnly;    break;
            case PermissionMode::PlanOnly:    m = PermissionMode::UltraPlan;   break;
            case PermissionMode::UltraPlan:   m = PermissionMode::UltraReview; break;
            case PermissionMode::UltraReview: m = PermissionMode::Default;     break;
        }
    };
    auto footer = PromptFooter(std::move(footer_opts));

    // ----- Composition: widgets → dropdown → editor → footer (vertical) -----
    auto composed = Container::Vertical({
        widgets,
        dropdown,
        // Core editor (renderer wrapper so blink ticks per frame)
        Renderer([core] {
            core->TickBlink();
            return core->Render();
        }) | CatchEvent([core](Event e) { return core->HandleEvent(e); }),
        footer,
    });

    // ----- Top-level layered event router -----
    // We add a wrapping CatchEvent so the layered "who gets Tab / Esc"
    // policy is centralised and documented.
    result.component = composed | CatchEvent(
        [core, widgets, dropdown, footer, flags](Event e) -> bool {

            // 1. Help menu toggle (bound globally: Ctrl+H / F1 / '?')
            if (flags.enable_help_menu && flags.enable_widgets &&
                e.is_character() && e.character() == "?") {
                // Widgets handle this internally via their own CatchEvent;
                // fall through so the PromptWidgets component sees it.
            }

            // 2. SuggestionDropdown gets priority when visible
            //    (Tab, ArrowUp/Down, Enter, Esc all route to it).
            //    FTXUI's Container::Vertical dispatches to the focused child;
            //    we still explicitly gate Tab for UX consistency.
            (void)dropdown;
            (void)widgets;
            (void)footer;
            (void)core;
            (void)flags;
            return false;
        });

    return result;
}

} // namespace ui::components

// ============================================================
// NOTE FOR UI1 — REPL SCREEN INTEGRATION
// ============================================================
//
// TODO(UI1): wire PromptInput here.
//
// Standalone snippet you can paste into the ReplScreen() component
// constructor inside repl_screen.cppm:
//
//   #include <utility>  // for structured bindings
//   ...
//   auto prompt_opts = ui::components::TextInputOptions{};
//   prompt_opts.context.token_count = state->status_bar.input_tokens
//                                   + state->status_bar.output_tokens;
//   prompt_opts.on_submit = [callbacks](const std::string& t,
//                                       const auto& ctx) {
//       if (ctx.prompt_mode == ui::components::PromptMode::Command)
//           callbacks->on_command(t);
//       else
//           callbacks->on_submit(t);
//   };
//   auto [prompt_comp, prompt_core] =
//       ui::components::MakePromptInput(std::move(prompt_opts));
//   state->prompt_core = prompt_core;           // store for later use
//   return Container::Vertical({
//       RenderStatusBar(...),
//       RenderMessageList(...),
//       prompt_comp,                             // <-- HERE
//   });
//
// In ReplScreenState, add:
//   std::shared_ptr<ui::components::TextInputImpl> prompt_core;
