/// @file all_renderers.cppm
/// @brief Aggregator import: pulls in every renderer module, then re-exports
///        the union of Render* / Handle*Event functions under a single
///        namespace (cc::ui::dialogs::all_renderers).
///
/// TEST CONTRACT: unit tests alias
///   namespace dr = cc::ui::dialogs::all_renderers;
/// and write dr::RenderXxx(payload, ctx) / dr::HandleXxxEvent(p, e).
/// This module therefore re-exports every such function via explicit
/// `using` declarations so lookup resolves without ambiguous
/// namespace-qualification errors.
module;
#include <utility>
#include <vector>
export module cc.ui.dialogs.all_renderers;

import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.bottom_renderers;
import cc.ui.dialogs.modal_renderers;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.sandbox_permission;
import cc.ui.dialogs.quick_open;
import cc.ui.dialogs.dialog_modern_renderer_stubs;

export namespace cc::ui::dialogs::all_renderers {

// ── registration entry-points (kept for historical callers) ──────────────
using cc::ui::dialogs::default_renderers::register_default_renderers;

/// Named registration entry-point matching the module name.
template <typename... Args>
inline void register_all_renderers(Args&&... args) {
    cc::ui::dialogs::default_renderers::register_default_renderers(
        std::forward<Args>(args)...);
}

// ── Layer 1: 6 overlay/bottom dialogs from default_renderers ─────────────
// (ToolPermission, SandboxPermission, PromptDialog, Elicitation,
//  CostThreshold, IdleReturn, GenericDialog fallback)
using cc::ui::dialogs::default_renderers::RenderToolPermission;
using cc::ui::dialogs::default_renderers::HandleToolPermissionEvent;

using cc::ui::dialogs::default_renderers::RenderSandboxPermission;
using cc::ui::dialogs::default_renderers::HandleSandboxPermissionEvent;

using cc::ui::dialogs::default_renderers::RenderPromptDialog;
using cc::ui::dialogs::default_renderers::HandlePromptDialogEvent;

using cc::ui::dialogs::default_renderers::RenderElicitation;
using cc::ui::dialogs::default_renderers::HandleElicitationEvent;

using cc::ui::dialogs::default_renderers::RenderCostThreshold;
using cc::ui::dialogs::default_renderers::HandleCostThresholdEvent;

using cc::ui::dialogs::default_renderers::RenderIdleReturn;
using cc::ui::dialogs::default_renderers::HandleIdleReturnEvent;

using cc::ui::dialogs::default_renderers::RenderGenericDialog;
using cc::ui::dialogs::default_renderers::HandleGenericDialogEvent;

// ── Layer 2: faithful permission-panel modules ───────────────────────────
using cc::ui::dialogs::sandbox_permission::RenderDefault;
using cc::ui::dialogs::sandbox_permission::HandleSandboxPermissionEvent;

// ── Layer 3: quick_open (exact signatures already, direct using) ─────────
using cc::ui::dialogs::quick_open::RenderQuickOpen;
using cc::ui::dialogs::quick_open::HandleQuickOpenEvent;

// ── Layer 4: modern chrome stubs for 12 remaining test-addressed dialogs ─
// (ManagedSettingsSecurity, FeedbackSurvey, GlobalSearch, HistorySearch,
//  PluginDialog, DiffDialog — each Render+Handle pair)
namespace mr = cc::ui::dialogs::modern_renderers;

using mr::RenderManagedSettingsSecurity;
using mr::HandleManagedSettingsSecurityEvent;

using mr::RenderFeedbackSurvey;
using mr::HandleFeedbackSurveyEvent;

using mr::RenderGlobalSearch;
using mr::HandleGlobalSearchEvent;

using mr::RenderHistorySearch;
using mr::HandleHistorySearchEvent;

using mr::RenderPluginDialog;
using mr::HandlePluginDialogEvent;

using mr::RenderDiffDialog;
using mr::HandleDiffDialogEvent;

}  // namespace cc::ui::dialogs::all_renderers
