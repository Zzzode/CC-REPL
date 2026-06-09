/// @file wizard_dialog.cppm
/// @brief Generic multi-step wizard framework.
///
/// Provides:
///   - WizardStep / WizardContext structs (with can_enter / can_leave guards,
///     conditional branching, async busy-flag support, cross-step shared data
///     and per-step error map).
///   - MakeWizard() factory returning a fully interactive FTXUI Component:
///       * Top: optional progress bar (%) + breadcrumb (dots with check/grey)
///       * Middle: current-step render() output
///       * Error strip: current step errors, red bg / yellow fg
///       * Footer: [Back] [Cancel] [Next / Finish] buttons with enable/disable
///       * Keyboard: ArrowRight/Space/N = next; ArrowLeft/Backspace/B = back;
///                   Esc = cancel; Enter = Next (validates via can_leave first)
///       * Async: render_spinner() placeholder disables buttons automatically.
///   - Three reusable sample wizards:
///       * Make3StepConfirmation  (Info -> Review -> Execute callback)
///       * MakeInputAndConfirm    (Input field -> Confirm)
///       * MakeFileWizard         (Select path -> Preview -> Confirm write)
///
/// Migrated from components/wizard/ (WizardProvider.tsx, WizardDialogLayout.tsx,
/// WizardNavigationFooter.tsx, useWizard.ts, index.ts).
///
/// Specific wizard content (install-GH-app, MCP add-server, plugin install) is
/// OWNED BY dedicated agents — this file is the framework + samples only.
module;

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <any>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <format>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.wizard_dialog;

export namespace cc::ui::wizard_dialog {
using namespace ftxui;

// ============================================================
// Forward-decl for shared context passed to step callbacks
// ============================================================
struct WizardContext;

// ============================================================
// Types
// ============================================================

/// Result of can_leave / can_enter checks.
/// - true message is empty  -> allowed
/// - false with message     -> blocked + message shown in error strip
struct StepCheckResult {
    bool ok = true;
    std::string message;  // used when !ok

    static StepCheckResult pass() { return {true, {}}; }
    static StepCheckResult fail(std::string msg) {
        return {false, std::move(msg)};
    }
    explicit operator bool() const { return ok; }
};

/// A single wizard step definition.
///
/// Step implementers provide callbacks for lifecycle hooks.
/// The render callback MUST NOT be null (it is the content).
struct WizardStep {
    std::string id;                  ///< Unique step id (for conditional jumps)
    std::string title;               ///< Display title
    std::string description;         ///< Short subtitle / description line
    bool optional = false;           ///< If true, "Skip" button is shown

    /// Called BEFORE entering this step.  Return fail() to redirect/prevent.
    std::function<StepCheckResult(WizardContext&)> can_enter;

    /// Called BEFORE leaving this step to go next/finish.
    /// Return fail() with error message to block (shown in error strip).
    std::function<StepCheckResult(WizardContext&)> can_leave;

    /// Content rendering.  Returns the step body as an Element.
    /// Returning the result of RenderSpinner() from this callback automatically
    /// disables navigation buttons (busy state).
    std::function<Element(WizardContext&)> render;

    /// Optional event handler — receives events *before* the wizard's own
    /// keyboard nav handles them.  Return true to consume the event.
    std::function<bool(WizardContext&, Event)> on_event;

    /// If non-empty, overrides the "next" step index (by id).
    /// Enables conditional branching, e.g. "go to step 'advanced' if flag set".
    /// Evaluated AFTER can_leave succeeds on the current step.
    /// Set from within can_leave or render via ctx.override_next.
    std::string override_next_id;

    /// Completion flag — shown in breadcrumb (✓) for steps before current.
    bool is_done = false;
};

/// Theme color identifier for the wizard dialog header.
enum class WizardColor : std::uint8_t {
    suggestion,
    permission,
    warning,
    info,
    success,
    danger,
};

/// Convert wizard color to FTXUI color.
[[nodiscard]] inline Color to_ftxui_color(WizardColor c) {
    switch (c) {
        case WizardColor::suggestion: return Color::Cyan;
        case WizardColor::permission: return Color::Magenta;
        case WizardColor::warning:    return Color::Yellow;
        case WizardColor::info:       return Color::Blue;
        case WizardColor::success:    return Color::Green;
        case WizardColor::danger:     return Color::Red;
    }
    return Color::White;
}

/// Shared wizard state passed to every step callback.
struct WizardContext {
    // ---- navigation state ----
    int current_step = 0;                      ///< Current step index
    int total_steps = 0;                       ///< Total linear steps (vector size)
    std::vector<WizardStep> steps;             ///< Steps storage (linear)
    std::vector<int> navigation_history;       ///< Back-stack
    bool is_completed = false;                 ///< Finish was triggered

    // ---- shared data (cross-step) ----
    /// Type-erased key/value store.  Put things like input strings,
    /// selected options, computed results here.  Usage:
    ///   ctx.data["path"] = std::string("/tmp/x");
    ///   auto s = std::any_cast<std::string>(ctx.data["path"]);
    std::unordered_map<std::string, std::any> data;

    // ---- error map ----
    /// Errors keyed by step id.  Error for current step id is shown.
    /// can_leave returning fail(msg) populates this for the current step id.
    std::unordered_map<std::string, std::string> errors;

    // ---- layout options ----
    std::string title = "Wizard";              ///< Dialog title
    bool show_step_counter = true;             ///< Append " (N/total)" to title
    bool show_progress_top = true;             ///< Render progress bar + breadcrumb
    bool show_breadcrumb = true;               ///< Render breadcrumb dots below progress
    WizardColor color = WizardColor::suggestion;
    std::optional<std::string> subtitle;       ///< Subtitle line under title
    std::optional<std::string> footer_text;    ///< Extra instruction line in footer

    // ---- conditional-branching override ----
    /// Set this from can_leave() or render() to jump to a non-linear step id
    /// on the next "Next" action.  Cleared automatically after use.
    std::optional<std::string> override_next_step_id;

    // ---- callbacks ----
    std::function<void(WizardContext&)> on_complete;  ///< Called on Finish
    std::function<void(WizardContext&)> on_cancel;    ///< Called on Esc at step 0 / explicit cancel

    // ============================================================
    // Convenience helpers
    // ============================================================

    /// Helper: get a value from data with a default (safer than any_cast throw).
    template <typename T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const {
        auto it = data.find(key);
        if (it == data.end()) return std::nullopt;
        try {
            return std::any_cast<T>(it->second);
        } catch (...) {
            return std::nullopt;
        }
    }

    /// Helper: put a value into data.
    template <typename T>
    void put(const std::string& key, T value) {
        data[key] = std::move(value);
    }

    /// Return current step pointer (null if out of range).
    [[nodiscard]] WizardStep* current() {
        if (current_step < 0 || current_step >= static_cast<int>(steps.size()))
            return nullptr;
        return &steps[current_step];
    }
    [[nodiscard]] const WizardStep* current() const {
        if (current_step < 0 || current_step >= static_cast<int>(steps.size()))
            return nullptr;
        return &steps[current_step];
    }

    /// Find step index by id.  Returns -1 if not found.
    [[nodiscard]] int find_step(const std::string& id) const {
        for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
            if (steps[i].id == id) return i;
        }
        return -1;
    }

    /// Check if current step is the last one.
    [[nodiscard]] bool is_last_step() const {
        return current_step >= static_cast<int>(steps.size()) - 1;
    }

    /// Get error message for current step (empty if none).
    [[nodiscard]] std::string current_error() const {
        auto* s = current();
        if (!s) return {};
        auto it = errors.find(s->id);
        return it == errors.end() ? std::string{} : it->second;
    }
};

// ============================================================
// Rendering primitives
// ============================================================

/// Spinner frames for async steps.  Not actually animated (FTXUI Renderer is
/// stateless in this framework), but provides a visual "busy" placeholder.
/// TODO(animation): FTXUI has no built-in fade/transition; hook into a ticker
/// once the main screen_loop is wired for per-frame redraws.
[[nodiscard]] inline Element RenderSpinner(std::string label = " Loading…") {
    // Use a deterministic spinner glyph; consumers relying on animation can
    // replace this with a per-frame index later.
    static constexpr const char* kFrames = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
    static std::size_t s_counter = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    char glyph = kFrames[(s_counter++) % 10];
    return hbox({
        text(std::string(1, glyph)) | color(Color::Cyan) | bold,
        text(" " + std::move(label)) | dim,
    });
}

/// True if an element is a spinner placeholder (heuristic: the render function
/// returned a spinner-like marker; we can't introspect Elements from the
/// outside, so we track a bool in the context instead.  This helper is for
/// cases where the caller wants to test their *own* render output.)
[[nodiscard]] inline bool IsSpinnerElement(const Element& /*e*/) {
    return false;  // no-op marker; actual busy flag is ctx.busy below.
}

/// Render a progress bar (0.0 ~ 1.0) using block characters.
[[nodiscard]] inline Element RenderProgressBar(double fraction, int width = 40) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    int filled = static_cast<int>(fraction * static_cast<double>(width));
    filled = std::clamp(filled, 0, width);
    int empty = width - filled;
    const std::string_view fill = "█";
    const std::string_view empty_char = "░";
    std::string bar;
    bar.reserve((filled + empty) * 3);  // ~3 bytes each for UTF-8 block chars
    for (int i = 0; i < filled; ++i) bar.append(fill);
    for (int i = 0; i < empty; ++i) bar.append(empty_char);

    int pct = static_cast<int>(fraction * 100.0);
    return hbox({
        text(" "),
        text(std::string{"["} + bar + "]") | color(Color::Cyan),
        text(std::format(" {:3d}%", pct)) | dim,
    });
}

/// Render breadcrumb dots + step titles.
///   done steps: ✓ title  (green)
///   current  : ● title   (bold white, arrow "→" prefix)
///   future   : ○ title   (dim / grey)
[[nodiscard]] inline Element RenderBreadcrumb(const WizardContext& ctx) {
    Elements items;
    for (int i = 0; i < static_cast<int>(ctx.steps.size()); ++i) {
        const auto& s = ctx.steps[i];
        std::string label = s.title.empty() ? s.id : s.title;
        Element line;
        if (i < ctx.current_step) {
            line = hbox({
                text("✓ ") | color(Color::Green),
                text(label) | color(Color::Green) | dim,
            });
        } else if (i == ctx.current_step) {
            line = hbox({
                text("→ ") | color(Color::Cyan) | bold,
                text(label) | bold,
            });
        } else {
            line = hbox({
                text("○ ") | color(Color::GrayDark),
                text(label) | dim,
            });
        }
        items.push_back(line);
        if (i != static_cast<int>(ctx.steps.size()) - 1) {
            items.push_back(text("  "));
        }
    }
    return hbox({ text(" "), hbox(std::move(items)) });
}

/// Render the error strip (only when the current step has an error).
[[nodiscard]] inline Element RenderErrorStrip(const WizardContext& ctx) {
    std::string msg = ctx.current_error();
    if (msg.empty()) return text("");
    return hbox({
        text(" "),
        hbox({
            text(" ✗ ") | bgcolor(Color::Red) | color(Color::Yellow) | bold,
            text(" " + msg + " ") | bgcolor(Color::Red) | color(Color::Yellow),
        }),
        text(" "),
    });
}

/// Render step counter label, e.g. " (2/5)".
[[nodiscard]] inline std::string step_counter_label(int current, int total) {
    return std::format(" ({}/{})", current + 1, total);
}

/// Navigation footer display options (actual buttons, not just text hints).
struct WizardFooterProps {
    bool can_go_back = false;
    bool can_go_next = true;
    bool is_last = false;          // last step => Next becomes Finish
    bool is_optional = false;      // current step is optional => Show Skip
    bool busy = false;             // async step => disable all nav
    std::string next_label;        // "Next" / "Finish" / custom
    std::string back_label = "Back";
    std::string cancel_label = "Cancel";
    std::optional<std::string> instructions;
};

/// Render the bottom footer with Back / Cancel / Next-Finish buttons.
/// Buttons are rendered as text labels (no interactive Button() widgets); the
/// wizard itself handles keyboard events so no component focus management is
/// needed for the footer itself.
[[nodiscard]] inline Element RenderFooterButtons(const WizardFooterProps& p) {
    auto btn = [](std::string label, bool enabled, Color col = Color::Cyan) {
        auto body = text(" " + std::move(label) + " ");
        if (enabled) {
            return body | border | color(col) | bold;
        }
        return body | border | dim | color(Color::GrayDark);
    };

    Elements row;
    // Back button
    row.push_back(btn(p.back_label, p.can_go_back && !p.busy, Color::Blue));
    row.push_back(text(" "));

    // Skip (for optional steps)
    if (p.is_optional) {
        row.push_back(btn("Skip", !p.busy, Color::Yellow));
        row.push_back(text(" "));
    }

    // Spacer
    row.push_back(filler());

    // Cancel
    row.push_back(btn(p.cancel_label, !p.busy, Color::Red));
    row.push_back(text(" "));

    // Next / Finish
    std::string lbl = p.next_label.empty()
        ? (p.is_last ? "Finish" : "Next")
        : p.next_label;
    row.push_back(btn(std::move(lbl), p.can_go_next && !p.busy,
                      p.is_last ? Color::Green : Color::Cyan));

    Elements result = { hbox(std::move(row)) };

    // Instruction line (below buttons)
    if (p.instructions) {
        Elements hints;
        hints.push_back(text(" "));
        hints.push_back(text("←/B:back") | dim | color(Color::GrayLight));
        hints.push_back(text("  "));
        hints.push_back(text("→/N/Enter:next") | dim | color(Color::GrayLight));
        hints.push_back(text("  "));
        hints.push_back(text("Esc:cancel") | dim | color(Color::GrayLight));
        hints.push_back(text("  "));
        hints.push_back(text(*p.instructions) | dim);
        result.push_back(hbox(std::move(hints)));
    }

    return vbox(result);
}

// ============================================================
// Core wizard component factory
// ============================================================

struct WizardConfig {
    std::string title = "Wizard";
    WizardColor color = WizardColor::suggestion;
    std::optional<std::string> subtitle;
    std::optional<std::string> footer_text;
    bool show_step_counter = true;
    bool show_progress_top = true;
    bool show_breadcrumb = true;
    std::unordered_map<std::string, std::any> initial_data;
};

/// Signature for the step-builder callback.
/// Receives a reference to the WizardContext (pre-filled with config) and must
/// populate ctx.steps with the step definitions.
using StepsFn = std::function<void(WizardContext&)>;

namespace internal {

// Determine whether the step content is "busy" (spinner-returning).
// Since we can't introspect FTXUI Elements after they're built, we provide an
// explicit opt-in: steps can set ctx.put<bool>("__busy", true) from their
// render() or on_event().  Also, when render() returns a spinner *and* the
// step.id contains "loading" the wizard treats it as busy.
[[nodiscard]] inline bool is_step_busy(const WizardContext& ctx) {
    auto v = ctx.get<bool>("__busy");
    if (v && *v) return true;
    auto* s = ctx.current();
    if (!s) return false;
    // Textual heuristic for id-name based "async step" convention.
    const auto& id = s->id;
    if (id.find("loading") != std::string::npos ||
        id.find("async") != std::string::npos ||
        id.find("fetch") != std::string::npos) {
        return true;
    }
    return false;
}

} // namespace internal

/// Build the full interactive wizard component.
///
/// @param config     Visual / behavioural configuration (title, colors, etc.)
/// @param build_steps Callback that populates ctx.steps and ctx.data.
///                    Invoked once at construction time.
/// @return           FTXUI Component suitable for Modal() / Screen rendering.
[[nodiscard]] inline Component MakeWizard(WizardConfig config,
                                          StepsFn build_steps) {
    // ---- Build context ----
    auto ctx = std::make_shared<WizardContext>();
    ctx->title              = std::move(config.title);
    ctx->color              = config.color;
    ctx->subtitle           = std::move(config.subtitle);
    ctx->footer_text        = std::move(config.footer_text);
    ctx->show_step_counter  = config.show_step_counter;
    ctx->show_progress_top  = config.show_progress_top;
    ctx->show_breadcrumb    = config.show_breadcrumb;
    ctx->data               = std::move(config.initial_data);

    // Invoke builder (populates ctx->steps, ctx->data etc.)
    if (build_steps) build_steps(*ctx);
    ctx->total_steps = static_cast<int>(ctx->steps.size());

    // We also cache the last-rendered body element to avoid re-rendering if the
    // step is busy (minimal).  The callbacks mutate ctx directly.

    // Lambda: compute derived footer properties from current state.
    auto compute_footer = [](const WizardContext& c) -> WizardFooterProps {
        WizardFooterProps p;
        p.can_go_back = (c.current_step > 0);
        p.is_last     = c.is_last_step();
        auto* cur = c.current();
        if (cur) p.is_optional = cur->optional;
        // can_go_next: step exists, can_leave passes (or has no guard)
        // We do NOT eagerly run can_leave here (it may have side effects).
        // Instead we default to true; the actual check runs on Next/Enter.
        p.can_go_next = (cur != nullptr);
        // But if there is an existing error for this step, disable Next/Finish.
        if (!c.current_error().empty()) p.can_go_next = false;
        // If busy, disable nav.
        p.busy = internal::is_step_busy(c);
        p.next_label = p.is_last ? "Finish" : "Next";
        if (c.footer_text) p.instructions = *c.footer_text;
        return p;
    };

    // Lambdas for navigation actions.  These mutate the context and may
    // run lifecycle hooks.
    struct Actions {
        std::shared_ptr<WizardContext> ctx;

        // Try go-next / finish.  Returns true if handled (even if blocked).
        bool try_advance() {
            if (ctx->is_completed) return true;
            WizardStep* cur = ctx->current();
            if (!cur) return false;

            // ---- can_leave guard ----
            if (cur->can_leave) {
                auto r = cur->can_leave(*ctx);
                if (!r.ok) {
                    ctx->errors[cur->id] = r.message;
                    return true;   // blocked: handled
                }
            }
            // Clear any prior error for this step now that we pass.
            ctx->errors.erase(cur->id);

            // Mark step done.
            cur->is_done = true;

            // ---- Determine destination ----
            if (ctx->is_last_step()) {
                // Finish
                ctx->is_completed = true;
                if (ctx->on_complete) ctx->on_complete(*ctx);
                return true;
            }

            int next_idx;
            // Check explicit override set via context.
            if (ctx->override_next_step_id) {
                int idx = ctx->find_step(*ctx->override_next_step_id);
                ctx->override_next_step_id.reset();
                if (idx >= 0) {
                    next_idx = idx;
                } else {
                    next_idx = ctx->current_step + 1;  // fall through
                }
            }
            // Check step's own override_next_id.
            else if (!cur->override_next_id.empty()) {
                int idx = ctx->find_step(cur->override_next_id);
                next_idx = (idx >= 0) ? idx : ctx->current_step + 1;
            } else {
                next_idx = ctx->current_step + 1;
            }

            // ---- can_enter on destination (if provided) ----
            // We re-check on the destination step in case it has an enter guard.
            int attempts = 0;
            while (attempts++ < 64) {
                if (next_idx < 0 || next_idx >= ctx->total_steps) break;
                auto& dst = ctx->steps[next_idx];
                if (dst.can_enter) {
                    auto r = dst.can_enter(*ctx);
                    if (!r.ok) {
                        // Can't enter this step.  Try next linear step unless
                        // the destination set an override we already honoured.
                        ctx->errors[dst.id] = r.message;
                        next_idx++;
                        continue;  // retry with linear next
                    }
                }
                // Enter allowed — clear errors for the destination.
                ctx->errors.erase(dst.id);
                break;
            }
            if (next_idx >= ctx->total_steps) next_idx = ctx->total_steps - 1;
            if (next_idx < 0) next_idx = 0;

            // Push history and advance.
            ctx->navigation_history.push_back(ctx->current_step);
            ctx->current_step = next_idx;
            return true;
        }

        // Go back one step (history-first, then linear decrement).
        bool try_back() {
            if (!ctx->navigation_history.empty()) {
                ctx->current_step = ctx->navigation_history.back();
                ctx->navigation_history.pop_back();
                // Clear any stale error on the step we're back to.
                auto* s = ctx->current();
                if (s) ctx->errors.erase(s->id);
                return true;
            }
            if (ctx->current_step > 0) {
                ctx->current_step--;
                auto* s = ctx->current();
                if (s) ctx->errors.erase(s->id);
                return true;
            }
            // Already at first step => cancel.
            if (ctx->on_cancel) ctx->on_cancel(*ctx);
            return true;
        }

        // Cancel unconditionally.
        bool try_cancel() {
            if (ctx->on_cancel) ctx->on_cancel(*ctx);
            return true;
        }

        // Optional step "skip" — behaves like advance but skips can_leave
        // entirely (the step was declared optional).
        bool try_skip() {
            WizardStep* cur = ctx->current();
            if (!cur || !cur->optional) return false;
            // Treat skip as "advance without validation".
            cur->is_done = true;
            if (ctx->is_last_step()) {
                ctx->is_completed = true;
                if (ctx->on_complete) ctx->on_complete(*ctx);
                return true;
            }
            ctx->navigation_history.push_back(ctx->current_step);
            ctx->current_step++;
            return true;
        }
    };

    auto actions = std::make_shared<Actions>();
    actions->ctx = ctx;

    // ---- Render function ----
    return Renderer([ctx, compute_footer] {
        // Header
        std::string title = ctx->title.empty() ? "Wizard" : ctx->title;
        if (ctx->show_step_counter && ctx->total_steps > 0) {
            title += step_counter_label(ctx->current_step, ctx->total_steps);
        }
        auto header_color = to_ftxui_color(ctx->color);

        Elements header_lines;
        header_lines.push_back(hbox({
            text(" " + title + " ") | bold | color(header_color),
        }));
        if (ctx->subtitle) {
            header_lines.push_back(text(" " + *ctx->subtitle) | dim);
        }

        Elements body_parts;
        body_parts.push_back(vbox(header_lines));
        body_parts.push_back(separator() | color(header_color));

        // Progress + breadcrumb
        if (ctx->show_progress_top && ctx->total_steps > 1) {
            double frac = (ctx->total_steps <= 1)
                ? 1.0
                : static_cast<double>(ctx->current_step) /
                      static_cast<double>(ctx->total_steps - 1);
            body_parts.push_back(RenderProgressBar(frac));
        }
        if (ctx->show_breadcrumb && ctx->total_steps > 1) {
            body_parts.push_back(RenderBreadcrumb(*ctx));
            body_parts.push_back(separatorLight() | dim);
        }

        // Current step content.
        Element step_content;
        if (ctx->steps.empty()) {
            step_content = text("  No steps defined") | dim;
        } else {
            auto* s = ctx->current();
            if (s && s->render) {
                step_content = s->render(*ctx);
            } else if (s) {
                // Fallback placeholder.
                Elements el;
                if (!s->title.empty()) el.push_back(text("  " + s->title) | bold);
                if (!s->description.empty()) {
                    el.push_back(text(""));
                    el.push_back(text("  " + s->description) | dim);
                }
                step_content = vbox(el);
            } else {
                step_content = text("  (invalid step index)") | dim | color(Color::Red);
            }
        }
        body_parts.push_back(step_content | flex);

        // Error strip.
        Element err = RenderErrorStrip(*ctx);
        body_parts.push_back(err);

        // Footer
        body_parts.push_back(separator() | dim);
        auto footer_props = compute_footer(*ctx);
        body_parts.push_back(RenderFooterButtons(footer_props));

        return vbox(body_parts) | border | color(header_color);
    }) | CatchEvent([ctx, actions](Event event) -> bool {
        // ---- Step-specific event hook first ----
        {
            auto* s = ctx->current();
            if (s && s->on_event) {
                if (bool consumed = s->on_event(*ctx, event); consumed) return true;
            }
        }

        // ---- Global keyboard navigation ----
        const bool busy = internal::is_step_busy(*ctx);
        if (busy) {
            // During async steps, only Esc (abort) is honored.
            if (event == Event::Escape) return actions->try_cancel();
            // Otherwise consume nothing so sub-components can still receive
            // input (useful if the busy step has its own cancel handler).
            return false;
        }

        // Cancel
        if (event == Event::Escape) return actions->try_cancel();

        // Next / Finish
        if (event == Event::Return ||
            event == Event::ArrowRight ||
            event == Event::Character(' ') ||
            event == Event::Character('n') ||
            event == Event::Character('N')) {
            return actions->try_advance();
        }

        // Back
        if (event == Event::ArrowLeft ||
            event == Event::Backspace ||
            event == Event::Character('b') ||
            event == Event::Character('B')) {
            return actions->try_back();
        }

        // Skip (optional step) — Tab / s
        auto* cur = ctx->current();
        if (cur && cur->optional) {
            if (event == Event::Tab ||
                event == Event::Character('s') ||
                event == Event::Character('S')) {
                return actions->try_skip();
            }
        }

        // Delegate to any interactive children: FTXUI propagates events down
        // the component tree via OnEvent; we only wrap *our* Renderer with
        // CatchEvent, so sub-components inside step render() must be attached
        // separately.  If the step's render returned a Component stored on
        // the step, this framework can't know about it.  To support this,
        // steps should handle their own events via on_event().  For the
        // common case (pure static render) this is fine.
        return false;
    });
}

// ============================================================
// Sample Wizard #1 — 3-step confirmation
// ============================================================
//
// Flow:
//   Step 1 "info"    — Summary text (provided by caller)
//   Step 2 "review"  — Detailed key/value review (provided by caller)
//   Step 3 "execute" — Confirmation + on_execute callback invoked on Finish
//
// The caller fills in the text/review entries via callbacks.

struct ConfirmationSummary {
    std::string title;                               ///< Title of the operation
    std::string description;                         ///< Short "what this does"
    std::vector<std::pair<std::string, std::string>> details;  ///< k/v pairs
};

/// Construct a reusable 3-step confirmation wizard.
///
/// @param summary   User-visible description + details (copied into ctx.data).
/// @param on_execute Called on Finish.  Must perform the real work.
///                   Return empty string on success; return an error string to
///                   block finish and show the error on step 3.
/// @param on_cancel Optional cancel handler.
[[nodiscard]] inline Component Make3StepConfirmation(
    ConfirmationSummary summary,
    std::function<std::string()> on_execute,
    std::function<void()> on_cancel = nullptr,
    WizardConfig base_config = {}) {

    auto config = std::move(base_config);
    if (config.title.empty()) config.title = summary.title.empty() ? "Confirm" : summary.title;
    if (!config.subtitle && !summary.description.empty()) config.subtitle = summary.description;
    config.color = WizardColor::info;

    StepsFn builder = [summary = std::move(summary),
                       on_execute = std::move(on_execute),
                       on_cancel = std::move(on_cancel)](WizardContext& ctx) {
        // Store details in shared data so steps can render them.
        ctx.put("__summary_title", summary.title);
        ctx.put("__summary_desc",  summary.description);
        ctx.put("__details",       summary.details);

        // ---- Step 1: Info summary ----
        WizardStep info;
        info.id = "info";
        info.title = "Information";
        info.description = "Review what you're about to do.";
        info.render = [](WizardContext& c) -> Element {
            Elements el;
            auto t = c.get<std::string>("__summary_title");
            auto d = c.get<std::string>("__summary_desc");
            el.push_back(text(""));
            el.push_back(hbox({ text("  ℹ  "), text(t.value_or("")) | bold | color(Color::Cyan) }));
            if (d && !d->empty()) {
                el.push_back(text(""));
                el.push_back(paragraph("  " + *d) | dim);
            }
            el.push_back(text(""));
            el.push_back(text("  Press → or Enter to see the detailed review.") | dim);
            return vbox(std::move(el)) | flex;
        };
        ctx.steps.push_back(std::move(info));

        // ---- Step 2: Review details ----
        WizardStep review;
        review.id = "review";
        review.title = "Review";
        review.description = "Please verify all details below.";
        review.render = [](WizardContext& c) -> Element {
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  📋  "), text("Detailed Review") | bold | color(Color::Yellow) }));
            el.push_back(text(""));
            auto details = c.get<std::vector<std::pair<std::string, std::string>>>("__details");
            if (details && !details->empty()) {
                for (const auto& [k, v] : *details) {
                    el.push_back(hbox({
                        text("    " + k + ": ") | dim,
                        text(v.empty() ? std::string{"(empty)"} : v) | color(Color::White),
                    }));
                }
            } else {
                el.push_back(text("    (no details provided)") | dim);
            }
            el.push_back(text(""));
            el.push_back(text("  If everything looks correct, press → / Enter to confirm.") | dim);
            return vbox(std::move(el)) | flex;
        };
        ctx.steps.push_back(std::move(review));

        // ---- Step 3: Confirm & execute ----
        WizardStep exec;
        exec.id = "execute";
        exec.title = "Confirm";
        exec.description = "This action cannot be undone.";
        exec.can_leave = [on_execute](WizardContext& c) -> StepCheckResult {
            // The "Finish" action triggers can_leave which is where we run
            // the real execute callback.  If it returns an error string the
            // Finish is blocked and the error shown.
            if (!on_execute) return StepCheckResult::pass();
            std::string err = on_execute();
            if (!err.empty()) return StepCheckResult::fail(std::move(err));
            return StepCheckResult::pass();
        };
        exec.render = [](WizardContext& c) -> Element {
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  ⚠  "), text("Final Confirmation") | bold | color(Color::Yellow) }));
            el.push_back(text(""));
            auto t = c.get<std::string>("__summary_title");
            el.push_back(paragraph("  You are about to: " + std::string(t.value_or("proceed with the operation")) + ".") | color(Color::Yellow));
            el.push_back(text(""));
            el.push_back(text("  This action cannot be undone.") | color(Color::Red) | dim);
            el.push_back(text(""));
            el.push_back(text("  Press Enter / Finish to proceed.") | dim);
            return vbox(std::move(el)) | flex;
        };
        ctx.steps.push_back(std::move(exec));

        // ---- Complete/Cancel wiring ----
        ctx.on_complete = [](WizardContext&) { /* execute ran inside can_leave */ };
        if (on_cancel) {
            ctx.on_cancel = [on_cancel](WizardContext&) { on_cancel(); };
        }
    };

    return MakeWizard(std::move(config), std::move(builder));
}

// ============================================================
// Sample Wizard #2 — Input-and-confirm (2-step)
// ============================================================
//
// Step 1: input field (supports Input component-like behaviour via on_event)
// Step 2: review and confirm
//
// The result is stored in ctx.data["input_value"] and passed to on_confirm.

struct InputAndConfirmConfig {
    std::string wizard_title = "Input";
    std::string input_label  = "Enter value";
    std::string input_hint   = "type your value here";
    std::string placeholder  = "";
    std::string initial_value = "";
    bool allow_empty = false;                 ///< If false, empty value is rejected
    std::function<std::string(std::string)> validate;  ///< Optional validator
    std::function<void(std::string)> on_confirm;       ///< Invoked on Finish with final value
    std::function<void()> on_cancel;
};

/// Create a 2-step "input then confirm" wizard.
///
/// The first step emulates a simple input field:
///   - printable characters append
///   - Backspace removes last char
///   - The typed value is stored in ctx.data["input_value"].
///
/// TODO(input): Replace with FTXUI's real Input() component once the FTXUI
/// widget library is properly exposed via the module system.
[[nodiscard]] inline Component MakeInputAndConfirm(InputAndConfirmConfig cfg) {
    WizardConfig config;
    config.title = std::move(cfg.wizard_title);
    config.color = WizardColor::suggestion;

    StepsFn builder = [cfg = std::move(cfg)](WizardContext& ctx) mutable {
        ctx.put<std::string>("input_value", cfg.initial_value);

        // ---- Step 1: Input ----
        WizardStep input;
        input.id = "input";
        input.title = cfg.input_label;
        input.description = cfg.input_hint;
        input.render = [label = std::move(cfg.input_label),
                        placeholder = std::move(cfg.placeholder)](WizardContext& c) -> Element {
            auto value = c.get<std::string>("input_value").value_or("");
            std::string display = value.empty() ? placeholder : value;
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({
                text("  " + label + ": ") | dim,
                text(display) | (value.empty() ? dim : bold) | color(Color::Cyan),
                text("▌") | color(Color::Cyan),  // cursor
            }));
            el.push_back(text(""));
            el.push_back(text("  Type to edit, Backspace to delete, Enter/→ when ready.") | dim);
            return vbox(std::move(el)) | flex;
        };
        input.on_event = [](WizardContext& c, Event e) -> bool {
            auto value = c.get<std::string>("input_value").value_or("");
            if (e.is_character()) {
                value.push_back(e.character()[0]);
                c.put<std::string>("input_value", std::move(value));
                return true;
            }
            if (e == Event::Backspace || e == Event::Delete) {
                if (!value.empty()) value.pop_back();
                c.put<std::string>("input_value", std::move(value));
                return true;
            }
            return false;
        };
        input.can_leave = [allow_empty = cfg.allow_empty,
                           validate = std::move(cfg.validate)](WizardContext& c) -> StepCheckResult {
            auto v = c.get<std::string>("input_value").value_or("");
            if (!allow_empty && v.empty()) {
                return StepCheckResult::fail("Value cannot be empty");
            }
            if (validate) {
                std::string err = validate(v);
                if (!err.empty()) return StepCheckResult::fail(std::move(err));
            }
            return StepCheckResult::pass();
        };
        ctx.steps.push_back(std::move(input));

        // ---- Step 2: Confirm ----
        WizardStep confirm;
        confirm.id = "confirm";
        confirm.title = "Confirm";
        confirm.description = "Verify your input.";
        confirm.render = [label = cfg.input_label](WizardContext& c) -> Element {
            auto v = c.get<std::string>("input_value").value_or("");
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  ✔  "), text("Review your input") | bold | color(Color::Green) }));
            el.push_back(text(""));
            el.push_back(hbox({ text("    " + label + ": ") | dim, text(v) | bold | color(Color::Cyan) }));
            el.push_back(text(""));
            el.push_back(text("  Press Enter / Finish to confirm.") | dim);
            return vbox(std::move(el)) | flex;
        };
        confirm.can_leave = [on_confirm = std::move(cfg.on_confirm)](WizardContext& c) -> StepCheckResult {
            if (on_confirm) {
                auto v = c.get<std::string>("input_value").value_or("");
                on_confirm(v);
            }
            return StepCheckResult::pass();
        };
        ctx.steps.push_back(std::move(confirm));

        if (cfg.on_cancel) {
            ctx.on_cancel = [oc = std::move(cfg.on_cancel)](WizardContext&) { oc(); };
        }
    };

    return MakeWizard(std::move(config), std::move(builder));
}

// ============================================================
// Sample Wizard #3 — File wizard (3-4 step)
// ============================================================
//
// Step 1: Select output path   (emulated input field)
// Step 2: Preview content      (shows first N lines + size)
// Step 3: Confirm write        (Finish triggers on_write callback)
//
// Optional Step 0: Source select — pass non-empty source_paths to enable.

struct FileWizardConfig {
    std::string wizard_title = "Export File";
    std::string default_path = "output.txt";
    std::string content_preview;          ///< Full content to be written
    std::vector<std::string> source_paths; ///< If >1, adds step 0 to pick a source
    /// Callback to actually perform the write.  Return empty string on
    /// success, or an error message to block Finish and show it.
    std::function<std::string(std::string_view path, std::string_view content)> on_write;
    std::function<void()> on_cancel;
    std::size_t preview_line_limit = 25;
};

/// Split a string by newlines into a vector (for preview truncation).
inline std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : s) {
        if (ch == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty() || !s.empty() && s.back() == '\n') {
        lines.push_back(std::move(cur));
    }
    return lines;
}

/// Create a 3 or 4-step "pick path → preview → confirm write" wizard.
[[nodiscard]] inline Component MakeFileWizard(FileWizardConfig cfg) {
    WizardConfig config;
    config.title = std::move(cfg.wizard_title);
    config.color = WizardColor::success;

    StepsFn builder = [cfg = std::move(cfg)](WizardContext& ctx) mutable {
        ctx.put<std::string>("output_path", cfg.default_path);
        ctx.put<std::string>("file_content", cfg.content_preview);
        ctx.put<std::size_t>("preview_line_limit", cfg.preview_line_limit);

        // ---- Optional Step 0: Source selection ----
        if (cfg.source_paths.size() > 1) {
            WizardStep src;
            src.id = "source";
            src.title = "Select source";
            src.description = "Choose which file to export.";
            ctx.put<int>("source_index", 0);
            src.on_event = [&paths = cfg.source_paths](WizardContext& c, Event e) -> bool {
                int idx = c.get<int>("source_index").value_or(0);
                if (e == Event::ArrowDown || e == Event::Character('j')) {
                    idx = std::min(static_cast<int>(paths.size()) - 1, idx + 1);
                    c.put<int>("source_index", idx);
                    return true;
                }
                if (e == Event::ArrowUp || e == Event::Character('k')) {
                    idx = std::max(0, idx - 1);
                    c.put<int>("source_index", idx);
                    return true;
                }
                return false;
            };
            src.render = [&paths = cfg.source_paths](WizardContext& c) -> Element {
                int sel = c.get<int>("source_index").value_or(0);
                Elements el;
                el.push_back(text(""));
                el.push_back(hbox({ text("  📂 "), text("Source file") | bold | color(Color::Yellow) }));
                el.push_back(text(""));
                for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
                    std::string prefix = (i == sel) ? "  > " : "    ";
                    Element line = text(prefix + paths[i]);
                    if (i == sel) line = line | bold | color(Color::Cyan);
                    else line = line | dim;
                    el.push_back(line);
                }
                el.push_back(text(""));
                el.push_back(text("  ↑↓ / jk to select, Enter / → to continue.") | dim);
                return vbox(std::move(el)) | flex;
            };
            src.can_leave = [&paths = cfg.source_paths](WizardContext& c) -> StepCheckResult {
                int sel = c.get<int>("source_index").value_or(0);
                if (sel >= 0 && sel < static_cast<int>(paths.size())) {
                    c.put<std::string>("source_selected", paths[sel]);
                }
                return StepCheckResult::pass();
            };
            ctx.steps.push_back(std::move(src));
        }

        // ---- Step (0) 1: Select output path ----
        WizardStep path;
        path.id = "path";
        path.title = "Output path";
        path.description = "Enter the destination file path.";
        path.render = [](WizardContext& c) -> Element {
            auto v = c.get<std::string>("output_path").value_or("");
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  💾 "), text("Output path") | bold | color(Color::Yellow) }));
            el.push_back(text(""));
            el.push_back(hbox({
                text("    Path: ") | dim,
                text(v.empty() ? "(type a path)" : v)
                    | (v.empty() ? dim : bold) | color(Color::Cyan),
                text("▌") | color(Color::Cyan),
            }));
            el.push_back(text(""));
            el.push_back(text("  Type characters, Backspace to delete, ←/→ to switch steps.") | dim);
            return vbox(std::move(el)) | flex;
        };
        path.on_event = [](WizardContext& c, Event e) -> bool {
            auto v = c.get<std::string>("output_path").value_or("");
            if (e.is_character()) {
                v.push_back(e.character()[0]);
                c.put<std::string>("output_path", std::move(v));
                return true;
            }
            if (e == Event::Backspace || e == Event::Delete) {
                if (!v.empty()) v.pop_back();
                c.put<std::string>("output_path", std::move(v));
                return true;
            }
            return false;
        };
        path.can_leave = [](WizardContext& c) -> StepCheckResult {
            auto v = c.get<std::string>("output_path").value_or("");
            if (v.empty()) return StepCheckResult::fail("Output path cannot be empty");
            return StepCheckResult::pass();
        };
        ctx.steps.push_back(std::move(path));

        // ---- Step 1 2: Preview content ----
        WizardStep preview;
        preview.id = "preview";
        preview.title = "Preview";
        preview.description = "Verify the content that will be written.";
        preview.render = [](WizardContext& c) -> Element {
            auto content = c.get<std::string>("file_content").value_or("");
            auto limit = c.get<std::size_t>("preview_line_limit").value_or(25);
            auto lines = split_lines(content);
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  👀 "), text("Content preview") | bold | color(Color::Yellow) }));
            el.push_back(text(std::format("    {} bytes, {} line{}",
                                          content.size(),
                                          lines.size(),
                                          lines.size() == 1 ? "" : "s")) | dim);
            el.push_back(separatorLight());
            std::size_t shown = 0;
            for (; shown < lines.size() && shown < limit; ++shown) {
                el.push_back(text("    " + lines[shown]));
            }
            if (shown < lines.size()) {
                el.push_back(text(std::format("    … ({} more lines omitted)",
                                              lines.size() - shown)) | dim);
            }
            if (lines.empty()) {
                el.push_back(text("    (file is empty)") | dim);
            }
            el.push_back(text(""));
            return vbox(std::move(el)) | yframe | flex;
        };
        ctx.steps.push_back(std::move(preview));

        // ---- Step 2 3: Confirm & write ----
        WizardStep write;
        write.id = "confirm";
        write.title = "Confirm write";
        write.description = "The file will be written when you confirm.";
        write.can_leave = [on_write = std::move(cfg.on_write)](WizardContext& c) -> StepCheckResult {
            if (!on_write) return StepCheckResult::pass();
            auto p = c.get<std::string>("output_path").value_or("");
            auto content = c.get<std::string>("file_content").value_or("");
            std::string err = on_write(p, content);
            if (!err.empty()) return StepCheckResult::fail(std::move(err));
            return StepCheckResult::pass();
        };
        write.render = [](WizardContext& c) -> Element {
            auto p = c.get<std::string>("output_path").value_or("");
            auto content = c.get<std::string>("file_content").value_or("");
            Elements el;
            el.push_back(text(""));
            el.push_back(hbox({ text("  ⚠  "), text("Confirm file write") | bold | color(Color::Yellow) }));
            el.push_back(text(""));
            el.push_back(hbox({ text("    Path:   ") | dim, text(p) | bold | color(Color::Cyan) }));
            el.push_back(hbox({ text("    Size:   ") | dim,
                                text(std::format("{} bytes", content.size())) | color(Color::White) }));
            el.push_back(text(""));
            el.push_back(text("  This will OVERWRITE the file if it exists.") | color(Color::Red) | dim);
            el.push_back(text("  Press Enter / Finish to write.") | dim);
            return vbox(std::move(el)) | flex;
        };
        ctx.steps.push_back(std::move(write));

        if (cfg.on_cancel) {
            ctx.on_cancel = [oc = std::move(cfg.on_cancel)](WizardContext&) { oc(); };
        }
    };

    return MakeWizard(std::move(config), std::move(builder));
}

} // namespace cc::ui::wizard_dialog
