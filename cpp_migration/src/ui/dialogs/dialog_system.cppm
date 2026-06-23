/// @file dialog_system.cppm
/// @brief Core dialog framework: type-safe dialog types, priority queue,
///        renderer registry, and slot-based rendering dispatch.
///
/// MODULE:   cc.ui.dialogs.system
/// LICENCE:  Exported.  Imported by REPL screen, dialog implementations,
///           and engine-side dialog lifecycle managers.
///
/// ARCHITECTURE (M7):
///   - DialogType: type tag enum (1:1 with TS dialog types)
///   - DialogSlot: which FullscreenLayout slot a dialog renders in
///   - DialogPriority: priority bands (mirrors TS getFocusedInputDialog order)
///   - DialogPayloadVariant: std::variant of all dialog payload structs
///   - DialogQueue: per-slot, per-priority-band FIFO queue with suppression
///   - DialogRendererRegistry: maps DialogType -> render function
///
/// The framework is designed so that:
///   1. Engine pushes dialog requests into the queue (with callbacks)
///   2. Renderer peeks the highest-priority non-suppressed dialog per slot
///   3. Renderer dispatches to the registered renderer for that dialog type
///   4. User input dispatches to dialog event handlers
///   5. Callbacks fire, dialogs pop, queue advances
module;

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.dialogs.system;

import cc.ui.design.theme;
import cc.ui.permissions.single_prompt;

export namespace cc::ui::dialogs::system {

using namespace ftxui;
using Theme = cc::ui::design::theme::Theme;

// ============================================================
// DialogType — type tag enum (1:1 with TS dialog types)
// ============================================================

/// Type tag for each dialog kind.  1:1 correspondence with TS dialog types.
/// Each type has an associated slot preference and priority band.
enum class DialogType : std::uint16_t {
    // -- overlay slot (inside ScrollBox) --
    ToolPermission,         ///< 'tool-permission' (band 3)

    // -- bottom slot (pinned below prompt) --
    MessageSelector,        ///< 'message-selector' (band 1, highest)
    SandboxPermission,      ///< 'sandbox-permission' (band 2)
    PromptDialog,           ///< 'prompt' hook input (band 3)
    WorkerSandboxPermission,///< 'worker-sandbox-permission' (band 3)
    Elicitation,            ///< 'elicitation' MCP input (band 3)
    CostThreshold,          ///< 'cost' threshold exceeded (band 4)
    IdleReturn,             ///< 'idle-return' welcome back (band 4)
    UltraplanChoice,        ///< 'ultraplan-choice' (band 4)
    UltraplanLaunch,        ///< 'ultraplan-launch' (band 4)
    IdeOnboarding,          ///< 'ide-onboarding' (band 5)
    InitOnboarding,         ///< 'init-onboarding' (band 5)
    ModelSwitch,            ///< 'model-switch' (band 5)
    UndercoverCallout,      ///< 'undercover-callout' auto-mode (band 5)
    EffortCallout,          ///< 'effort-callout' (band 5)
    RemoteCallout,          ///< 'remote-callout' (band 5)
    LspRecommendation,      ///< 'lsp-recommendation' (band 6, lowest)
    PluginHint,             ///< 'plugin-hint' (band 6)
    DesktopUpsell,          ///< 'desktop-upsell' (band 6)

    // -- modal slot (full-width dbox overlay, ▔ divider) --
    SettingsPanel,          ///< /settings panel
    TasksView,              ///< /tasks panel
    TeamsView,              ///< /teams panel
    HelpView,               ///< /help panel
    QuickOpen,              ///< /quick-open
    PluginDialog,           ///< /plugins
    MCPDialog,              ///< /mcp
    DiffDialog,             ///< /diff
    ConfigDialog,           ///< /config
    ExportDialog,           ///< /export
    GlobalSearch,           ///< /search
    HistorySearch,          ///< history search
    BridgeDialog,           ///< /bridge
    WorktreeExitDialog,     ///< worktree exit confirmation
    RemoteEnvDialog,        ///< /remote
    AboutDialog,            ///< about / credits
    ConfirmationDialog,     ///< generic yes/no confirmation
    FeedbackSurvey,         ///< feedback survey
    ManagedSettingsSecurity,///< managed settings security page

    // -- Standalone / full-screen (not in REPL layout) --
    TrustDialog,            ///< first-run trust dialog
    Onboarding,             ///< first-run onboarding wizard

    // -- Wizards (multi-step standalone) --
    InstallGitHubAppWizard, ///< 12-step GitHub app install
    InstallSlackAppWizard,  ///< Slack app install
    CreateAgentWizard,      ///< new agent wizard
    EditAgentWizard,        ///< edit agent wizard

    _COUNT,                 ///< sentinel
};

/// Human-readable name for a dialog type (for debugging / logging).
[[nodiscard]] inline std::string_view dialog_type_name(DialogType t) noexcept {
    switch (t) {
        case DialogType::ToolPermission:           return "tool-permission";
        case DialogType::MessageSelector:          return "message-selector";
        case DialogType::SandboxPermission:        return "sandbox-permission";
        case DialogType::PromptDialog:             return "prompt-dialog";
        case DialogType::WorkerSandboxPermission:  return "worker-sandbox-permission";
        case DialogType::Elicitation:              return "elicitation";
        case DialogType::CostThreshold:            return "cost-threshold";
        case DialogType::IdleReturn:               return "idle-return";
        case DialogType::UltraplanChoice:          return "ultraplan-choice";
        case DialogType::UltraplanLaunch:          return "ultraplan-launch";
        case DialogType::IdeOnboarding:            return "ide-onboarding";
        case DialogType::InitOnboarding:           return "init-onboarding";
        case DialogType::ModelSwitch:              return "model-switch";
        case DialogType::UndercoverCallout:        return "undercover-callout";
        case DialogType::EffortCallout:            return "effort-callout";
        case DialogType::RemoteCallout:            return "remote-callout";
        case DialogType::LspRecommendation:        return "lsp-recommendation";
        case DialogType::PluginHint:               return "plugin-hint";
        case DialogType::DesktopUpsell:            return "desktop-upsell";
        case DialogType::SettingsPanel:            return "settings-panel";
        case DialogType::TasksView:                return "tasks-view";
        case DialogType::TeamsView:                return "teams-view";
        case DialogType::HelpView:                 return "help-view";
        case DialogType::QuickOpen:                return "quick-open";
        case DialogType::PluginDialog:             return "plugin-dialog";
        case DialogType::MCPDialog:                return "mcp-dialog";
        case DialogType::DiffDialog:               return "diff-dialog";
        case DialogType::ConfigDialog:             return "config-dialog";
        case DialogType::ExportDialog:             return "export-dialog";
        case DialogType::GlobalSearch:             return "global-search";
        case DialogType::HistorySearch:            return "history-search";
        case DialogType::BridgeDialog:             return "bridge-dialog";
        case DialogType::WorktreeExitDialog:       return "worktree-exit-dialog";
        case DialogType::RemoteEnvDialog:          return "remote-env-dialog";
        case DialogType::AboutDialog:            return "about-dialog";
        case DialogType::ConfirmationDialog:   return "confirmation-dialog";
        case DialogType::FeedbackSurvey:           return "feedback-survey";
        case DialogType::ManagedSettingsSecurity:  return "managed-settings-security";
        case DialogType::TrustDialog:              return "trust-dialog";
        case DialogType::Onboarding:               return "onboarding";
        case DialogType::InstallGitHubAppWizard:   return "install-github-app-wizard";
        case DialogType::InstallSlackAppWizard:    return "install-slack-app-wizard";
        case DialogType::CreateAgentWizard:        return "create-agent-wizard";
        case DialogType::EditAgentWizard:          return "edit-agent-wizard";
        case DialogType::_COUNT:                   return "(count)";
    }
    return "unknown";
}

// ============================================================
// DialogSlot — which FullscreenLayout slot a dialog renders in
// ============================================================

/// Which FullscreenLayout slot a dialog renders in.
/// Mirrors TS slot architecture: overlay, bottom, modal, standalone.
enum class DialogSlot : std::uint8_t {
    Overlay,    ///< inside ScrollBox (tool-permission)
    Bottom,     ///< bottom slot — focusedInputDialog set
    Modal,      ///< modal slot — centered toolJSX panels
    Standalone, ///< full-screen — showSetupDialog / showDialog
};

/// Get the slot preference for a given dialog type.
[[nodiscard]] inline DialogSlot slot_for(DialogType type) noexcept {
    switch (type) {
        case DialogType::ToolPermission:
            return DialogSlot::Overlay;

        case DialogType::MessageSelector:
        case DialogType::SandboxPermission:
        case DialogType::PromptDialog:
        case DialogType::WorkerSandboxPermission:
        case DialogType::Elicitation:
        case DialogType::CostThreshold:
        case DialogType::IdleReturn:
        case DialogType::UltraplanChoice:
        case DialogType::UltraplanLaunch:
        case DialogType::IdeOnboarding:
        case DialogType::InitOnboarding:
        case DialogType::ModelSwitch:
        case DialogType::UndercoverCallout:
        case DialogType::EffortCallout:
        case DialogType::RemoteCallout:
        case DialogType::LspRecommendation:
        case DialogType::PluginHint:
        case DialogType::DesktopUpsell:
            return DialogSlot::Bottom;

        case DialogType::SettingsPanel:
        case DialogType::TasksView:
        case DialogType::TeamsView:
        case DialogType::HelpView:
        case DialogType::QuickOpen:
        case DialogType::PluginDialog:
        case DialogType::MCPDialog:
        case DialogType::DiffDialog:
        case DialogType::ConfigDialog:
        case DialogType::ExportDialog:
        case DialogType::GlobalSearch:
        case DialogType::HistorySearch:
        case DialogType::BridgeDialog:
        case DialogType::WorktreeExitDialog:
        case DialogType::RemoteEnvDialog:
        case DialogType::AboutDialog:
        case DialogType::ConfirmationDialog:
        case DialogType::FeedbackSurvey:
        case DialogType::ManagedSettingsSecurity:
            return DialogSlot::Modal;

        case DialogType::TrustDialog:
        case DialogType::Onboarding:
            return DialogSlot::Modal;

        case DialogType::InstallGitHubAppWizard:
        case DialogType::InstallSlackAppWizard:
        case DialogType::CreateAgentWizard:
        case DialogType::EditAgentWizard:
            return DialogSlot::Modal;

        case DialogType::_COUNT:
            return DialogSlot::Modal;
    }
    return DialogSlot::Modal;
}

// ============================================================
// DialogPriority — priority bands
// ============================================================

/// Priority band for bottom-slot dialogs.
/// Mirrors TS `getFocusedInputDialog()` priority order (REPL.tsx:2017).
///
/// Band 0 (highest) — implicit exit states
/// Band 1 — MessageSelector (never suppressed by typing)
/// Band 2 — SandboxPermission
/// Band 3 — ToolPermission, Prompt, WorkerSandbox, Elicitation
/// Band 4 — CostThreshold, IdleReturn, UltraplanChoice, UltraplanLaunch
/// Band 5 — IdeOnboarding, InitOnboarding, ModelSwitch, callouts
/// Band 6 (lowest) — LspRecommendation, PluginHint, DesktopUpsell
enum class DialogPriority : std::uint8_t {
    Exit = 0,       // highest (implicit)
    Band1 = 1,      // MessageSelector
    Band2 = 2,      // SandboxPermission
    Band3 = 3,      // Permissions, prompt, elicitation
    Band4 = 4,      // Cost, idle, ultraplan
    Band5 = 5,      // Onboarding, callouts, model switch
    Band6 = 6,      // Recommendations, hints, upsells (lowest)
};

/// Get the priority band for a given dialog type.
/// Only meaningful for bottom-slot and overlay-slot dialogs.
[[nodiscard]] inline DialogPriority priority_for(DialogType type) noexcept {
    switch (type) {
        case DialogType::MessageSelector:
            return DialogPriority::Band1;

        case DialogType::SandboxPermission:
            return DialogPriority::Band2;

        case DialogType::ToolPermission:
        case DialogType::PromptDialog:
        case DialogType::WorkerSandboxPermission:
        case DialogType::Elicitation:
            return DialogPriority::Band3;

        case DialogType::CostThreshold:
        case DialogType::IdleReturn:
        case DialogType::UltraplanChoice:
        case DialogType::UltraplanLaunch:
            return DialogPriority::Band4;

        case DialogType::IdeOnboarding:
        case DialogType::InitOnboarding:
        case DialogType::ModelSwitch:
        case DialogType::UndercoverCallout:
        case DialogType::EffortCallout:
        case DialogType::RemoteCallout:
            return DialogPriority::Band5;

        case DialogType::LspRecommendation:
        case DialogType::PluginHint:
        case DialogType::DesktopUpsell:
            return DialogPriority::Band6;

        default:
            // Modal and standalone dialogs don't use priority bands.
            return DialogPriority::Band3;
    }
}

/// Whether a dialog is suppressed while the user is actively typing.
/// Mirrors TS: only MessageSelector (band 1) shows while typing.
[[nodiscard]] inline bool is_suppressed_by_typing(DialogType type) noexcept {
    return priority_for(type) != DialogPriority::Band1;
}

// ============================================================
// DialogPayloadVariant — tagged union of all dialog payloads
// ============================================================

// Forward-declare payload structs.  Each dialog type defines its own
// payload type.  We start with the M7 set and extend as dialogs are ported.

/// Payload for ToolPermission dialog (overlay slot, band 3).
struct ToolPermissionPayload {
    using Decision = cc::ui::permissions::single_prompt::Decision;

    std::string id;                          ///< unique instance id
    std::string tool_name;                   ///< e.g. "BashTool"
    cc::ui::permissions::single_prompt::ActionKind action_kind
        = cc::ui::permissions::single_prompt::ActionKind::Other;
    cc::ui::permissions::single_prompt::RiskLevel risk_level
        = cc::ui::permissions::single_prompt::RiskLevel::Medium;
    std::string description;
    std::vector<std::string> affected_paths;
    std::optional<std::string> workspace_root;
    cc::ui::permissions::single_prompt::ToolDetail detail;
    std::string rule_match_explanation;
    bool can_always_allow = true;
    bool initial_sandbox_toggle = false;
    /// Callback invoked when the user makes a choice.
    std::function<void(Decision, bool sandbox)> on_response;
    /// Optional abort callback (Esc).
    std::function<void()> on_abort;
};

/// Payload for SandboxPermission dialog (bottom slot, band 2).
struct SandboxPermissionPayload {
    std::string id;
    std::string host_pattern;
    bool is_worker = false;
    std::string worker_request_id;
    std::function<void(bool allow, bool always)> on_response;
};

/// Payload for PromptDialog (hook input) — bottom slot, band 3.
struct PromptDialogPayload {
    std::string id;
    std::string title;
    std::string prompt_text;
    std::optional<std::string> default_value;
    std::function<void(std::optional<std::string> value)> on_response;
};

/// Payload for Elicitation (MCP structured input) — bottom slot, band 3.
struct ElicitationPayload {
    std::string id;
    std::string server_name;
    std::string request_description;
    std::uint64_t request_id = 0;
    std::function<void(bool approve)> on_response;
};

/// Payload for CostThreshold dialog — bottom slot, band 4.
struct CostThresholdPayload {
    std::string id;
    double cost_threshold_usd = 0.0;
    double current_cost_usd = 0.0;
    std::string model_name;
    std::function<void(bool continue_, bool reset)> on_response;
};

/// Payload for IdleReturn dialog — bottom slot, band 4.
struct IdleReturnPayload {
    std::string id;
    std::uint32_t idle_minutes = 0;
    std::function<void(bool resume)> on_response;
};

// Placeholder for other dialog types (added as they're ported)
struct GenericDialogPayload {
    std::string id;
    std::string title;
    std::string message;
    std::vector<std::string> buttons;
    std::optional<int> default_button;
    std::function<void(int button_index)> on_response;
};

// ---- Bottom-slot dialog payloads (M7.3) ----

/// Payload for LspRecommendation banner (bottom slot, band 6).
struct LspRecommendationPayload {
    std::string id;
    std::string server_name;
    std::function<void(bool install)> on_response;
};

/// Payload for PluginHint banner (bottom slot, band 6).
struct PluginHintPayload {
    std::string id;
    std::string plugin_name;
    std::string hint_text;
    std::function<void(bool enable)> on_response;
};

/// Payload for ModelSwitch banner (bottom slot, band 5).
struct ModelSwitchPayload {
    std::string id;
    std::string from_model;
    std::string to_model;
    std::function<void(bool confirm)> on_response;
};

/// Payload for UndercoverCallout banner (bottom slot, band 5).
struct UndercoverCalloutPayload {
    std::string id;
    bool is_active = false;
    std::function<void()> on_dismiss;
};

/// Payload for EffortCallout banner (bottom slot, band 5).
struct EffortCalloutPayload {
    std::string id;
    std::string effort_level;  // "low" | "medium" | "high"
    std::function<void()> on_dismiss;
};

/// Payload for RemoteCallout banner (bottom slot, band 5).
struct RemoteCalloutPayload {
    std::string id;
    std::string host;
    bool is_connected = false;
    std::function<void()> on_dismiss;
};

/// Payload for WorkerSandboxPermission (bottom slot, band 3).
struct WorkerSandboxPermissionPayload {
    std::string id;
    std::string tool_name;
    std::string worker_id;
    std::string description;
    std::function<void(bool allow, bool always)> on_response;
};

/// Payload for UltraplanChoice banner (bottom slot, band 4).
struct UltraplanChoicePayload {
    std::string id;
    std::string plan_name;
    std::string price_text;
    std::function<void(bool upgrade)> on_response;
};

/// Payload for UltraplanLaunch banner (bottom slot, band 4).
struct UltraplanLaunchPayload {
    std::string id;
    std::string plan_name;
    std::function<void(bool activate)> on_response;
};

/// Payload for IdeOnboarding banner (bottom slot, band 5).
struct IdeOnboardingPayload {
    std::string id;
    std::string ide_name;
    int step = 0;
    int total_steps = 3;
    std::function<void(bool dismiss)> on_response;
};

/// Payload for InitOnboarding banner (bottom slot, band 5).
struct InitOnboardingPayload {
    std::string id;
    int step = 0;
    int total_steps = 5;
    std::function<void(bool dismiss)> on_response;
};

// ---- Modal dialog payloads (M7.2) ----

/// Payload for BridgeDialog — IDE bridge connection status.
struct BridgeDialogPayload {
    std::string id;
    std::string ide_name;
    bool is_connected = false;
    std::optional<std::string> version;
    std::optional<std::string> error;
    std::function<void()> on_close;
};

/// Payload for WorktreeExitDialog — confirm worktree exit.
struct WorktreeExitPayload {
    std::string id;
    std::string worktree_path;
    std::string branch;
    bool has_uncommitted_changes = false;
    std::vector<std::string> modified_files;
    std::function<void(bool exit)> on_response;
};

/// Payload for RemoteEnvDialog — remote environment status.
struct RemoteEnvPayload {
    std::string id;
    std::string host;
    std::optional<std::string> user;
    std::optional<uint16_t> port;
    bool is_connected = false;
    std::function<void()> on_close;
};

/// Payload for AboutDialog — app info & credits.
struct AboutDialogPayload {
    std::string id;
    std::string version;
    std::string build_date;
    std::shared_ptr<void> component;  ///< Opaque component (optional, for high-fidelity mode)
    std::function<void()> on_close;
};

/// Payload for ConfirmationDialog — yes/no confirmation.
struct ConfirmationDialogPayload {
    std::string id;
    std::string title;
    std::string message;
    std::string confirm_text = "OK";
    std::string cancel_text = "Cancel";
    bool is_destructive = false;
    std::function<void(bool confirmed)> on_response;
};

/// Payload for DesktopUpsell — desktop app upgrade dialog.
struct DesktopUpsellPayload {
    std::string id;
    bool is_dismissed = false;
    std::function<void(bool upgrade)> on_response;
};

// ---- More modal dialog payloads (M7.4 — remaining modals) ----

/// Payload for MessageSelector (bottom slot, band 1).
struct MessageSelectorPayload {
    std::string id;
    std::vector<std::string> options;
    int selected_index = 0;
    std::string placeholder;
    std::function<void(int index)> on_select;
};

/// Payload for SettingsPanel modal.
struct SettingsPanelPayload {
    std::string id;
    std::string initial_tab;
    std::shared_ptr<void> component;  ///< Opaque component (optional, for high-fidelity mode)
    std::function<void()> on_close;
};

/// Payload for TasksView modal.
/// Payload for TasksView modal.
struct TasksViewPayload {
    std::string id;
    int selected_index = 0;
    std::function<void()> on_close;
    std::function<void(std::string_view task_id)> on_select;
};

/// Payload for TeamsView modal.
/// Payload for TeamsView modal.
struct TeamsViewPayload {
    std::string id;
    int selected_index = 0;
    std::function<void()> on_close;
};

/// Payload for HelpView modal.
struct HelpViewPayload {
    std::string id;
    std::string initial_section;
    std::shared_ptr<void> component;  ///< Opaque component (optional, for high-fidelity mode)
    std::function<void()> on_close;
};

/// An item in the quick-open palette
struct QuickOpenItem {
    std::string label;
    std::string description;
    std::optional<std::string> shortcut;
    std::string category;
};

/// Payload for QuickOpen modal (command palette).
struct QuickOpenPayload {
    std::string id;
    std::string query;
    std::vector<QuickOpenItem> items;
    int selected_index = 0;
    std::function<void(int index, bool confirmed)> on_result;
};

/// Payload for PluginDialog modal.
struct PluginDialogPayload {
    std::string id;
    int menu_selected = 0;
    std::function<void()> on_close;
};

/// Payload for MCPDialog modal.
struct MCPDialogPayload {
    std::string id;
    std::shared_ptr<void> component;  ///< Opaque component (optional, for high-fidelity mode)
    std::function<void()> on_close;
};

/// Payload for DiffDialog modal.
struct DiffDialogPayload {
    std::string id;
    std::string title;
    std::string file_path;
    std::string before_text;
    std::string after_text;
    int scroll_offset = 0;
    std::function<void(bool accept)> on_response;
    std::function<void()> on_close;
};

/// Payload for ConfigDialog modal.
struct ConfigDialogPayload {
    std::string id;
    std::shared_ptr<void> component;  ///< Opaque component (optional, for high-fidelity mode)
    std::function<void()> on_close;
};

/// Payload for ExportDialog modal.
struct ExportDialogPayload {
    std::string id;
    std::string format;
    std::function<void(bool export_)> on_response;
};

/// Payload for GlobalSearch modal.
struct GlobalSearchPayload {
    std::string id;
    std::string query;
    int selected_index = 0;
    std::function<void()> on_close;
};

/// Payload for HistorySearch modal.
struct HistorySearchPayload {
    std::string id;
    std::string query;
    int selected_index = 0;
    std::function<void(std::string_view conversation_id)> on_select;
};

/// Feedback survey state (mirrors cc::ui::feedback_survey::SurveyState)
enum class FeedbackSurveyState {
    Closed,
    Open,
    Thanks,
    TranscriptPrompt,
    Submitting,
    Submitted,
};

/// Payload for FeedbackSurvey modal.
struct FeedbackSurveyPayload {
    std::string id;
    FeedbackSurveyState state = FeedbackSurveyState::Open;
    std::string message;      // Custom message (optional)
    std::function<void()> on_close;
    std::function<void(int rating)> on_submit;  // rating: 1-3, or 0 for dismiss
};

/// Payload for ManagedSettingsSecurity modal.
struct ManagedSettingsSecurityPayload {
    std::string id;
    std::optional<std::string> organization_name;  ///< Name of managing org
    int selected_index = 0;                     ///< Selected category index
    std::function<void()> on_close;
};

/// Payload for TrustDialog (modal).
///
/// Uses a type-erased shared state so the dialog-system module does not
/// depend on the full trust_dialog implementation.  Renderers cast the
/// opaque pointer back to `trust_dialog::DialogState` via
/// `std::static_pointer_cast`.
struct TrustDialogPayload {
    std::string id;
    std::shared_ptr<void> state; ///< Opaque dialog state (type-erased)
};

/// Payload for Onboarding (standalone wizard).
///
/// Holds a type-erased component handle so the dialog-system module does
/// not depend on the full onboarding implementation.
struct OnboardingPayload {
    std::string id;
    int step = 0;
    std::shared_ptr<void> component; ///< Opaque wizard component
    std::function<void(bool complete)> on_complete;
};

/// Payload for InstallGitHubAppWizard.
struct InstallGitHubAppWizardPayload {
    std::string id;
    int step = 0;
    std::shared_ptr<void> component; ///< Opaque wizard component
    std::function<void(bool complete)> on_complete;
};

/// Payload for InstallSlackAppWizard.
struct InstallSlackAppWizardPayload {
    std::string id;
    int step = 0;
    std::shared_ptr<void> component; ///< Opaque wizard component
    std::function<void(bool complete)> on_complete;
};

/// Payload for CreateAgentWizard.
struct CreateAgentWizardPayload {
    std::string id;
    std::shared_ptr<void> component; ///< Opaque wizard component
    std::function<void(bool complete, std::string agent_name)> on_complete;
};

/// Payload for EditAgentWizard.
struct EditAgentWizardPayload {
    std::string id;
    std::string agent_name;
    std::shared_ptr<void> component; ///< Opaque wizard component
    std::function<void(bool complete)> on_complete;
};

/// Tagged union of all dialog payload types.
/// Use std::visit to dispatch on the active type.
using DialogPayloadVariant = std::variant<
    std::monostate,     // empty / no dialog
    ToolPermissionPayload,
    SandboxPermissionPayload,
    PromptDialogPayload,
    ElicitationPayload,
    CostThresholdPayload,
    IdleReturnPayload,
    GenericDialogPayload,
    LspRecommendationPayload,
    PluginHintPayload,
    ModelSwitchPayload,
    UndercoverCalloutPayload,
    EffortCalloutPayload,
    RemoteCalloutPayload,
    WorkerSandboxPermissionPayload,
    UltraplanChoicePayload,
    UltraplanLaunchPayload,
    IdeOnboardingPayload,
    InitOnboardingPayload,
    BridgeDialogPayload,
    WorktreeExitPayload,
    RemoteEnvPayload,
    AboutDialogPayload,
    ConfirmationDialogPayload,
    DesktopUpsellPayload,
    MessageSelectorPayload,
    SettingsPanelPayload,
    TasksViewPayload,
    TeamsViewPayload,
    HelpViewPayload,
    QuickOpenPayload,
    PluginDialogPayload,
    MCPDialogPayload,
    DiffDialogPayload,
    ConfigDialogPayload,
    ExportDialogPayload,
    GlobalSearchPayload,
    HistorySearchPayload,
    FeedbackSurveyPayload,
    ManagedSettingsSecurityPayload,
    TrustDialogPayload,
    OnboardingPayload,
    InstallGitHubAppWizardPayload,
    InstallSlackAppWizardPayload,
    CreateAgentWizardPayload,
    EditAgentWizardPayload
>;

/// Get the DialogType from a payload variant.
/// The monostate variant should never reach callers (empty queue returns nullopt).
[[nodiscard]] inline DialogType type_of(const DialogPayloadVariant& payload) {
    return std::visit([](const auto& p) -> DialogType {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return DialogType::_COUNT;
        } else if constexpr (std::is_same_v<T, ToolPermissionPayload>) {
            return DialogType::ToolPermission;
        } else if constexpr (std::is_same_v<T, SandboxPermissionPayload>) {
            return DialogType::SandboxPermission;
        } else if constexpr (std::is_same_v<T, PromptDialogPayload>) {
            return DialogType::PromptDialog;
        } else if constexpr (std::is_same_v<T, ElicitationPayload>) {
            return DialogType::Elicitation;
        } else if constexpr (std::is_same_v<T, CostThresholdPayload>) {
            return DialogType::CostThreshold;
        } else if constexpr (std::is_same_v<T, IdleReturnPayload>) {
            return DialogType::IdleReturn;
        } else if constexpr (std::is_same_v<T, GenericDialogPayload>) {
            // Generic dialog — caller decides slot via context
            return DialogType::_COUNT;
        } else if constexpr (std::is_same_v<T, LspRecommendationPayload>) {
            return DialogType::LspRecommendation;
        } else if constexpr (std::is_same_v<T, PluginHintPayload>) {
            return DialogType::PluginHint;
        } else if constexpr (std::is_same_v<T, ModelSwitchPayload>) {
            return DialogType::ModelSwitch;
        } else if constexpr (std::is_same_v<T, UndercoverCalloutPayload>) {
            return DialogType::UndercoverCallout;
        } else if constexpr (std::is_same_v<T, EffortCalloutPayload>) {
            return DialogType::EffortCallout;
        } else if constexpr (std::is_same_v<T, RemoteCalloutPayload>) {
            return DialogType::RemoteCallout;
        } else if constexpr (std::is_same_v<T, WorkerSandboxPermissionPayload>) {
            return DialogType::WorkerSandboxPermission;
        } else if constexpr (std::is_same_v<T, UltraplanChoicePayload>) {
            return DialogType::UltraplanChoice;
        } else if constexpr (std::is_same_v<T, UltraplanLaunchPayload>) {
            return DialogType::UltraplanLaunch;
        } else if constexpr (std::is_same_v<T, IdeOnboardingPayload>) {
            return DialogType::IdeOnboarding;
        } else if constexpr (std::is_same_v<T, InitOnboardingPayload>) {
            return DialogType::InitOnboarding;
        } else if constexpr (std::is_same_v<T, BridgeDialogPayload>) {
            return DialogType::BridgeDialog;
        } else if constexpr (std::is_same_v<T, WorktreeExitPayload>) {
            return DialogType::WorktreeExitDialog;
        } else if constexpr (std::is_same_v<T, RemoteEnvPayload>) {
            return DialogType::RemoteEnvDialog;
        } else if constexpr (std::is_same_v<T, AboutDialogPayload>) {
            return DialogType::AboutDialog;
        } else if constexpr (std::is_same_v<T, ConfirmationDialogPayload>) {
            return DialogType::ConfirmationDialog;
        } else if constexpr (std::is_same_v<T, DesktopUpsellPayload>) {
            return DialogType::DesktopUpsell;
        } else if constexpr (std::is_same_v<T, MessageSelectorPayload>) {
            return DialogType::MessageSelector;
        } else if constexpr (std::is_same_v<T, SettingsPanelPayload>) {
            return DialogType::SettingsPanel;
        } else if constexpr (std::is_same_v<T, TasksViewPayload>) {
            return DialogType::TasksView;
        } else if constexpr (std::is_same_v<T, TeamsViewPayload>) {
            return DialogType::TeamsView;
        } else if constexpr (std::is_same_v<T, HelpViewPayload>) {
            return DialogType::HelpView;
        } else if constexpr (std::is_same_v<T, QuickOpenPayload>) {
            return DialogType::QuickOpen;
        } else if constexpr (std::is_same_v<T, PluginDialogPayload>) {
            return DialogType::PluginDialog;
        } else if constexpr (std::is_same_v<T, MCPDialogPayload>) {
            return DialogType::MCPDialog;
        } else if constexpr (std::is_same_v<T, DiffDialogPayload>) {
            return DialogType::DiffDialog;
        } else if constexpr (std::is_same_v<T, ConfigDialogPayload>) {
            return DialogType::ConfigDialog;
        } else if constexpr (std::is_same_v<T, ExportDialogPayload>) {
            return DialogType::ExportDialog;
        } else if constexpr (std::is_same_v<T, GlobalSearchPayload>) {
            return DialogType::GlobalSearch;
        } else if constexpr (std::is_same_v<T, HistorySearchPayload>) {
            return DialogType::HistorySearch;
        } else if constexpr (std::is_same_v<T, FeedbackSurveyPayload>) {
            return DialogType::FeedbackSurvey;
        } else if constexpr (std::is_same_v<T, ManagedSettingsSecurityPayload>) {
            return DialogType::ManagedSettingsSecurity;
        } else if constexpr (std::is_same_v<T, TrustDialogPayload>) {
            return DialogType::TrustDialog;
        } else if constexpr (std::is_same_v<T, OnboardingPayload>) {
            return DialogType::Onboarding;
        } else if constexpr (std::is_same_v<T, InstallGitHubAppWizardPayload>) {
            return DialogType::InstallGitHubAppWizard;
        } else if constexpr (std::is_same_v<T, InstallSlackAppWizardPayload>) {
            return DialogType::InstallSlackAppWizard;
        } else if constexpr (std::is_same_v<T, CreateAgentWizardPayload>) {
            return DialogType::CreateAgentWizard;
        } else if constexpr (std::is_same_v<T, EditAgentWizardPayload>) {
            return DialogType::EditAgentWizard;
        }
        return DialogType::_COUNT;
    }, payload);
}

/// Get the unique instance id from a payload.
[[nodiscard]] inline std::string_view id_of(const DialogPayloadVariant& payload) {
    return std::visit([](const auto& p) -> std::string_view {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "";
        } else {
            return p.id;
        }
    }, payload);
}

/// Get the DialogSlot for a payload.
[[nodiscard]] inline DialogSlot slot_of(const DialogPayloadVariant& payload) {
    return slot_for(type_of(payload));
}

/// Get the DialogPriority for a payload.
[[nodiscard]] inline DialogPriority priority_of(const DialogPayloadVariant& payload) {
    return priority_for(type_of(payload));
}

// ============================================================
// DialogRenderContext — environmental data for all renderers
// ============================================================

/// Environmental data passed to every dialog renderer.
/// Carries theme, terminal size, and optional REPL state.
struct DialogRenderContext {
    int term_cols = 80;
    int term_rows = 24;
    Theme theme{};
    bool is_fullscreen = false;
    /// Optional — pointer to REPL screen state for dialogs that need it.
    /// Typed as `void*` to avoid a dependency cycle with repl_screen.
    const void* repl_state = nullptr;
};

// ============================================================
// DialogRenderer — render function type
// ============================================================

/// Render function signature: given a payload + context, return an Element.
using DialogRenderer = std::function<Element(const DialogPayloadVariant&,
                                              const DialogRenderContext&)>;

/// Event handler signature: given a payload + event, return whether handled.
/// Event handlers mutate the payload via shared state (the payload struct
/// holds state shared between render and event handling).
using DialogEventHandler = std::function<bool(DialogPayloadVariant&,
                                               const Event&)>;

// ============================================================
// DialogRendererRegistry — maps DialogType -> renderer + event handler
// ============================================================

/// Registry of renderers and event handlers for each dialog type.
/// Register all dialog renderers at startup.
class DialogRendererRegistry {
public:
    /// Register a renderer for a dialog type.
    void register_renderer(DialogType type, DialogRenderer renderer) {
        auto idx = static_cast<std::size_t>(type);
        if (idx >= renderers_.size()) {
            renderers_.resize(static_cast<std::size_t>(DialogType::_COUNT));
        }
        renderers_[idx] = std::move(renderer);
    }

    /// Register an event handler for a dialog type.
    void register_event_handler(DialogType type, DialogEventHandler handler) {
        auto idx = static_cast<std::size_t>(type);
        if (idx >= event_handlers_.size()) {
            event_handlers_.resize(static_cast<std::size_t>(DialogType::_COUNT));
        }
        event_handlers_[idx] = std::move(handler);
    }

    /// Register both a renderer and event handler.
    void register_dialog(DialogType type,
                         DialogRenderer renderer,
                         DialogEventHandler handler = {}) {
        register_renderer(type, std::move(renderer));
        if (handler) register_event_handler(type, std::move(handler));
    }

    /// Render a dialog payload using its registered renderer.
    /// Returns an empty element if no renderer is registered.
    [[nodiscard]] Element render(const DialogPayloadVariant& payload,
                                 const DialogRenderContext& ctx) const {
        auto type = type_of(payload);
        auto idx = static_cast<std::size_t>(type);
        if (idx >= renderers_.size() || !renderers_[idx]) {
            // Fallback: render a generic placeholder so the UI never goes blank
            return render_fallback(payload, ctx);
        }
        return renderers_[idx](payload, ctx);
    }

    /// Handle an event for a dialog payload using its registered handler.
    /// Returns true if the event was handled.
    bool handle_event(DialogPayloadVariant& payload, const Event& event) const {
        auto type = type_of(payload);
        auto idx = static_cast<std::size_t>(type);
        if (idx >= event_handlers_.size() || !event_handlers_[idx]) {
            return false;
        }
        return event_handlers_[idx](payload, event);
    }

private:
    std::vector<DialogRenderer> renderers_;
    std::vector<DialogEventHandler> event_handlers_;

    /// Fallback renderer for dialog types with no registered renderer.
    /// Shows a placeholder so the UI never goes blank.
    [[nodiscard]] Element render_fallback(const DialogPayloadVariant& payload,
                                          const DialogRenderContext& /*ctx*/) const {
        auto type_name = std::string{dialog_type_name(type_of(payload))};
        return window(
            text(" " + type_name + " ") | bold | color(Color::Yellow),
            vbox({
                text("Dialog not implemented yet") | dim | center,
                text(""),
                text("Press Esc to close") | dim | center,
            })
        ) | color(Color::Yellow);
    }
};

// ============================================================
// DialogQueue — per-slot priority FIFO with typing suppression
// ============================================================

/// Priority queue for dialogs.
///
/// Architecture:
///   - overlay slot: single deque (only ToolPermission)
///   - bottom slot: 6 priority bands (deques)
///   - modal slot: stack (navigation depth)
///   - standalone: N/A (handled separately)
///
/// The queue owns all pending dialog instances.
/// Push adds to the appropriate band.
/// Peek returns the highest-priority non-suppressed dialog for a slot.
/// Pop removes the current front of the highest-priority non-suppressed band.
class DialogQueue {
public:
    /// Push a dialog into the appropriate slot/based on its type.
    /// Returns the dialog's unique id (same as payload.id).
    std::string push(DialogPayloadVariant payload) {
        auto slot = slot_of(payload);
        auto prio = priority_of(payload);
        auto id = std::string{id_of(payload)};

        switch (slot) {
            case DialogSlot::Overlay:
                overlay_.push_back(std::move(payload));
                break;
            case DialogSlot::Bottom: {
                auto band_idx = static_cast<std::size_t>(prio) - 1; // 0-indexed
                if (band_idx >= bottom_bands_.size()) band_idx = bottom_bands_.size() - 1;
                bottom_bands_[band_idx].push_back(std::move(payload));
                break;
            }
            case DialogSlot::Modal:
                modal_stack_.push_back(std::move(payload));
                break;
            case DialogSlot::Standalone:
                // Standalone dialogs don't go in the queue
                break;
        }
        return id;
    }

    /// Remove a dialog by id (cancelled externally).
    void remove(std::string_view id) {
        if (id.empty()) return;

        // Overlay
        std::erase_if(overlay_, [id](const auto& p) {
            return id_of(p) == id;
        });

        // Bottom bands
        for (auto& band : bottom_bands_) {
            std::erase_if(band, [id](const auto& p) {
                return id_of(p) == id;
            });
        }

        // Modal stack
        std::erase_if(modal_stack_, [id](const auto& p) {
            return id_of(p) == id;
        });
    }

    // ---- Overlay slot ----

    /// Check if overlay slot has any pending dialogs.
    [[nodiscard]] bool has_overlay() const {
        return !overlay_.empty();
    }

    /// Peek the front of the overlay queue.
    [[nodiscard]] std::optional<std::reference_wrapper<const DialogPayloadVariant>>
    peek_overlay() const {
        if (overlay_.empty()) return std::nullopt;
        return std::cref(overlay_.front());
    }

    /// Mutable peek at the front of the overlay queue (for event dispatch).
    [[nodiscard]] std::optional<std::reference_wrapper<DialogPayloadVariant>>
    peek_overlay_mut() {
        if (overlay_.empty()) return std::nullopt;
        return std::ref(overlay_.front());
    }

    /// Pop the front of the overlay queue.
    void pop_overlay() {
        if (!overlay_.empty()) overlay_.pop_front();
    }

    // ---- Bottom slot ----

    /// Get the highest-priority non-suppressed dialog in the bottom slot.
    /// Returns nullopt if no dialogs or all are suppressed.
    [[nodiscard]] std::optional<std::reference_wrapper<const DialogPayloadVariant>>
    peek_bottom(bool is_prompt_input_active) const {
        auto* band = find_active_band(is_prompt_input_active);
        if (!band || band->empty()) return std::nullopt;
        return std::cref(band->front());
    }

    /// Mutable peek at the highest-priority non-suppressed bottom dialog.
    [[nodiscard]] std::optional<std::reference_wrapper<DialogPayloadVariant>>
    peek_bottom_mut(bool is_prompt_input_active) {
        auto* band = find_active_band(is_prompt_input_active);
        if (!band || band->empty()) return std::nullopt;
        return std::ref(band->front());
    }

    /// Pop the front of the highest-priority non-suppressed band.
    void pop_bottom(bool is_prompt_input_active) {
        auto* band = find_active_band(is_prompt_input_active);
        if (band && !band->empty()) band->pop_front();
    }

    /// Check if bottom slot has any pending dialogs (regardless of suppression).
    [[nodiscard]] bool has_any_bottom() const {
        for (const auto& band : bottom_bands_) {
            if (!band.empty()) return true;
        }
        return false;
    }

    // ---- Modal slot ----

    /// Peek the top of the modal stack.
    [[nodiscard]] std::optional<std::reference_wrapper<const DialogPayloadVariant>>
    peek_modal() const {
        if (modal_stack_.empty()) return std::nullopt;
        return std::cref(modal_stack_.back());
    }

    /// Mutable peek at the top of the modal stack (for event dispatch).
    [[nodiscard]] std::optional<std::reference_wrapper<DialogPayloadVariant>>
    peek_modal_mut() {
        if (modal_stack_.empty()) return std::nullopt;
        return std::ref(modal_stack_.back());
    }

    /// Pop the top of the modal stack.
    void pop_modal() {
        if (!modal_stack_.empty()) modal_stack_.pop_back();
    }

    /// Check if modal slot has any dialogs.
    [[nodiscard]] bool has_modal() const {
        return !modal_stack_.empty();
    }

    /// Push a modal dialog (replaces top if same type? no — always push).
    void push_modal(DialogPayloadVariant payload) {
        modal_stack_.push_back(std::move(payload));
    }

    // ---- Aggregate ----

    /// Total number of pending dialogs across all slots.
    [[nodiscard]] std::size_t total_size() const {
        std::size_t total = overlay_.size();
        for (const auto& band : bottom_bands_) total += band.size();
        total += modal_stack_.size();
        return total;
    }

    /// True if no pending dialogs in any slot.
    [[nodiscard]] bool empty() const {
        return total_size() == 0;
    }

    /// Clear all dialogs from all slots.
    void clear() {
        overlay_.clear();
        for (auto& band : bottom_bands_) band.clear();
        modal_stack_.clear();
    }

    /// Check if any slot contains a dialog of the given type.
    [[nodiscard]] bool contains_type(DialogType type) const {
        for (const auto& p : overlay_) {
            if (type_of(p) == type) return true;
        }
        for (const auto& band : bottom_bands_) {
            for (const auto& p : band) {
                if (type_of(p) == type) return true;
            }
        }
        for (const auto& p : modal_stack_) {
            if (type_of(p) == type) return true;
        }
        return false;
    }

private:
    // overlay slot: single deque (ToolPermission)
    std::deque<DialogPayloadVariant> overlay_;

    // bottom slot: 6 priority bands (band 1 = index 0, band 6 = index 5)
    static constexpr std::size_t kBottomBands = 6;
    std::array<std::deque<DialogPayloadVariant>, kBottomBands> bottom_bands_;

    // modal slot: stack (navigation depth)
    std::vector<DialogPayloadVariant> modal_stack_;

    /// Find the highest-priority non-empty non-suppressed bottom band.
    /// Returns nullptr if no active band.
    [[nodiscard]] const std::deque<DialogPayloadVariant>*
    find_active_band(bool is_prompt_input_active) const {
        for (std::size_t i = 0; i < bottom_bands_.size(); ++i) {
            if (bottom_bands_[i].empty()) continue;

            // Band 1 (index 0) = MessageSelector — never suppressed
            // All others suppressed while typing
            if (is_prompt_input_active && i > 0) continue;

            return &bottom_bands_[i];
        }
        return nullptr;
    }

    /// Mutable version of find_active_band.
    [[nodiscard]] std::deque<DialogPayloadVariant>*
    find_active_band(bool is_prompt_input_active) {
        auto* const_this = const_cast<const DialogQueue*>(this);
        return const_cast<std::deque<DialogPayloadVariant>*>(
            const_this->find_active_band(is_prompt_input_active));
    }
};

// ============================================================
// Convenience: should this dialog be shown?
// ============================================================

/// Determine if a dialog should be shown given typing state.
/// Mirrors TS suppression logic.
[[nodiscard]] inline bool should_show_dialog(const DialogPayloadVariant& dlg,
                                              bool is_prompt_input_active) {
    auto type = type_of(dlg);
    auto slot = slot_for(type);

    // Modal slot: always show when present
    if (slot == DialogSlot::Modal) return true;

    // Overlay slot: suppressed while typing (band 3)
    if (slot == DialogSlot::Overlay) {
        return !is_prompt_input_active;
    }

    // Bottom slot: suppressed only when typing AND dialog is suppressible
    return !(is_prompt_input_active && is_suppressed_by_typing(type));
}

} // namespace cc::ui::dialogs::system
