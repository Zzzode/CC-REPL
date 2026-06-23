/// @file dialog_default_renderers.cppm
/// @brief Default dialog renderers for M7 payload types.
///
/// MODULE:   cc.ui.dialogs.default_renderers
/// LICENCE:  Exported.  Imported by app initialization to register the
///           default set of dialog renderers.
///
/// Each renderer uses DialogFrame for consistent styling (faithful
/// to TS PermissionDialog visual language).  These are default
/// implementations that can be overridden by registering custom renderers.
///
/// Renderers provided here:
///   - ToolPermission (overlay slot) — delegates to faithful permission panels
///   - SandboxPermission (bottom slot) — network access request
///   - PromptDialog (bottom slot) — hook input request
///   - Elicitation (bottom slot) — MCP structured input
///   - CostThreshold (bottom slot) — cost exceeded
///   - IdleReturn (bottom slot) — welcome back
///   - GenericDialogPayload — fallback generic dialog
module;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.default_renderers;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.permissions.single_prompt;
import cc.ui.permissions.components;
import cc.ui.design.theme;
import cc.ui.design.primitives;

export namespace cc::ui::dialogs::default_renderers {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
namespace pc = cc::ui::permissions::components;
namespace sp = cc::ui::permissions::single_prompt;
using Theme = cc::ui::design::theme::Theme;

// ============================================================
// ToolPermission renderer
// ============================================================

/// Render the ToolPermission dialog using the faithful SinglePrompt.
/// This delegates to cc.ui.permissions.single_prompt.
[[nodiscard]] inline Element RenderToolPermission(
    const dsys::ToolPermissionPayload& p,
    const dsys::DialogRenderContext& /*ctx*/)
{
    // Build SinglePromptProps from the payload
    sp::SinglePromptProps props;
    props.tool_name = p.tool_name;
    props.action_kind = p.action_kind;
    props.risk_level = p.risk_level;
    props.description = p.description;
    props.affected_paths = p.affected_paths;
    if (p.workspace_root) props.workspace_root = *p.workspace_root;
    props.detail = p.detail;
    props.rule_match_explanation = p.rule_match_explanation;
    props.initial_sandbox_toggle = p.initial_sandbox_toggle;
    // Note: callbacks are invoked via the event handler, not here
    // (this is just the render function).

    // Use SinglePrompt's render function for faithful output
    auto state = std::make_shared<sp::PromptState>();
    state->props = std::move(props);
    return sp::RenderSinglePrompt(state);
}

/// ToolPermission event handler — delegates to SinglePrompt's event handling.
inline bool HandleToolPermissionEvent(
    dsys::ToolPermissionPayload& p,
    const Event& event)
{
    // We need a PromptState to use the existing event handling.
    // Build a temporary state, handle the event, and update the payload.
    // For now, implement simplified key handling matching the SinglePrompt patterns.

    // Direct shortcuts
    if (event == Event::Character('y') || event == Event::Character('Y')) {
        if (p.on_response) {
            p.on_response(sp::Decision::AllowOnce, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) {
            p.on_response(sp::Decision::Deny, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Character('a') || event == Event::Character('A')) {
        if (p.can_always_allow && p.on_response) {
            p.on_response(sp::Decision::AlwaysAllow, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Character('d') || event == Event::Character('D')) {
        if (p.on_response) {
            p.on_response(sp::Decision::AlwaysDeny, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Character('s') || event == Event::Character('S')) {
        p.initial_sandbox_toggle = !p.initial_sandbox_toggle;
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_abort) p.on_abort();
        if (p.on_response) {
            p.on_response(sp::Decision::Abort, p.initial_sandbox_toggle);
        }
        return true;
    }
    if (event == Event::Return) {
        if (p.on_response) {
            p.on_response(sp::Decision::AllowOnce, p.initial_sandbox_toggle);
        }
        return true;
    }

    return false;
}

// ============================================================
// SandboxPermission renderer
// ============================================================

[[nodiscard]] inline Element RenderSandboxPermission(
    const dsys::SandboxPermissionPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = p.is_worker ? "Worker Network Access" : "Network Access Request";
    props.subtitle = p.is_worker
        ? std::string{"Worker \"" + p.worker_request_id + "\" requests network access"}
        : std::string{"Claude wants to access the network"};
    props.style = dframe::FrameStyle::Warning;
    props.content = vbox({
        hbox({
            text("Host: ") | dim,
            text(p.host_pattern) | bold,
        }),
        text(""),
        hbox({
            text("Allow access to ") | dim,
            text(p.host_pattern),
            text("?") | dim,
        }),
        text(""),
        hbox({
            text(" [y] Allow") | color(Color::Green),
            text("  [a] Always allow") | color(Color::Cyan),
            text("  [n] Deny") | color(Color::Red),
            text("  [Esc] Cancel") | dim,
        }),
    });
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandleSandboxPermissionEvent(
    dsys::SandboxPermissionPayload& p,
    const Event& event)
{
    if (event == Event::Character('y') || event == Event::Character('Y')) {
        if (p.on_response) p.on_response(true, false);
        return true;
    }
    if (event == Event::Character('a') || event == Event::Character('A')) {
        if (p.on_response) p.on_response(true, true);
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) p.on_response(false, false);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(false, false);
        return true;
    }
    if (event == Event::Return) {
        if (p.on_response) p.on_response(true, false);
        return true;
    }
    return false;
}

// ============================================================
// CostThreshold renderer
// ============================================================

[[nodiscard]] inline Element RenderCostThreshold(
    const dsys::CostThresholdPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = "Cost Threshold Reached";
    props.subtitle = "Session cost has exceeded your configured threshold.";
    props.style = dframe::FrameStyle::Warning;
    props.content = vbox({
        hbox({
            text("Current: ") | dim,
            text(std::format("${:.2f}", p.current_cost_usd)) | bold | color(Color::Yellow),
        }),
        hbox({
            text("Threshold: ") | dim,
            text(std::format("${:.2f}", p.cost_threshold_usd)),
        }),
        hbox({
            text("Model: ") | dim,
            text(p.model_name),
        }),
        text(""),
        hbox({
            text(" [c] Continue") | color(Color::Green),
            text("  [r] Reset counter") | color(Color::Cyan),
            text("  [q] Quit") | color(Color::Red),
        }),
    });
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandleCostThresholdEvent(
    dsys::CostThresholdPayload& p,
    const Event& event)
{
    if (event == Event::Character('c') || event == Event::Character('C')) {
        if (p.on_response) p.on_response(true, false);
        return true;
    }
    if (event == Event::Character('r') || event == Event::Character('R')) {
        if (p.on_response) p.on_response(true, true);
        return true;
    }
    if (event == Event::Character('q') || event == Event::Character('Q') ||
        event == Event::Escape) {
        if (p.on_response) p.on_response(false, false);
        return true;
    }
    return false;
}

// ============================================================
// IdleReturn renderer
// ============================================================

[[nodiscard]] inline Element RenderIdleReturn(
    const dsys::IdleReturnPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = "Welcome Back";
    props.subtitle = p.idle_minutes > 0
        ? std::format("Your session has been idle for {} minutes.", p.idle_minutes)
        : std::string{"Your session has been idle."};
    props.style = dframe::FrameStyle::Info;
    props.content = vbox({
        text("Would you like to resume where you left off?") | center,
        text(""),
        hbox({
            text(" [Enter] Resume") | color(Color::Green),
            text("  [n] Start new") | color(Color::Cyan),
        }) | center,
    });
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandleIdleReturnEvent(
    dsys::IdleReturnPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    if (event == Event::Escape) {
        // Esc = resume (treat as dismiss = resume)
        if (p.on_response) p.on_response(true);
        return true;
    }
    return false;
}

// ============================================================
// PromptDialog renderer (hook input)
// ============================================================

[[nodiscard]] inline Element RenderPromptDialog(
    const dsys::PromptDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = p.title.empty() ? std::string{"Input Required"} : p.title;
    props.style = dframe::FrameStyle::Info;

    auto value_display = p.default_value
        ? hbox({ text("[") | dim, text(*p.default_value), text("]") | dim })
        : text("");

    props.content = vbox({
        paragraph(p.prompt_text),
        text(""),
        value_display,
        text(""),
        hbox({
            text(" [Enter] Submit") | color(Color::Green),
            text("  [Esc] Cancel") | color(Color::Red),
        }),
    });
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandlePromptDialogEvent(
    dsys::PromptDialogPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(p.default_value);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(std::nullopt);
        return true;
    }
    return false;
}

// ============================================================
// Elicitation renderer (MCP structured input)
// ============================================================

[[nodiscard]] inline Element RenderElicitation(
    const dsys::ElicitationPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = "MCP Server Request";
    props.subtitle = std::string{p.server_name + " needs additional input"};
    props.style = dframe::FrameStyle::Info;
    props.content = vbox({
        paragraph(p.request_description),
        text(""),
        hbox({
            text(" Request ID: ") | dim,
            text(std::to_string(p.request_id)) | dim,
        }),
        text(""),
        hbox({
            text(" [Enter] Approve") | color(Color::Green),
            text("  [Esc] Deny") | color(Color::Red),
        }),
    });
    return dframe::DialogFrame(props, ctx.theme);
}

inline bool HandleElicitationEvent(
    dsys::ElicitationPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// Generic dialog renderer
// ============================================================

[[nodiscard]] inline Element RenderGenericDialog(
    const dsys::GenericDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.title = p.title;
    props.style = dframe::FrameStyle::Default;

    Elements buttons;
    for (size_t i = 0; i < p.buttons.size(); ++i) {
        bool is_default = p.default_button.has_value() &&
                         static_cast<int>(i) == *p.default_button;
        auto btn = hbox({
            text(" "),
            text(p.buttons[i]),
            text(" "),
        });
        if (is_default) {
            btn = btn | borderStyled(Color::Cyan) | color(Color::Cyan);
        } else {
            btn = btn | borderStyled(Color::GrayDark);
        }
        if (i > 0) buttons.push_back(text("  "));
        buttons.push_back(btn);
    }

    props.content = vbox({
        paragraph(p.message),
        text(""),
        hbox(buttons) | center,
    });
    return dframe::DialogFrame(props, ctx.theme);
}

// ============================================================
// Registry setup — register all default renderers
// ============================================================

/// Register all default dialog renderers into a registry.
/// Call this once at app startup.
void register_default_renderers(dsys::DialogRendererRegistry& registry) {
    // ToolPermission
    registry.register_dialog(
        dsys::DialogType::ToolPermission,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ToolPermissionPayload>(&payload);
            if (!p) return text("");
            return RenderToolPermission(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ToolPermissionPayload>(&payload);
            if (!p) return false;
            return HandleToolPermissionEvent(*p, event);
        }
    );

    // SandboxPermission
    registry.register_dialog(
        dsys::DialogType::SandboxPermission,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::SandboxPermissionPayload>(&payload);
            if (!p) return text("");
            return RenderSandboxPermission(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::SandboxPermissionPayload>(&payload);
            if (!p) return false;
            return HandleSandboxPermissionEvent(*p, event);
        }
    );

    // PromptDialog
    registry.register_dialog(
        dsys::DialogType::PromptDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::PromptDialogPayload>(&payload);
            if (!p) return text("");
            return RenderPromptDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::PromptDialogPayload>(&payload);
            if (!p) return false;
            return HandlePromptDialogEvent(*p, event);
        }
    );

    // Elicitation
    registry.register_dialog(
        dsys::DialogType::Elicitation,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ElicitationPayload>(&payload);
            if (!p) return text("");
            return RenderElicitation(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ElicitationPayload>(&payload);
            if (!p) return false;
            return HandleElicitationEvent(*p, event);
        }
    );

    // CostThreshold
    registry.register_dialog(
        dsys::DialogType::CostThreshold,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::CostThresholdPayload>(&payload);
            if (!p) return text("");
            return RenderCostThreshold(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::CostThresholdPayload>(&payload);
            if (!p) return false;
            return HandleCostThresholdEvent(*p, event);
        }
    );

    // IdleReturn
    registry.register_dialog(
        dsys::DialogType::IdleReturn,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::IdleReturnPayload>(&payload);
            if (!p) return text("");
            return RenderIdleReturn(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::IdleReturnPayload>(&payload);
            if (!p) return false;
            return HandleIdleReturnEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::default_renderers
