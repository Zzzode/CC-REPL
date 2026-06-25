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
import cc.ui.dialogs.sandbox_permission;
import cc.ui.permissions.single_prompt;
import cc.ui.permissions.components;
import cc.ui.design.theme;
import cc.ui.design.primitives;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.dialog_modern_renderer_stubs;

export namespace cc::ui::dialogs::default_renderers {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
namespace pc = cc::ui::permissions::components;
namespace sp = cc::ui::permissions::single_prompt;
using Theme = cc::ui::design::theme::Theme;

// ============================================================
// ToolPermission renderer + event handler
// ============================================================

namespace detail {
/// Lazily initialise and return the PromptState attached to a payload via the
/// type-erased ui_state field.  Subsequent calls reuse the same shared state
/// so button focus, checkboxes, and one-shot guard survive repaints.
inline std::shared_ptr<sp::PromptState> EnsurePromptState(
    const dsys::ToolPermissionPayload& p)
{
    auto st = std::static_pointer_cast<sp::PromptState>(p.ui_state);
    if (st) return st;
    st = std::make_shared<sp::PromptState>();
    return st;
}

/// Sync fields currently known only by the payload (decision shortcut bools,
/// current sandbox toggle) onto the PromptState before rendering or handling
/// an event.  Called on every render so mutations made through the event
/// handler remain visible.
inline void SyncPayloadToState(const dsys::ToolPermissionPayload& p,
                               sp::PromptState& st)
{
    st.props.tool_name = p.tool_name;
    st.props.action_kind = p.action_kind;
    st.props.risk_level = p.risk_level;
    st.props.description = p.description;
    st.props.affected_paths = p.affected_paths;
    if (p.workspace_root) st.props.workspace_root = *p.workspace_root;
    st.props.detail = p.detail;
    st.props.rule_match_explanation = p.rule_match_explanation;
    st.props.initial_sandbox_toggle = p.initial_sandbox_toggle;
    st.sandbox_toggle = p.initial_sandbox_toggle;
}

/// Emit a decision exactly once per prompt.  Enforces the TS contract:
/// on_abort wins over on_response if set, and once the guard fires any
/// subsequent decision key is swallowed.  Returns true on the first
/// successful emission; subsequent calls or a missing callback return false
/// so Arrow/Tab still appear handled but nothing fires.
inline bool EmitDecision(dsys::ToolPermissionPayload& p,
                         sp::PromptState& st,
                         sp::Decision d)
{
    if (st.callback_fired) return true; // swallow, but report handled
    st.callback_fired = true;

    const bool sandbox = st.sandbox_toggle;

    if (d == sp::Decision::Abort) {
        // Priority 1: on_abort, if set, fires alone.
        if (p.on_abort) {
            p.on_abort();
            return true;
        }
        // Priority 2: no on_abort → fall through to on_response(Abort, …)
        // so the consumer always receives SOME signal.
        if (p.on_response) p.on_response(sp::Decision::Abort, sandbox);
        return true;
    }

    if (d == sp::Decision::AlwaysAllow && !p.can_always_allow) {
        // "Always allow" disabled — treat as AllowOnce (matches TS guard).
        d = sp::Decision::AllowOnce;
    }

    if (p.on_response) {
        p.on_response(d, sandbox);
        return true;
    }
    return false; // no callback wired → handled but nothing fired
}

/// Map a focused-button index to a Decision.
inline sp::Decision FocusToDecision(int focused_button)
{
    switch (focused_button) {
        case 1:  return sp::Decision::Deny;
        case 2:  return sp::Decision::AlwaysAllow;
        case 3:  return sp::Decision::AlwaysDeny;
        case 0:
        default: return sp::Decision::AllowOnce;
    }
}
} // namespace

/// Render the ToolPermission dialog using the faithful SinglePrompt.
/// Lazily attaches a PromptState to the payload on first render so UI state
/// (focus, checkboxes, guard) persists across repaint + event cycles.
[[nodiscard]] inline Element RenderToolPermission(
    dsys::ToolPermissionPayload& p,
    const dsys::DialogRenderContext& /*ctx*/)
{
    if (!p.ui_state) p.ui_state = std::make_shared<sp::PromptState>();
    auto st = std::static_pointer_cast<sp::PromptState>(p.ui_state);
    detail::SyncPayloadToState(p, *st);
    return sp::RenderSinglePrompt(std::move(st));
}

/// ToolPermission event handler — shared-state aware.
///
/// Keyboard layout (faithful TS single-prompt):
///   y / Y      → Allow once
///   n / N      → Deny
///   a / A      → Always allow (respects p.can_always_allow)
///   d / D      → Always deny
///   s / S      → Toggle sandbox
///   1          → Toggle "always deny this tool" checkbox
///   2          → Toggle "always allow this tool" checkbox (if can_always_allow)
///   ← / →      → Cycle button focus
///   Tab / S-Tab→ Same as → / ←
///   Return     → Activate focused button
///   Esc        → on_abort first; otherwise on_response(Abort)
///
/// Implements the TS one-shot guard via PromptState::callback_fired so
/// exactly one terminal callback fires per prompt lifetime.
inline bool HandleToolPermissionEvent(
    dsys::ToolPermissionPayload& p,
    const Event& event)
{
    if (!p.ui_state) p.ui_state = std::make_shared<sp::PromptState>();
    auto st = std::static_pointer_cast<sp::PromptState>(p.ui_state);

    // ── Direct shortcuts ─────────────────────────────────────────────
    if (event == Event::Character('y') || event == Event::Character('Y')) {
        // 'y' with always_allow checkbox pre-checked → AlwaysAllow wins.
        if (st->always_allow_checkbox && p.can_always_allow) {
            return detail::EmitDecision(p, *st, sp::Decision::AlwaysAllow);
        }
        return detail::EmitDecision(p, *st, sp::Decision::AllowOnce);
    }
    if (event == Event::Character('n') || event == Event::Character('N')) {
        if (st->always_deny_checkbox) {
            return detail::EmitDecision(p, *st, sp::Decision::AlwaysDeny);
        }
        return detail::EmitDecision(p, *st, sp::Decision::Deny);
    }
    if (event == Event::Character('a') || event == Event::Character('A')) {
        if (!p.can_always_allow) return false;
        return detail::EmitDecision(p, *st, sp::Decision::AlwaysAllow);
    }
    if (event == Event::Character('d') || event == Event::Character('D')) {
        return detail::EmitDecision(p, *st, sp::Decision::AlwaysDeny);
    }

    // ── Checkbox toggles ─────────────────────────────────────────────
    if (event == Event::Character('1')) {
        st->always_deny_checkbox = !st->always_deny_checkbox;
        return true;
    }
    if (event == Event::Character('2')) {
        if (!p.can_always_allow) return false;
        st->always_allow_checkbox = !st->always_allow_checkbox;
        return true;
    }

    // ── Sandbox toggle ───────────────────────────────────────────────
    if (event == Event::Character('s') || event == Event::Character('S')) {
        st->sandbox_toggle = !st->sandbox_toggle;
        // Keep payload in sync so the value is visible on render / observers.
        p.initial_sandbox_toggle = st->sandbox_toggle;
        return true;
    }

    // ── Escape ───────────────────────────────────────────────────────
    if (event == Event::Escape) {
        return detail::EmitDecision(p, *st, sp::Decision::Abort);
    }

    // ── Focus movement / button activation ───────────────────────────
    constexpr int kNumButtons = 4; // 0 AllowOnce 1 Deny 2 AlwaysAllow 3 AlwaysDeny
    if (event == Event::ArrowRight || event == Event::Tab) {
        st->focused_button = (st->focused_button + 1) % kNumButtons;
        return true;
    }
    if (event == Event::ArrowLeft || event == Event::TabReverse) {
        st->focused_button = (st->focused_button - 1 + kNumButtons) % kNumButtons;
        return true;
    }
    if (event == Event::Return || event == Event::Character(' ')) {
        // Honor checkbox overrides as well on Enter, just like shortcuts.
        sp::Decision d = detail::FocusToDecision(st->focused_button);
        if (st->always_allow_checkbox && p.can_always_allow
            && d == sp::Decision::AllowOnce) {
            d = sp::Decision::AlwaysAllow;
        }
        if (st->always_deny_checkbox && d == sp::Decision::Deny) {
            d = sp::Decision::AlwaysDeny;
        }
        return detail::EmitDecision(p, *st, d);
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
    // Delegate to the faithful renderer port in cc.ui.dialogs.sandbox_permission
    // (see sandbox_permission.cppm for the 1:1 TS layout).
    return cc::ui::dialogs::sandbox_permission::RenderDefault(p, ctx);
}

inline bool HandleSandboxPermissionEvent(
    dsys::SandboxPermissionPayload& p,
    const Event& event)
{
    // Delegate to the faithful event handler (y/a/n/Esc/Enter + arrow nav).
    return cc::ui::dialogs::sandbox_permission::HandleSandboxPermissionEvent(
        p, event);
}

// ============================================================
// CostThreshold renderer
//
// UNIFIED: delegates EXCLUSIVELY to cost_threshold_dialog.cppm's
// RenderCostThreshold / HandleCostThresholdEvent to guarantee the
// P0x3 contract.  This renderer previously contained fabricated
// "Continue / Reset counter / Quit" chrome — that code has been
// permanently removed.
// ============================================================

[[nodiscard]] inline Element RenderCostThreshold(
    const dsys::CostThresholdPayload& p,
    const dsys::DialogRenderContext& /*ctx*/)
{
    namespace ct = cc::ui::dialogs::cost_threshold;
    ct::CostThresholdState st;
    st.dollars_spent = p.dollars_spent;
    st.model_name    = p.model_name;
    st.selected_index = 0;
    return ct::RenderCostThreshold(st);
}

inline bool HandleCostThresholdEvent(
    dsys::CostThresholdPayload& p,
    const Event& event)
{
    namespace ct = cc::ui::dialogs::cost_threshold;
    ct::CostThresholdState st;
    st.dollars_spent = p.dollars_spent;
    st.model_name    = p.model_name;
    st.selected_index = 0;
    st.on_done = [&p] { if (p.on_done) p.on_done(); };
    return ct::HandleCostThresholdEvent(st, event);
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

/// Generic dialog event handler — arrow nav across buttons + Enter on
/// focused, Esc dismisses (invokes default_button=none → -1 callback).
inline bool HandleGenericDialogEvent(
    dsys::GenericDialogPayload& p,
    const Event& event)
{
    const int n = static_cast<int>(p.buttons.size());
    if (n <= 0) return false;
    int focus = p.default_button.value_or(-1);
    if (focus < 0) focus = 0;
    if (event == Event::ArrowRight) {
        ++focus; if (focus >= n) focus = n - 1;
        p.default_button = focus;
        return true;
    }
    if (event == Event::ArrowLeft) {
        --focus; if (focus < 0) focus = 0;
        p.default_button = focus;
        return true;
    }
    if (event == Event::Return) {
        if (p.on_response) p.on_response(focus);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(-1);
        return true;
    }
    return false;
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
        [](dsys::DialogPayloadVariant& payload,
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
        [](dsys::DialogPayloadVariant& payload,
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
        [](dsys::DialogPayloadVariant& payload,
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
        [](dsys::DialogPayloadVariant& payload,
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
        [](dsys::DialogPayloadVariant& payload,
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
        [](dsys::DialogPayloadVariant& payload,
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

    // ─── Modern chrome stubs (6 dialogs; full FTXUI components tracked in M8)
    namespace mr = cc::ui::dialogs::modern_renderers;

    // ManagedSettingsSecurity
    registry.register_dialog(
        dsys::DialogType::ManagedSettingsSecurity,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ManagedSettingsSecurityPayload>(&payload);
            if (!p) return text("");
            return mr::RenderManagedSettingsSecurity(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ManagedSettingsSecurityPayload>(&payload);
            if (!p) return false;
            return mr::HandleManagedSettingsSecurityEvent(*p, event);
        }
    );

    // FeedbackSurvey
    registry.register_dialog(
        dsys::DialogType::FeedbackSurvey,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::FeedbackSurveyPayload>(&payload);
            if (!p) return text("");
            return mr::RenderFeedbackSurvey(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::FeedbackSurveyPayload>(&payload);
            if (!p) return false;
            return mr::HandleFeedbackSurveyEvent(*p, event);
        }
    );

    // GlobalSearch
    registry.register_dialog(
        dsys::DialogType::GlobalSearch,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::GlobalSearchPayload>(&payload);
            if (!p) return text("");
            return mr::RenderGlobalSearch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::GlobalSearchPayload>(&payload);
            if (!p) return false;
            return mr::HandleGlobalSearchEvent(*p, event);
        }
    );

    // HistorySearch
    registry.register_dialog(
        dsys::DialogType::HistorySearch,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::HistorySearchPayload>(&payload);
            if (!p) return text("");
            return mr::RenderHistorySearch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::HistorySearchPayload>(&payload);
            if (!p) return false;
            return mr::HandleHistorySearchEvent(*p, event);
        }
    );

    // PluginDialog
    registry.register_dialog(
        dsys::DialogType::PluginDialog,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return text("");
            return mr::RenderPluginDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return false;
            return mr::HandlePluginDialogEvent(*p, event);
        }
    );

    // DiffDialog
    registry.register_dialog(
        dsys::DialogType::DiffDialog,
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::DiffDialogPayload>(&payload);
            if (!p) return text("");
            return mr::RenderDiffDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::DiffDialogPayload>(&payload);
            if (!p) return false;
            return mr::HandleDiffDialogEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::default_renderers
