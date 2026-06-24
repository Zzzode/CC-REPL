/// @file cost_threshold_dialog.cppm
/// @brief Faithful port of src/components/CostThresholdDialog.tsx
///
/// The cost-threshold dialog is a purely informative alert shown when the
/// session's Anthropic API spend reaches a pre-configured threshold.
///
/// It is a "Got it, thanks!" dialog with a single Select-style option.
/// Both Enter (commit selection) and Escape (Dialog.onCancel) dismiss the
/// dialog.  There is NEVER a "quit" or "reset" action — the dialog MUST NOT
/// cause data loss.
///
/// Contract (P0):
///   Payload  = { dollars_spent, optional model_name, on_done: void() }
///   Render   = title sprintf("You've spent $%.0f on the Anthropic API
///                       this session.", dollars_spent)
///              body   = "Learn more about how to monitor your spending:"
///                       + https://code.claude.com/docs/en/costs
///              actions = one button: "Got it, thanks!"
///   Keyboard = Enter  -> on_done()
///              Escape -> on_done()
///
/// Three historical renderers used to exist (dialog_default_renderers.cppm,
/// cost_threshold_dialog.cppm, and an inline lambda in repl_screen.cppm with
/// fabricated Continue/Reset/Quit chrome).  They have been unified into this
/// single module.  Callers should import this module and use
/// RenderCostThreshold / HandleCostThresholdEvent.
module;

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.cost_threshold_dialog;

export namespace cc::ui::dialogs::cost_threshold {

using namespace ftxui;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// External link rendered below the body paragraph.  Kept as a named
/// constant so tests can assert the exact reference URL without touching
/// the compiled output.
inline constexpr std::string_view kDocsUrl =
    "https://code.claude.com/docs/en/costs";

/// Body paragraph immediately above the external link.
inline constexpr std::string_view kBodyParagraph =
    "Learn more about how to monitor your spending:";

/// Label for the single Select option.  Mirrors TS
///   options = [{ value: "ok", label: "Got it, thanks!" }]
inline constexpr std::string_view kOkButtonLabel = "Got it, thanks!";

/// Mirrors TS CostThresholdDialog Props plus the engine-provided
/// `dollars_spent` / `model_name` context used to interpolate the title.
///
/// P0 CONTRACT — DO NOT add fabricated actions (Continue/Reset/Quit).
struct CostThresholdPayload {
    /// Session USD spend, formatted with `%.0f` in the title.
    double dollars_spent = 0.0;
    /// Optional model name, rendered dim under the body for context.
    std::optional<std::string> model_name;
    /// Invoked EXACTLY once when the user acknowledges the dialog via
    /// Enter, Escape, or a character shortcut (see HandleCostThresholdEvent).
    std::function<void()> on_done;
};

/// Internal state exposed to callers so that the single-option Select bullet
/// can be rendered consistently across re-draws.  The TS counterpart uses a
/// <Select> with a single option; because there is only one option we keep
/// selected_index fixed at 0 and allow Arrow keys to be no-ops (the index
/// never leaves [0, 0]).
struct CostThresholdState {
    double dollars_spent = 0.0;
    std::optional<std::string> model_name;
    int selected_index = 0;
    std::function<void()> on_done;
};

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

/// Build a render/event state from a payload.
[[nodiscard]] inline CostThresholdState state_from_payload(
    const CostThresholdPayload& p) {
    return CostThresholdState{
        .dollars_spent = p.dollars_spent,
        .model_name    = p.model_name,
        .selected_index = 0,
        .on_done       = p.on_done,
    };
}

/// Format the dialog title — "You've spent $N on the Anthropic API this
/// session."  Uses `std::lround` so that e.g. $4.70 displays as "$5" to
/// match the exact sprintf("%.0f") semantics required by the contract.
[[nodiscard]] inline std::string format_title(double dollars_spent) {
    const long whole = std::lround(dollars_spent);
    // Use std::to_string; for non-negative values (spend is always
    // non-negative) this matches sprintf("%lld") perfectly.
    return "You've spent $" + std::to_string(whole) +
           " on the Anthropic API this session.";
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

/// Faithful 1:1 render of TS <CostThresholdDialog>.
///
///   ┌──────────────────────────────────────────────────────────┐
///   │ You've spent $5 on the Anthropic API this session.       │
///   ├──────────────────────────────────────────────────────────┤
///   │  Learn more about how to monitor your spending:          │
///   │  https://code.claude.com/docs/en/costs                   │
///   │  (model: claude-3-5-sonnet-20241022)                     │  ← optional
///   ├──────────────────────────────────────────────────────────┤
///   │  ● Got it, thanks!                                       │
///   ├──────────────────────────────────────────────────────────┤
///   │  Enter acknowledge   Esc dismiss                         │
///   └──────────────────────────────────────────────────────────┘
///
/// Color: Yellow (warning) — consistent with the unified single renderer.
[[nodiscard]] inline Element RenderCostThreshold(const CostThresholdState& st) {
    using namespace ftxui;

    // ── Title (Dialog.title) ────────────────────────────────────
    const std::string title = format_title(st.dollars_spent);

    // ── Body paragraph + external link ──────────────────────────
    Elements body_rows;
    body_rows.push_back(text(std::string(kBodyParagraph)));
    body_rows.push_back(text(std::string(kDocsUrl)) | color(Color::CyanLight));
    if (st.model_name && !st.model_name->empty()) {
        body_rows.push_back(
            text("(model: " + *st.model_name + ")") | dim);
    }

    // ── Single-option CustomSelect clone ────────────────────────
    //    TS CustomSelect renders a ● bullet for the selected option,
    //    using the accent color for the selected row.  Because there is
    //    only one option, selected_index is always 0.
    const bool selected = (st.selected_index == 0);
    auto option_row = hbox({
        text(selected ? "● " : "○ ") |
            (selected ? bold | color(Color::YellowLight) : dim),
        text(std::string(kOkButtonLabel)) |
            (selected ? bold | color(Color::White) : dim),
    });

    // ── Keyboard byline ─────────────────────────────────────────
    auto byline = hbox({
        text("Enter") | bold | dim,
        text(" acknowledge") | dim,
        text("  ") | dim,
        text("Esc") | bold | dim,
        text(" dismiss") | dim,
    }) | dim;

    // ── Assemble inner content ──────────────────────────────────
    auto inner = vbox({
        text(title) | bold | color(Color::Yellow),
        separator(),
        vbox(std::move(body_rows)) | color(Color::GrayLight),
        separator(),
        std::move(option_row),
        separator(),
        std::move(byline),
    }) | size(WIDTH, GREATER_THAN, 58);

    // ── Wrap in titled window ───────────────────────────────────
    return window(text(" Cost Threshold ") | color(Color::Yellow),
                  std::move(inner)) | color(Color::Yellow);
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

/// Keyboard handler matching the TS <Dialog> + single-option <Select>
/// contract exactly:
///
///   Enter  → on_done()    (commit "Got it, thanks!")
///   Escape → on_done()    (Dialog.onCancel → onDone)  — NO data-loss quit!
///   Space  → on_done()    (Select's default activation key)
///   'g'/'G' → on_done()   (mnemonic for "Got it, thanks!")
///   'y'/'Y' → on_done()   (common "yes / acknowledge" mnemonic)
///   'o'/'O' → on_done()   (mnemonic for "OK")
///   'k'/'K' → on_done()   (mnemonic for "OK")
///
/// Arrow keys and Tab are consumed (they don't make sense with a single
/// option, but swallowing them prevents them from bubbling to a parent
/// handler and e.g. scrolling the message list while the dialog is open).
///
/// Any other character is also swallowed so that stray keystrokes do not
/// leak through to the prompt input — the TS dialog has focus.
///
/// Returns true if the event was consumed.
inline bool HandleCostThresholdEvent(CostThresholdState& st,
                                     const Event& ev) {
    // ── Definite dismissals ─────────────────────────────────────
    if (ev == Event::Return || ev == Event::Escape ||
        (ev.is_character() && ev.character() == " ")) {
        if (st.on_done) st.on_done();
        return true;
    }

    // ── Arrow keys + Tab: swallow so they never reach the parent ─
    if (ev == Event::ArrowUp || ev == Event::ArrowDown ||
        ev == Event::ArrowLeft || ev == Event::ArrowRight ||
        ev == Event::Tab || ev == Event::TabReverse) {
        return true;
    }

    // ── Character shortcuts ─────────────────────────────────────
    if (ev.is_character() && !ev.character().empty()) {
        const char c = ev.character()[0];
        switch (c) {
            case 'g': case 'G':   // mnemonic: "Got it, thanks!"
            case 'y': case 'Y':   // mnemonic: "acknowledge"
            case 'o': case 'O':   // mnemonic: "OK"
            case 'k': case 'K':   // mnemonic: "OK"
                if (st.on_done) st.on_done();
                return true;
            default:
                // Consume any other character so that stray keystrokes do
                // not leak through to the prompt input.  The TS dialog has
                // focus; any key other than the ones explicitly handled
                // should not do anything destructive.
                return true;
        }
    }

    return false;
}

} // namespace cc::ui::dialogs::cost_threshold
