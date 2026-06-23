/// @file dialog_modal_renderers.cppm
/// @brief Renderers for modal and bottom-slot dialogs registered into
///        the DialogRendererRegistry.
///
/// MODULE:   cc.ui.dialogs.modal_renderers
/// LICENCE:  Exported.  Imported by app initialization alongside
///           default_renderers to register the full set of dialog renderers.
///
/// This module bridges existing standalone dialog implementations
/// (About, Confirmation, Bridge, WorktreeExit, RemoteEnv, DesktopUpsell)
/// into the DialogRendererRegistry system so they can be pushed onto
/// the DialogQueue and rendered via the standard queue pipeline.
///
/// Renderers provided here:
///   - BridgeDialog (modal slot) — IDE bridge connection status
///   - WorktreeExitDialog (modal slot) — worktree exit confirmation
///   - RemoteEnvDialog (modal slot) — remote environment status
///   - ConfirmationDialog (modal slot) — generic yes/no confirmation
///   - DesktopUpsell (bottom slot, band 6) — desktop app upgrade
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

export module cc.ui.dialogs.modal_renderers;

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.dialogs.confirmation;
import cc.ui.dialogs.bridge_dialog;
import cc.ui.dialogs.worktree_exit_dialog;
import cc.ui.dialogs.remote_env_dialog;
import cc.ui.dialogs.desktop_upsell;
import cc.ui.design.theme;
import cc.ui.design.tokens;

export namespace cc::ui::dialogs::modal_renderers {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
namespace confirm_ns = cc::ui::dialogs::confirmation;
// Note: bridge / worktree_exit / remote_env / desktop_upsell are in
// the ::cc::ui::dialogs namespace (not a sub-namespace).
using Theme = cc::ui::design::theme::Theme;
using Role = cc::ui::design::tokens::Role;

// ============================================================
// BridgeDialog
// ============================================================

/// Render the BridgeDialog using the existing bridge_dialog renderer.
[[nodiscard]] inline Element RenderBridgeDialog(
    const dsys::BridgeDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    cc::ui::dialogs::BridgeStatus status;
    status.ide_name = p.ide_name;
    status.is_connected = p.is_connected;
    status.version = p.version;
    status.error = p.error;
    return cc::ui::dialogs::render_bridge_dialog(status);
}

/// BridgeDialog event handler — esc/enter closes the dialog.
inline bool HandleBridgeDialogEvent(
    dsys::BridgeDialogPayload& p,
    const Event& event)
{
    if (event == Event::Escape || event == Event::Return) {
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// WorktreeExitDialog
// ============================================================

/// Render the WorktreeExitDialog using the existing renderer.
[[nodiscard]] inline Element RenderWorktreeExitDialog(
    const dsys::WorktreeExitPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    cc::ui::dialogs::WorktreeExitInfo info;
    info.worktree_path = p.worktree_path;
    info.branch = p.branch;
    info.has_uncommitted_changes = p.has_uncommitted_changes;
    info.modified_files = p.modified_files;
    return cc::ui::dialogs::render_worktree_exit(info);
}

/// WorktreeExitDialog event handler — y=exit, n=cancel, esc=cancel.
inline bool HandleWorktreeExitEvent(
    dsys::WorktreeExitPayload& p,
    const Event& event)
{
    if (event == Event::Character('y') || event == Event::Character('Y')) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N') ||
        event == Event::Escape) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// RemoteEnvDialog
// ============================================================

/// Render the RemoteEnvDialog using the existing renderer.
[[nodiscard]] inline Element RenderRemoteEnvDialog(
    const dsys::RemoteEnvPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    cc::ui::dialogs::RemoteEnvInfo info;
    info.host = p.host;
    info.user = p.user;
    info.port = p.port;
    info.is_connected = p.is_connected;
    return cc::ui::dialogs::render_remote_env(info);
}

/// RemoteEnvDialog event handler — esc/enter closes.
inline bool HandleRemoteEnvEvent(
    dsys::RemoteEnvPayload& p,
    const Event& event)
{
    if (event == Event::Escape || event == Event::Return) {
        if (p.on_close) p.on_close();
        return true;
    }
    return false;
}

// ============================================================
// ConfirmationDialog
// ============================================================

/// Render the ConfirmationDialog using the faithful confirmation renderer.
[[nodiscard]] inline Element RenderConfirmationDialog(
    const dsys::ConfirmationDialogPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    confirm_ns::ConfirmDialogProps props;
    props.title = p.title;
    props.message = p.message;
    props.style = p.is_destructive
        ? confirm_ns::ConfirmStyle::Danger
        : confirm_ns::ConfirmStyle::Info;
    props.buttons.yes = p.confirm_text;
    props.buttons.no = p.cancel_text;
    props.buttons.cancel = std::nullopt;  // 2-button for simple confirm
    props.default_button = p.is_destructive ? 1 : 0;

    confirm_ns::ConfirmDialogState state;
    state.focused_button = p.is_destructive ? 1 : 0;

    return confirm_ns::RenderConfirmDialog(props, state, ctx.theme);
}

/// ConfirmationDialog event handler — y=confirm, n/cancel=decline.
inline bool HandleConfirmationDialogEvent(
    dsys::ConfirmationDialogPayload& p,
    const Event& event)
{
    if (event == Event::Character('y') || event == Event::Character('Y') ||
        event == Event::Return) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Character('n') || event == Event::Character('N') ||
        event == Event::Escape) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// DesktopUpsell
// ============================================================

/// Render the DesktopUpsell dialog using the existing renderer.
/// This is a bottom-slot (band 6) dialog.
[[nodiscard]] inline Element RenderDesktopUpsell(
    const dsys::DesktopUpsellPayload& p,
    const dsys::DialogRenderContext& ctx)
{
    cc::ui::dialogs::DesktopUpsellProps props;
    props.features = cc::ui::dialogs::default_upsell_features();
    props.dialog_width = ctx.term_cols - 4;
    return cc::ui::dialogs::RenderDesktopUpsellDialog(
        props.features, 0, props.dialog_width);
}

/// DesktopUpsell event handler — enter=upgrade, esc/not-now=dismiss.
inline bool HandleDesktopUpsellEvent(
    dsys::DesktopUpsellPayload& p,
    const Event& event)
{
    if (event == Event::Return) {
        if (p.on_response) p.on_response(true);
        return true;
    }
    if (event == Event::Escape || event == Event::Character('n') ||
        event == Event::Character('N')) {
        if (p.on_response) p.on_response(false);
        return true;
    }
    return false;
}

// ============================================================
// Registry setup — register all modal/bottom dialog renderers
// ============================================================

/// Register all modal and bottom-slot dialog renderers into a registry.
/// Call this once at app startup alongside register_default_renderers().
void register_modal_renderers(dsys::DialogRendererRegistry& registry) {
    // BridgeDialog (modal)
    registry.register_dialog(
        dsys::DialogType::BridgeDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::BridgeDialogPayload>(&payload);
            if (!p) return text("");
            return RenderBridgeDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::BridgeDialogPayload>(&payload);
            if (!p) return false;
            return HandleBridgeDialogEvent(*p, event);
        }
    );

    // WorktreeExitDialog (modal)
    registry.register_dialog(
        dsys::DialogType::WorktreeExitDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::WorktreeExitPayload>(&payload);
            if (!p) return text("");
            return RenderWorktreeExitDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::WorktreeExitPayload>(&payload);
            if (!p) return false;
            return HandleWorktreeExitEvent(*p, event);
        }
    );

    // RemoteEnvDialog (modal)
    registry.register_dialog(
        dsys::DialogType::RemoteEnvDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::RemoteEnvPayload>(&payload);
            if (!p) return text("");
            return RenderRemoteEnvDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::RemoteEnvPayload>(&payload);
            if (!p) return false;
            return HandleRemoteEnvEvent(*p, event);
        }
    );

    // ConfirmationDialog (modal)
    registry.register_dialog(
        dsys::DialogType::ConfirmationDialog,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::ConfirmationDialogPayload>(&payload);
            if (!p) return text("");
            return RenderConfirmationDialog(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::ConfirmationDialogPayload>(&payload);
            if (!p) return false;
            return HandleConfirmationDialogEvent(*p, event);
        }
    );

    // DesktopUpsell (bottom slot, band 6)
    registry.register_dialog(
        dsys::DialogType::DesktopUpsell,
        [](const dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::DesktopUpsellPayload>(&payload);
            if (!p) return text("");
            return RenderDesktopUpsell(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::DesktopUpsellPayload>(&payload);
            if (!p) return false;
            return HandleDesktopUpsellEvent(*p, event);
        }
    );
}

} // namespace cc::ui::dialogs::modal_renderers
