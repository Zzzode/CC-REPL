/// @file dialog_bottom_renderers.cppm
/// @brief Renderers for bottom-slot dialogs (bands 1–6) registered into
///        the DialogRendererRegistry.
///
/// MODULE:   cc.ui.dialogs.bottom_renderers
/// LICENCE:  Exported.  Imported by app initialization alongside
///           default_renderers and modal_renderers.
///
/// Bottom-slot dialogs appear as floating banners above the prompt
/// (or inside the ScrollBox for overlay slot).  They share a common
/// visual pattern: icon + message + optional action button(s).
///
/// Renderers provided here (M7.3):
///   - LspRecommendation (band 6) — LSP server recommendation hint
///   - PluginHint (band 6) — plugin suggestion hint
///   - DesktopUpsell (band 6) — desktop app upgrade banner
///   - ModelSwitch (band 5) — model switch confirmation banner
///   - UndercoverCallout (band 5) — auto-mode "undercover" callout
///   - EffortCallout (band 5) — effort level callout
///   - RemoteCallout (band 5) — remote environment callout
///   - IdeOnboarding (band 5) — IDE integration onboarding
///   - InitOnboarding (band 5) — initial onboarding banner
///   - WorkerSandboxPermission (band 3) — worker sandbox permission
///   - UltraplanChoice (band 4) — ultraplan upgrade choice
///   - UltraplanLaunch (band 4) — ultraplan launch confirmation
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

export module cc.ui.dialogs.bottom_renderers;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::bottom_renderers {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Render a standard bottom-slot banner with icon + message + optional action.
/// Bottom-slot dialogs use DialogFrame with full_border=false for the
/// "floating banner" look that sits above the prompt.
[[nodiscard]] inline Element RenderBottomBanner(
    const std::string& icon,
    const std::string& message,
    const std::optional<std::string>& action_label,
    dframe::FrameStyle style,
    const Theme& theme)
{
    dframe::DialogFrameProps props;
    props.style = style;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    Elements content_els;

    // Icon + message row
    auto icon_el = text(icon);
    auto msg_el = text(message) | flex;

    Elements row_els;
    row_els.push_back(icon_el);
    row_els.push_back(text(" "));
    row_els.push_back(msg_el);

    if (action_label && !action_label->empty()) {
        row_els.push_back(text("  "));
        row_els.push_back(
            text(*action_label) | bold
            | color(theme.color_for(Role::Info))
            | underlined);
    }

    content_els.push_back(hbox(std::move(row_els)));

    props.content = vbox(std::move(content_els));
    return dframe::DialogFrame(props, theme);
}

} // namespace detail

// ============================================================
// LspRecommendation (band 6)
// ============================================================

/// Render LspRecommendation banner.
[[nodiscard]] inline Element RenderLspRecommendation(
    const dsys::LspRecommendationPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    return detail::RenderBottomBanner(
        "💡",
        "Install " + p.server_name + " language server for better IDE features",
        "Install",
        dframe::FrameStyle::Info,
        ctx.theme);
}

/// LspRecommendation event handler.
inline bool HandleLspRecommendationEvent(
    dsys::LspRecommendationPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Escape || event == Event::Character('d') ||
        event == Event::Character('D')) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// PluginHint (band 6)
// ============================================================

/// Render PluginHint banner.
[[nodiscard]] inline Element RenderPluginHint(
    const dsys::PluginHintPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    std::string msg = p.hint_text.empty()
        ? "Enable " + p.plugin_name + " plugin for enhanced functionality"
        : p.hint_text;
    return detail::RenderBottomBanner(
        "🔌",
        msg,
        "Enable",
        dframe::FrameStyle::Info,
        ctx.theme);
}

/// PluginHint event handler.
inline bool HandlePluginHintEvent(
    dsys::PluginHintPayload& p,
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
// ModelSwitch (band 5)
// ============================================================

/// Render ModelSwitch banner.
[[nodiscard]] inline Element RenderModelSwitch(
    const dsys::ModelSwitchPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Info;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text("🔄 "),
        text("Switching model: ") | dim,
        text(p.from_model) | dim | strikethrough,
        text(" → "),
        text(p.to_model) | bold,
        filler(),
        text("[Confirm] ") | color(ctx.theme.color_for(Role::Success)),
        text("[Esc cancel]") | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// ModelSwitch event handler.
inline bool HandleModelSwitchEvent(
    dsys::ModelSwitchPayload& p,
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
// UndercoverCallout (band 5)
// ============================================================

/// Render UndercoverCallout banner.
[[nodiscard]] inline Element RenderUndercoverCallout(
    const dsys::UndercoverCalloutPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    std::string msg = p.is_active
        ? "🕵️  Undercover mode active — Claude is working autonomously"
        : "🕵️  Undercover mode — Claude works without showing thinking";
    return detail::RenderBottomBanner(
        "",
        msg,
        "Got it",
        dframe::FrameStyle::Warning,
        ctx.theme);
}

/// UndercoverCallout event handler.
inline bool HandleUndercoverCalloutEvent(
    dsys::UndercoverCalloutPayload& p,
    const Event& event)
{
    if (event == Event::Return || event == Event::Escape ||
        event == Event::Character(' ')) {
        if (p.on_dismiss) p.on_dismiss();
        return true;
    }
    return false;
}

// ============================================================
// EffortCallout (band 5)
// ============================================================

/// Render EffortCallout banner.
[[nodiscard]] inline Element RenderEffortCallout(
    const dsys::EffortCalloutPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    return detail::RenderBottomBanner(
        "⚡",
        "Effort level: " + p.effort_level + " — tap to adjust",
        "Change",
        dframe::FrameStyle::Info,
        ctx.theme);
}

/// EffortCallout event handler.
inline bool HandleEffortCalloutEvent(
    dsys::EffortCalloutPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_dismiss) p.on_dismiss();
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_dismiss) p.on_dismiss();
        return true;
    }
    return false;
}

// ============================================================
// RemoteCallout (band 5)
// ============================================================

/// Render RemoteCallout banner.
[[nodiscard]] inline Element RenderRemoteCallout(
    const dsys::RemoteCalloutPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    auto status_color = p.is_connected ? Role::Success : Role::Warning;
    std::string status_text = p.is_connected ? "connected" : "connecting…";
    std::string msg =
        p.is_connected
            ? "🌐 Remote mode active — running commands on " + p.host
            : "🌐 Connecting to remote environment " + p.host + "…";

    dframe::DialogFrameProps props;
    props.style = p.is_connected
        ? dframe::FrameStyle::Success
        : dframe::FrameStyle::Warning;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text(msg),
        filler(),
        text("[" + status_text + "]") | color(ctx.theme.color_for(status_color)),
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// RemoteCallout event handler.
inline bool HandleRemoteCalloutEvent(
    dsys::RemoteCalloutPayload& p,
    const Event& event)
{
    if (event == Event::Return || event == Event::Escape) {
        if (p.on_dismiss) p.on_dismiss();
        return true;
    }
    return false;
}

// ============================================================
// ============================================================
// WorkerSandboxPermission (band 3)
// ============================================================

/// Render WorkerSandboxPermission banner.
[[nodiscard]] inline Element RenderWorkerSandboxPermission(
    const dsys::WorkerSandboxPermissionPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Warning;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = vbox({
        hbox({
            text("⚠️ ") | color(Color::Yellow),
            text("Worker wants to use " + p.tool_name) | bold,
            filler(),
            text("[y] allow ") | dim,
            text("[n] deny") | dim,
        }),
        text("  Worker: " + p.worker_id) | dim,
        text("  " + p.description) | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// WorkerSandboxPermission event handler.
inline bool HandleWorkerSandboxPermissionEvent(
    dsys::WorkerSandboxPermissionPayload& p,
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
    if (event == Event::Character('n') || event == Event::Character('N') ||
        event == Event::Escape) {
        if (p.on_response) p.on_response(false, false);
        return true;
    }
    return false;
}

// ============================================================
// UltraplanChoice (band 4)
// ============================================================

/// Render UltraplanChoice banner.
[[nodiscard]] inline Element RenderUltraplanChoice(
    const dsys::UltraplanChoicePayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Success;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text("✨ ") | color(Color::Green),
        text("Upgrade to " + p.plan_name + "? ") | bold,
        text(p.price_text) | dim,
        filler(),
        text("[Upgrade] ") | color(ctx.theme.color_for(Role::Success)),
        text("[Esc dismiss]") | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// UltraplanChoice event handler.
inline bool HandleUltraplanChoiceEvent(
    dsys::UltraplanChoicePayload& p,
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
// UltraplanLaunch (band 4)
// ============================================================

/// Render UltraplanLaunch banner.
[[nodiscard]] inline Element RenderUltraplanLaunch(
    const dsys::UltraplanLaunchPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Success;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text("🚀 ") | color(Color::Green),
        text(p.plan_name + " is now active ") | bold | color(ctx.theme.color_for(Role::Success)),
        filler(),
        text("[Got it]") | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// UltraplanLaunch event handler.
inline bool HandleUltraplanLaunchEvent(
    dsys::UltraplanLaunchPayload& p,
    const Event& event)
{
    if (event == Event::Return || event == Event::Escape ||
        event == Event::Character(' ')) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    return false;
}

// ============================================================
// IdeOnboarding (band 5)
// ============================================================

/// Render IdeOnboarding banner.
[[nodiscard]] inline Element RenderIdeOnboarding(
    const dsys::IdeOnboardingPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    std::string progress =
        "Step " + std::to_string(p.step + 1) + "/" + std::to_string(p.total_steps);

    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Info;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text("👋 ") | color(Color::Cyan),
        text("Welcome to " + p.ide_name + " extension — ") | bold,
        text(progress) | dim,
        filler(),
        text("[Next] ") | color(ctx.theme.color_for(Role::Info)),
        text("[Esc skip]") | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// IdeOnboarding event handler.
inline bool HandleIdeOnboardingEvent(
    dsys::IdeOnboardingPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    return false;
}

// ============================================================
// InitOnboarding (band 5)
// ============================================================

/// Render InitOnboarding banner.
[[nodiscard]] inline Element RenderInitOnboarding(
    const dsys::InitOnboardingPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    std::string progress =
        "Step " + std::to_string(p.step + 1) + "/" + std::to_string(p.total_steps);

    dframe::DialogFrameProps props;
    props.style = dframe::FrameStyle::Info;
    props.full_border = false;
    props.inner_padding_x = 2;
    props.inner_padding_y = 0;

    auto content = hbox({
        text("🌟 ") | color(Color::Cyan),
        text("Let's get Claude Code set up — ") | bold,
        text(progress) | dim,
        filler(),
        text("[Continue] ") | color(ctx.theme.color_for(Role::Info)),
        text("[Esc skip]") | dim,
    });

    props.content = content;
    return dframe::DialogFrame(props, ctx.theme);
}

/// InitOnboarding event handler.
inline bool HandleInitOnboardingEvent(
    dsys::InitOnboardingPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    if (event == Event::Escape) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    return false;
}

// ============================================================
// Registry setup — register all bottom-slot dialog renderers
// ============================================================

/// Register all bottom-slot dialog renderers into a registry.
/// Call this once at app startup alongside register_default_renderers()
/// and register_modal_renderers().
void register_bottom_renderers(dsys::DialogRendererRegistry& registry) {
    // LspRecommendation (band 6)
    registry.register_dialog(
        dsys::DialogType::LspRecommendation,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::LspRecommendationPayload>(&payload);
            if (!p) return text("");
            return RenderLspRecommendation(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::LspRecommendationPayload>(&payload);
            if (!p) return false;
            return HandleLspRecommendationEvent(*p, event);
        }
    );

    // PluginHint (band 6)
    registry.register_dialog(
        dsys::DialogType::PluginHint,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::PluginHintPayload>(&payload);
            if (!p) return text("");
            return RenderPluginHint(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::PluginHintPayload>(&payload);
            if (!p) return false;
            return HandlePluginHintEvent(*p, event);
        }
    );

    // ModelSwitch (band 5)
    registry.register_dialog(
        dsys::DialogType::ModelSwitch,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ModelSwitchPayload>(&payload);
            if (!p) return text("");
            return RenderModelSwitch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ModelSwitchPayload>(&payload);
            if (!p) return false;
            return HandleModelSwitchEvent(*p, event);
        }
    );

    // UndercoverCallout (band 5)
    registry.register_dialog(
        dsys::DialogType::UndercoverCallout,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::UndercoverCalloutPayload>(&payload);
            if (!p) return text("");
            return RenderUndercoverCallout(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::UndercoverCalloutPayload>(&payload);
            if (!p) return false;
            return HandleUndercoverCalloutEvent(*p, event);
        }
    );

    // EffortCallout (band 5)
    registry.register_dialog(
        dsys::DialogType::EffortCallout,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::EffortCalloutPayload>(&payload);
            if (!p) return text("");
            return RenderEffortCallout(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::EffortCalloutPayload>(&payload);
            if (!p) return false;
            return HandleEffortCalloutEvent(*p, event);
        }
    );

    // RemoteCallout (band 5)
    registry.register_dialog(
        dsys::DialogType::RemoteCallout,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::RemoteCalloutPayload>(&payload);
            if (!p) return text("");
            return RenderRemoteCallout(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::RemoteCalloutPayload>(&payload);
            if (!p) return false;
            return HandleRemoteCalloutEvent(*p, event);
        }
    );

    // WorkerSandboxPermission (band 3)
    registry.register_dialog(
        dsys::DialogType::WorkerSandboxPermission,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::WorkerSandboxPermissionPayload>(&payload);
            if (!p) return text("");
            return RenderWorkerSandboxPermission(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::WorkerSandboxPermissionPayload>(&payload);
            if (!p) return false;
            return HandleWorkerSandboxPermissionEvent(*p, event);
        }
    );

    // UltraplanChoice (band 4)
    registry.register_dialog(
        dsys::DialogType::UltraplanChoice,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::UltraplanChoicePayload>(&payload);
            if (!p) return text("");
            return RenderUltraplanChoice(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::UltraplanChoicePayload>(&payload);
            if (!p) return false;
            return HandleUltraplanChoiceEvent(*p, event);
        }
    );

    // UltraplanLaunch (band 4)
    registry.register_dialog(
        dsys::DialogType::UltraplanLaunch,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::UltraplanLaunchPayload>(&payload);
            if (!p) return text("");
            return RenderUltraplanLaunch(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::UltraplanLaunchPayload>(&payload);
            if (!p) return false;
            return HandleUltraplanLaunchEvent(*p, event);
        }
    );

    // IdeOnboarding (band 5)
    registry.register_dialog(
        dsys::DialogType::IdeOnboarding,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::IdeOnboardingPayload>(&payload);
            if (!p) return text("");
            return RenderIdeOnboarding(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::IdeOnboardingPayload>(&payload);
            if (!p) return false;
            return HandleIdeOnboardingEvent(*p, event);
        }
    );

    // InitOnboarding (band 5)
    registry.register_dialog(
        dsys::DialogType::InitOnboarding,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::InitOnboardingPayload>(&payload);
            if (!p) return text("");
            return RenderInitOnboarding(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::InitOnboardingPayload>(&payload);
            if (!p) return false;
            return HandleInitOnboardingEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::bottom_renderers
