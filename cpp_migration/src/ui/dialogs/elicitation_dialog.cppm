/// @file elicitation_dialog.cppm
/// @brief Elicitation — MCP structured-input confirmation dialog.
///
/// MODULE:   cc.ui.dialogs.elicitation
/// LICENCE:  Exported.  Renderer + keyboard-event handler for the
///           simple Band3 Elicitation prompt shown when an MCP server
///           asks Claude to gather extra input from the user.
///
/// TS REFERENCE (simple form — "server connect prompt"):
///   src/components/mcp/ElicitationDialog.tsx
///     - ElicitationFormDialog render (line ~957): title + subtitle +
///       body paragraph + Accept/Decline buttons with keyboard shortcuts.
///     - ElicitationURLDialog render (line ~1140): same DialogFrame but
///       URL-flavoured subtitle.
///   This module renders the SIMPLE (non-form, non-URL-resolving) form
///   of the prompt — just "Allow connecting to <server name>?" with
///   three escape hatches.  Multi-field schemas are handled by the
///   heavier cc.ui.dialogs.mcp_dialogs module (Faithful port of the
///   1200-line renderFormFields engine).
///
/// VISUAL SPEC (faithful to TS <Dialog color="permission">):
///   ┌─ MCP Server Request ─────────────────────────────────────┐
///   │ "Brave Search" needs additional input                    │ ← subtitle, dim
///   ├──────────────────────────────────────────────────────────┤
///   │  Allow connecting to Brave Search?                       │ ← body (paragraph)
///   │                                                          │
///   │  Request ID: 42                                          │ ← dim row
///   │                                                          │
///   │  [y] Allow    [n] Deny    [Esc] Cancel                   │ ← buttons row
///   └──────────────────────────────────────────────────────────┘
///   FrameStyle::Info (permission color).  Rendered into the
///   Bottom slot of FullscreenLayout (band 3 — suppressed while
///   user types or a JSX tool animation is active).
///
/// KEYBOARD SPEC (exact key → callback mapping):
///   y / Y       → on_response(approve=true)   "Allow once"
///   Enter       → on_response(approve=true)   default accept
///   n / N       → on_response(approve=false)  "Deny"
///   Esc         → on_cancel()                 "Cancel / dismiss"
///   All other keys: return false (fall through to global handler).
///
/// CALLBACK SEMANTICS:
///   on_response is the "decision" callback; on_cancel is a distinct
///   "user cancelled out-of-band" path so callers can distinguish
///   "no, and Claude should be told to try differently" from
///   "user bailed out — do not send a response back to the server".
module;

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.elicitation;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.design.theme;

export namespace cc::ui::dialogs::elicitation {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;

// ============================================================
// Renderer
// ============================================================

/// Render a faithful Elicitation dialog (simple form).
///
/// Mirrors the non-field ElicitationDialog render from TS — a
/// DialogFrame with title/subtitle/body/request-id row / buttons row.
[[nodiscard]] inline Element RenderElicitation(
    const dsys::ElicitationPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    const auto& theme = ctx.theme;

    dframe::DialogFrameProps props;
    // Matches TS: `Dialog title={`MCP server "${serverName}" requests your input`}`
    // Compressed form for narrow terminals:
    props.title = "MCP Server Request";
    props.subtitle = p.server_name.empty()
        ? std::string{"An MCP server needs additional input"}
        : std::string{p.server_name + " needs additional input"};
    props.style = dframe::FrameStyle::Permission;

    // Body:
    //   - question: "Allow connecting to <server_name>?"
    //   - blank
    //   - "Request ID: <id>"  (dim, matches TS request_id row)
    //   - blank
    //   - buttons row
    Elements body;

    // --- Question paragraph ---
    const std::string display_server = p.server_name.empty()
        ? std::string{"the MCP server"}
        : p.server_name;
    body.push_back(hbox({
        text("Allow connecting to "),
        text(display_server) | bold,
        text("?"),
    }));

    body.push_back(text(""));

    // --- Optional request-description paragraph ---
    if (!p.request_description.empty()) {
        body.push_back(paragraph(p.request_description) | dim);
        body.push_back(text(""));
    }

    // --- Request ID row (dim, mono-ish) ---
    body.push_back(hbox({
        text("Request ID: ") | dim,
        text(std::to_string(p.request_id)) | dim,
    }));

    body.push_back(text(""));

    // --- Buttons row ---
    body.push_back(hbox({
        hbox({
            text("[y] Allow") | color(Color::Green),
        }),
        text("  "),
        hbox({
            text("[n] Deny") | color(Color::Red),
        }),
        text("  "),
        hbox({
            text("[Esc] Cancel") | color(Color::GrayDark) | dim,
        }),
    }));

    props.content = vbox(std::move(body));
    return dframe::DialogFrame(props, theme);
}

// ============================================================
// Keyboard event handler
// ============================================================

/// Handle keyboard events for the simple Elicitation dialog.
///
/// Mappings (faithful to TS ElicitationFormDialog keyboard shortcuts):
///   y / Y / Enter → on_response(approve=true)
///   n / N         → on_response(approve=false)
///   Esc           → on_cancel (distinct callback; callers typically
///                   pop the dialog and do NOT send a response to the
///                   MCP server — Esc is an out-of-band dismiss).
///
/// Returns true if the event was handled.
inline bool HandleElicitationEvent(
    dsys::ElicitationPayload& p,
    const Event& event)
{
    // y / Y — allow (same as TS `confirm:yes` shortcut)
    if (event == Event::Character('y') || event == Event::Character('Y')) {
        if (p.on_response) p.on_response(/*approve=*/true);
        return true;
    }
    // n / N — deny
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) p.on_response(/*approve=*/false);
        return true;
    }
    // Enter — default = approve
    if (event == Event::Return) {
        if (p.on_response) p.on_response(/*approve=*/true);
        return true;
    }
    // Esc — cancel / dismiss (out-of-band — separate callback when set,
    // otherwise fall back to on_response(false) so callers that only
    // bind on_response still get a deterministic signal).
    if (event == Event::Escape) {
        if (p.on_cancel) {
            p.on_cancel();
        } else if (p.on_response) {
            p.on_response(/*approve=*/false);
        }
        return true;
    }
    return false;
}

// ============================================================
// Convenience: register into a DialogRendererRegistry
// ============================================================

/// Register the Elicitation renderer + event handler into a registry.
/// (The default set of registrations lives in
/// cc.ui.dialogs.default_renderers — callers that want to opt-in to
/// only this specific dialog can do so directly via this helper.)
inline void RegisterElicitationDialog(dsys::DialogRendererRegistry& registry)
{
    registry.register_dialog(
        dsys::DialogType::Elicitation,
        /*renderer=*/
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ElicitationPayload>(&payload);
            if (!p) return text("");
            return RenderElicitation(*p, ctx);
        },
        /*event_handler=*/
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ElicitationPayload>(&payload);
            if (!p) return false;
            return HandleElicitationEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::elicitation