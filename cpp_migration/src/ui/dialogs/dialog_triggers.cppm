/// @file dialog_triggers.cppm
/// @brief Engine-side API for pushing dialogs onto the DialogQueue.
///        Faithful port of TS dialog-trigger utilities — the engine/app
///        layer calls these to show a dialog without knowing about ReplMode.
///
/// MODULE:   cc.ui.dialogs.triggers
/// LICENCE:  Exported.  Imported by app.cppm, query engine, and services
///           that need to show dialogs to the user.
///
/// TS REFERENCE:
///   src/utils/dialogLaunchers.tsx (standalone pattern)
///   REPL.tsx dialog state transitions (queue-integrated pattern)
///
/// USAGE:
///   auto payload = cc::ui::dialogs::triggers::MakeToolPermissionPayload(
///       "Bash", "Run ls -la",
///       [](bool allow, bool always) { /* handle result */ });
///   dialog_queue.push(std::move(payload));
///
/// Or use the convenience push wrappers:
///   cc::ui::dialogs::triggers::PushToolPermission(
///       dialog_queue, "Bash", "Run ls -la", callback);
///
/// All trigger functions are non-blocking — they push the dialog onto
/// the queue and return immediately.  The callback fires when the user
/// makes a choice.
module;

#include <functional>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.ui.dialogs.triggers;

import cc.ui.dialogs.system;
import cc.ui.trust_dialog;
import cc.ui.trust_utils;

export namespace cc::ui::dialogs::triggers {

namespace dsys = cc::ui::dialogs::system;

// ============================================================
// Overlay-slot dialogs (band 3 — suppressed while typing)
// ============================================================

/// Build a ToolPermission payload.
/// This is the primary engine-triggered dialog for tool-use permissions.
[[nodiscard]] inline dsys::ToolPermissionPayload MakeToolPermissionPayload(
    std::string tool_name,
    std::string description,
    std::function<void(dsys::ToolPermissionPayload::Decision, bool sandbox)> on_response,
    std::function<void()> on_abort = nullptr,
    bool can_always_allow = true)
{
    dsys::ToolPermissionPayload p;
    p.id = "perm_" + tool_name + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.tool_name = std::move(tool_name);
    p.description = std::move(description);
    p.can_always_allow = can_always_allow;
    p.on_response = std::move(on_response);
    p.on_abort = std::move(on_abort);
    return p;
}

/// Push a ToolPermission dialog onto the queue.
inline void PushToolPermission(
    dsys::DialogQueue& queue,
    std::string tool_name,
    std::string description,
    std::function<void(dsys::ToolPermissionPayload::Decision, bool sandbox)> on_response,
    std::function<void()> on_abort = nullptr,
    bool can_always_allow = true)
{
    queue.push(MakeToolPermissionPayload(
        std::move(tool_name), std::move(description),
        std::move(on_response), std::move(on_abort), can_always_allow));
}

// ============================================================
// Bottom-slot dialogs (bands 1–6 — priority-ordered)
// ============================================================

// -- Band 1: never suppressed by typing --

/// Build a MessageSelector payload (completion picker / message chooser).
/// Band 1 — never suppressed by typing.
[[nodiscard]] inline dsys::MessageSelectorPayload MakeMessageSelectorPayload(
    std::vector<std::string> options,
    std::string placeholder,
    std::function<void(int index)> on_select)
{
    dsys::MessageSelectorPayload p;
    p.id = "msgsel_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    p.options = std::move(options);
    p.placeholder = std::move(placeholder);
    p.on_select = std::move(on_select);
    return p;
}

/// Push a MessageSelector dialog onto the queue.
inline void PushMessageSelector(
    dsys::DialogQueue& queue,
    std::vector<std::string> options,
    std::string placeholder,
    std::function<void(int index)> on_select)
{
    queue.push(MakeMessageSelectorPayload(
        std::move(options), std::move(placeholder), std::move(on_select)));
}

// -- Band 3: suppressed while typing --

/// Build a SandboxPermission payload (network sandbox request).
/// Band 3 — suppressed while typing.
[[nodiscard]] inline dsys::SandboxPermissionPayload MakeSandboxPermissionPayload(
    std::string host_pattern,
    std::function<void(bool allow, bool always)> on_response,
    bool is_worker = false,
    std::string worker_request_id = {})
{
    dsys::SandboxPermissionPayload p;
    p.id = "sbx_" + host_pattern + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.host_pattern = std::move(host_pattern);
    p.is_worker = is_worker;
    p.worker_request_id = std::move(worker_request_id);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a SandboxPermission dialog onto the queue.
inline void PushSandboxPermission(
    dsys::DialogQueue& queue,
    std::string host_pattern,
    std::function<void(bool allow, bool always)> on_response,
    bool is_worker = false,
    std::string worker_request_id = {})
{
    queue.push(MakeSandboxPermissionPayload(
        std::move(host_pattern), std::move(on_response),
        is_worker, std::move(worker_request_id)));
}

/// Build a WorkerSandboxPermission payload.
/// Band 3 — suppressed while typing.
[[nodiscard]] inline dsys::WorkerSandboxPermissionPayload
MakeWorkerSandboxPermissionPayload(
    std::string tool_name,
    std::string worker_id,
    std::string description,
    std::function<void(bool allow, bool always)> on_response)
{
    dsys::WorkerSandboxPermissionPayload p;
    p.id = "wsbx_" + worker_id + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.tool_name = std::move(tool_name);
    p.worker_id = std::move(worker_id);
    p.description = std::move(description);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a WorkerSandboxPermission dialog onto the queue.
inline void PushWorkerSandboxPermission(
    dsys::DialogQueue& queue,
    std::string tool_name,
    std::string worker_id,
    std::string description,
    std::function<void(bool allow, bool always)> on_response)
{
    queue.push(MakeWorkerSandboxPermissionPayload(
        std::move(tool_name), std::move(worker_id),
        std::move(description), std::move(on_response)));
}

/// Build a PromptDialog payload (hook / user input prompt).
/// Band 3 — suppressed while typing.
[[nodiscard]] inline dsys::PromptDialogPayload MakePromptDialogPayload(
    std::string title,
    std::string prompt_text,
    std::optional<std::string> default_value,
    std::function<void(std::optional<std::string>)> on_response)
{
    dsys::PromptDialogPayload p;
    p.id = "prompt_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.title = std::move(title);
    p.prompt_text = std::move(prompt_text);
    p.default_value = std::move(default_value);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a PromptDialog onto the queue.
inline void PushPromptDialog(
    dsys::DialogQueue& queue,
    std::string title,
    std::string prompt_text,
    std::optional<std::string> default_value,
    std::function<void(std::optional<std::string>)> on_response)
{
    queue.push(MakePromptDialogPayload(
        std::move(title), std::move(prompt_text),
        std::move(default_value), std::move(on_response)));
}

// -- Band 4: suppressed while typing (decision banners) --

/// Build a CostThreshold payload.
/// Band 4 — suppressed while typing.
[[nodiscard]] inline dsys::CostThresholdPayload MakeCostThresholdPayload(
    double cost_threshold_usd,
    double current_cost_usd,
    std::string model_name,
    std::function<void(bool continue_, bool reset)> on_response)
{
    dsys::CostThresholdPayload p;
    p.id = "cost_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.cost_threshold_usd = cost_threshold_usd;
    p.current_cost_usd = current_cost_usd;
    p.model_name = std::move(model_name);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a CostThreshold dialog onto the queue.
inline void PushCostThreshold(
    dsys::DialogQueue& queue,
    double cost_threshold_usd,
    double current_cost_usd,
    std::string model_name,
    std::function<void(bool continue_, bool reset)> on_response)
{
    queue.push(MakeCostThresholdPayload(
        cost_threshold_usd, current_cost_usd,
        std::move(model_name), std::move(on_response)));
}

/// Build an Elicitation payload.
/// Band 4 — suppressed while typing.
[[nodiscard]] inline dsys::ElicitationPayload MakeElicitationPayload(
    std::string server_name,
    std::uint64_t request_id,
    std::string request_description,
    std::function<void(bool approve)> on_response)
{
    dsys::ElicitationPayload p;
    p.id = "elicit_" + server_name + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.server_name = std::move(server_name);
    p.request_id = request_id;
    p.request_description = std::move(request_description);
    p.on_response = std::move(on_response);
    return p;
}

/// Push an Elicitation dialog onto the queue.
inline void PushElicitation(
    dsys::DialogQueue& queue,
    std::string server_name,
    std::uint64_t request_id,
    std::string request_description,
    std::function<void(bool approve)> on_response)
{
    queue.push(MakeElicitationPayload(
        std::move(server_name), request_id,
        std::move(request_description), std::move(on_response)));
}

/// Build an IdleReturn payload.
/// Band 4 — suppressed while typing.
[[nodiscard]] inline dsys::IdleReturnPayload MakeIdleReturnPayload(
    std::uint32_t idle_minutes,
    std::function<void(bool resume)> on_response)
{
    dsys::IdleReturnPayload p;
    p.id = "idle_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.idle_minutes = idle_minutes;
    p.on_response = std::move(on_response);
    return p;
}

/// Push an IdleReturn dialog onto the queue.
inline void PushIdleReturn(
    dsys::DialogQueue& queue,
    std::uint32_t idle_minutes,
    std::function<void(bool resume)> on_response)
{
    queue.push(MakeIdleReturnPayload(idle_minutes, std::move(on_response)));
}

/// Build an UltraplanChoice payload.
/// Band 4 — suppressed while typing.
[[nodiscard]] inline dsys::UltraplanChoicePayload MakeUltraplanChoicePayload(
    std::string plan_name,
    std::string price_text,
    std::function<void(bool upgrade)> on_response)
{
    dsys::UltraplanChoicePayload p;
    p.id = "upc_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.plan_name = std::move(plan_name);
    p.price_text = std::move(price_text);
    p.on_response = std::move(on_response);
    return p;
}

/// Push an UltraplanChoice dialog onto the queue.
inline void PushUltraplanChoice(
    dsys::DialogQueue& queue,
    std::string plan_name,
    std::string price_text,
    std::function<void(bool upgrade)> on_response)
{
    queue.push(MakeUltraplanChoicePayload(
        std::move(plan_name), std::move(price_text), std::move(on_response)));
}

// -- Band 5: informational banners --

/// Build a ModelSwitch payload.
/// Band 5 — informational banner.
[[nodiscard]] inline dsys::ModelSwitchPayload MakeModelSwitchPayload(
    std::string from_model,
    std::string to_model,
    std::function<void(bool confirm)> on_response)
{
    dsys::ModelSwitchPayload p;
    p.id = "msw_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.from_model = std::move(from_model);
    p.to_model = std::move(to_model);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a ModelSwitch dialog onto the queue.
inline void PushModelSwitch(
    dsys::DialogQueue& queue,
    std::string from_model,
    std::string to_model,
    std::function<void(bool confirm)> on_response)
{
    queue.push(MakeModelSwitchPayload(
        std::move(from_model), std::move(to_model), std::move(on_response)));
}

/// Build an UndercoverCallout payload.
/// Band 5 — informational banner.
[[nodiscard]] inline dsys::UndercoverCalloutPayload MakeUndercoverCalloutPayload(
    bool is_active,
    std::function<void()> on_dismiss = nullptr)
{
    dsys::UndercoverCalloutPayload p;
    p.id = "uc_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.is_active = is_active;
    p.on_dismiss = std::move(on_dismiss);
    return p;
}

/// Push an UndercoverCallout dialog onto the queue.
inline void PushUndercoverCallout(
    dsys::DialogQueue& queue,
    bool is_active,
    std::function<void()> on_dismiss = nullptr)
{
    queue.push(MakeUndercoverCalloutPayload(is_active, std::move(on_dismiss)));
}

/// Build an EffortCallout payload.
/// Band 5 — informational banner.
[[nodiscard]] inline dsys::EffortCalloutPayload MakeEffortCalloutPayload(
    std::string effort_level,
    std::function<void()> on_dismiss = nullptr)
{
    dsys::EffortCalloutPayload p;
    p.id = "ec_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.effort_level = std::move(effort_level);
    p.on_dismiss = std::move(on_dismiss);
    return p;
}

/// Push an EffortCallout dialog onto the queue.
inline void PushEffortCallout(
    dsys::DialogQueue& queue,
    std::string effort_level,
    std::function<void()> on_dismiss = nullptr)
{
    queue.push(MakeEffortCalloutPayload(
        std::move(effort_level), std::move(on_dismiss)));
}

/// Build a RemoteCallout payload.
/// Band 5 — informational banner.
[[nodiscard]] inline dsys::RemoteCalloutPayload MakeRemoteCalloutPayload(
    std::string host,
    bool is_connected,
    std::function<void()> on_dismiss = nullptr)
{
    dsys::RemoteCalloutPayload p;
    p.id = "rc_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.host = std::move(host);
    p.is_connected = is_connected;
    p.on_dismiss = std::move(on_dismiss);
    return p;
}

/// Push a RemoteCallout dialog onto the queue.
inline void PushRemoteCallout(
    dsys::DialogQueue& queue,
    std::string host,
    bool is_connected,
    std::function<void()> on_dismiss = nullptr)
{
    queue.push(MakeRemoteCalloutPayload(
        std::move(host), is_connected, std::move(on_dismiss)));
}

// -- Band 6: hint / recommendation banners --

/// Build an LspRecommendation payload.
/// Band 6 — hint banner (lowest priority).
[[nodiscard]] inline dsys::LspRecommendationPayload MakeLspRecommendationPayload(
    std::string server_name,
    std::function<void(bool install)> on_response)
{
    dsys::LspRecommendationPayload p;
    p.id = "lsp_" + server_name + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.server_name = std::move(server_name);
    p.on_response = std::move(on_response);
    return p;
}

/// Push an LspRecommendation dialog onto the queue.
inline void PushLspRecommendation(
    dsys::DialogQueue& queue,
    std::string server_name,
    std::function<void(bool install)> on_response)
{
    queue.push(MakeLspRecommendationPayload(
        std::move(server_name), std::move(on_response)));
}

/// Build a PluginHint payload.
/// Band 6 — hint banner (lowest priority).
[[nodiscard]] inline dsys::PluginHintPayload MakePluginHintPayload(
    std::string plugin_name,
    std::string hint_text,
    std::function<void(bool enable)> on_response)
{
    dsys::PluginHintPayload p;
    p.id = "plug_" + plugin_name + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.plugin_name = std::move(plugin_name);
    p.hint_text = std::move(hint_text);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a PluginHint dialog onto the queue.
inline void PushPluginHint(
    dsys::DialogQueue& queue,
    std::string plugin_name,
    std::string hint_text,
    std::function<void(bool enable)> on_response)
{
    queue.push(MakePluginHintPayload(
        std::move(plugin_name), std::move(hint_text), std::move(on_response)));
}

// ============================================================
// Modal-slot dialogs (full overlays)
// ============================================================

/// Build a SettingsPanel payload.
/// Modal slot — full overlay dialog.
[[nodiscard]] inline dsys::SettingsPanelPayload MakeSettingsPanelPayload(
    std::string initial_tab = {},
    std::function<void()> on_close = nullptr)
{
    dsys::SettingsPanelPayload p;
    p.id = "settings_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.initial_tab = std::move(initial_tab);
    p.on_close = std::move(on_close);
    return p;
}

/// Push a SettingsPanel dialog onto the queue.
inline void PushSettingsPanel(
    dsys::DialogQueue& queue,
    std::string initial_tab = {},
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeSettingsPanelPayload(std::move(initial_tab), std::move(on_close)));
}

/// Build a HelpView payload.
/// Modal slot — full overlay dialog.
[[nodiscard]] inline dsys::HelpViewPayload MakeHelpViewPayload(
    std::string initial_section = {},
    std::function<void()> on_close = nullptr)
{
    dsys::HelpViewPayload p;
    p.id = "help_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.initial_section = std::move(initial_section);
    p.on_close = std::move(on_close);
    return p;
}

/// Push a HelpView dialog onto the queue.
inline void PushHelpView(
    dsys::DialogQueue& queue,
    std::string initial_section = {},
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeHelpViewPayload(std::move(initial_section), std::move(on_close)));
}

/// Build a QuickOpen payload (command palette).
/// Modal slot — full overlay dialog.
[[nodiscard]] inline dsys::QuickOpenPayload MakeQuickOpenPayload(
    std::vector<dsys::QuickOpenItem> items,
    std::string initial_query = {},
    std::function<void(int index, bool confirmed)> on_result = nullptr)
{
    dsys::QuickOpenPayload p;
    p.id = "quickopen_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.items = std::move(items);
    p.query = std::move(initial_query);
    p.on_result = std::move(on_result);
    return p;
}

/// Push a QuickOpen dialog onto the queue.
inline void PushQuickOpen(
    dsys::DialogQueue& queue,
    std::vector<dsys::QuickOpenItem> items,
    std::string initial_query = {},
    std::function<void(int index, bool confirmed)> on_result = nullptr)
{
    queue.push(MakeQuickOpenPayload(
        std::move(items), std::move(initial_query), std::move(on_result)));
}

/// Build a MCPDialog payload.
/// Modal slot — full overlay dialog.
[[nodiscard]] inline dsys::MCPDialogPayload MakeMCPDialogPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::MCPDialogPayload p;
    p.id = "mcp_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push an MCPDialog onto the queue.
inline void PushMCPDialog(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeMCPDialogPayload(std::move(on_close)));
}

/// Build a ConfigDialog payload.
/// Modal slot — full overlay dialog.
[[nodiscard]] inline dsys::ConfigDialogPayload MakeConfigDialogPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::ConfigDialogPayload p;
    p.id = "config_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a ConfigDialog onto the queue.
inline void PushConfigDialog(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeConfigDialogPayload(std::move(on_close)));
}

// ============================================================
// More modal dialog triggers (M7.5 — panel views + modals)
// ============================================================

/// Build an AboutDialog payload.
/// Modal slot — about / credits dialog.
[[nodiscard]] inline dsys::AboutDialogPayload MakeAboutDialogPayload(
    std::string version = {},
    std::function<void()> on_close = nullptr)
{
    dsys::AboutDialogPayload p;
    p.id = "about_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.version = std::move(version);
    p.on_close = std::move(on_close);
    return p;
}

/// Push an AboutDialog onto the queue.
inline void PushAboutDialog(
    dsys::DialogQueue& queue,
    std::string version = {},
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeAboutDialogPayload(std::move(version), std::move(on_close)));
}

/// Build a TasksView payload.
/// Modal slot — task list view.
[[nodiscard]] inline dsys::TasksViewPayload MakeTasksViewPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::TasksViewPayload p;
    p.id = "tasks_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a TasksView dialog onto the queue.
inline void PushTasksView(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeTasksViewPayload(std::move(on_close)));
}

/// Build a TeamsView payload.
/// Modal slot — team / multi-agent view.
[[nodiscard]] inline dsys::TeamsViewPayload MakeTeamsViewPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::TeamsViewPayload p;
    p.id = "teams_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a TeamsView dialog onto the queue.
inline void PushTeamsView(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeTeamsViewPayload(std::move(on_close)));
}

/// Build an ExportDialog payload.
/// Modal slot — export conversation.
[[nodiscard]] inline dsys::ExportDialogPayload MakeExportDialogPayload(
    std::string format = {},
    std::function<void(bool /*export_*/)> on_response = nullptr)
{
    dsys::ExportDialogPayload p;
    p.id = "export_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.format = std::move(format);
    p.on_response = std::move(on_response);
    return p;
}

/// Push an ExportDialog onto the queue.
inline void PushExportDialog(
    dsys::DialogQueue& queue,
    std::string format = {},
    std::function<void(bool /*export_*/)> on_response = nullptr)
{
    queue.push(MakeExportDialogPayload(std::move(format), std::move(on_response)));
}

/// Build a DiffDialog payload.
/// Modal slot — diff viewer.
[[nodiscard]] inline dsys::DiffDialogPayload MakeDiffDialogPayload(
    std::string title = {},
    std::string before_text = {},
    std::string after_text = {},
    std::function<void(bool /*accept*/)> on_response = nullptr)
{
    dsys::DiffDialogPayload p;
    p.id = "diff_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.title = std::move(title);
    p.before_text = std::move(before_text);
    p.after_text = std::move(after_text);
    p.on_response = std::move(on_response);
    return p;
}

/// Push a DiffDialog onto the queue.
inline void PushDiffDialog(
    dsys::DialogQueue& queue,
    std::string title = {},
    std::string before_text = {},
    std::string after_text = {},
    std::function<void(bool /*accept*/)> on_response = nullptr)
{
    queue.push(MakeDiffDialogPayload(
        std::move(title), std::move(before_text),
        std::move(after_text), std::move(on_response)));
}

/// Build a PluginDialog payload.
/// Modal slot — plugin manager.
[[nodiscard]] inline dsys::PluginDialogPayload MakePluginDialogPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::PluginDialogPayload p;
    p.id = "plugin_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a PluginDialog onto the queue.
inline void PushPluginDialog(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakePluginDialogPayload(std::move(on_close)));
}

/// Build a GlobalSearch payload.
/// Modal slot — full-text search.
[[nodiscard]] inline dsys::GlobalSearchPayload MakeGlobalSearchPayload(
    std::string initial_query = {},
    std::function<void()> on_close = nullptr)
{
    dsys::GlobalSearchPayload p;
    p.id = "gsearch_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.query = std::move(initial_query);
    p.on_close = std::move(on_close);
    return p;
}

/// Push a GlobalSearch dialog onto the queue.
inline void PushGlobalSearch(
    dsys::DialogQueue& queue,
    std::string initial_query = {},
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeGlobalSearchPayload(std::move(initial_query), std::move(on_close)));
}

/// Build a HistorySearch payload.
/// Modal slot — conversation history search.
[[nodiscard]] inline dsys::HistorySearchPayload MakeHistorySearchPayload(
    std::string initial_query = {},
    std::function<void(std::string_view /*conversation_id*/)> on_select = nullptr)
{
    dsys::HistorySearchPayload p;
    p.id = "hsearch_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.query = std::move(initial_query);
    p.on_select = std::move(on_select);
    return p;
}

/// Push a HistorySearch dialog onto the queue.
inline void PushHistorySearch(
    dsys::DialogQueue& queue,
    std::string initial_query = {},
    std::function<void(std::string_view /*conversation_id*/)> on_select = nullptr)
{
    queue.push(MakeHistorySearchPayload(std::move(initial_query), std::move(on_select)));
}

/// Build a FeedbackSurvey payload.
/// Modal slot — feedback survey.
[[nodiscard]] inline dsys::FeedbackSurveyPayload MakeFeedbackSurveyPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::FeedbackSurveyPayload p;
    p.id = "feedback_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a FeedbackSurvey dialog onto the queue.
inline void PushFeedbackSurvey(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeFeedbackSurveyPayload(std::move(on_close)));
}

/// Build a ManagedSettingsSecurity payload.
/// Modal slot — managed org security settings.
[[nodiscard]] inline dsys::ManagedSettingsSecurityPayload
MakeManagedSettingsSecurityPayload(
    std::function<void()> on_close = nullptr)
{
    dsys::ManagedSettingsSecurityPayload p;
    p.id = "msec_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.on_close = std::move(on_close);
    return p;
}

/// Push a ManagedSettingsSecurity dialog onto the queue.
inline void PushManagedSettingsSecurity(
    dsys::DialogQueue& queue,
    std::function<void()> on_close = nullptr)
{
    queue.push(MakeManagedSettingsSecurityPayload(std::move(on_close)));
}

/// Build a TrustDialog payload.
/// Simple API: takes a hostname and a boolean callback (trust = true/false).
/// Creates a Medium-tier trust dialog with the hostname as target.
///
/// For richer tiered trust (Low/Medium/High/Critical with full gating),
/// use MakeRichTrustDialogPayload() with TrustDialogProps directly.
[[nodiscard]] inline dsys::TrustDialogPayload MakeTrustDialogPayload(
    std::string hostname = {},
    std::function<void(bool /*trust*/)> on_response = nullptr)
{
    namespace td = cc::ui::trust_dialog;
    namespace tu = cc::ui::trust_utils;

    auto state = std::make_shared<td::DialogState>();
    state->props.action = tu::ActionType::PathAccess;
    state->props.action_label = "Host Access";
    state->props.summary.action_summary =
        hostname.empty()
            ? std::string{"Claude Code wants to access an external host."}
            : std::format("Claude Code wants to access {}.", hostname);
    state->props.summary.domains = hostname.empty()
        ? std::vector<std::string>{}
        : std::vector<std::string>{hostname};
    state->props.forced_level = tu::RiskLevel::Medium;

    // Wrap the bool callback into the full TrustChoice callback.
    state->props.on_done = [cb = std::move(on_response)](td::TrustChoice choice) {
        if (!cb) return;
        const bool allow =
            choice == td::TrustChoice::AllowOnce ||
            choice == td::TrustChoice::AlwaysAllow ||
            choice == td::TrustChoice::EnableAnyway;
        cb(allow);
    };

    state->selected = 0;
    state->show_details = true;

    dsys::TrustDialogPayload p;
    p.id = "trust_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.state = std::move(state);
    return p;
}

/// Build a TrustDialog payload from full TrustDialogProps.
/// Supports all risk tiers, plugin info, sensitive paths, etc.
[[nodiscard]] inline dsys::TrustDialogPayload MakeRichTrustDialogPayload(
    cc::ui::trust_dialog::TrustDialogProps props)
{
    namespace td = cc::ui::trust_dialog;
    namespace tu = cc::ui::trust_utils;

    // Pre-compute risk tier if caller left it at Low (default).
    if (props.forced_level == tu::RiskLevel::Low) {
        props.forced_level = tu::classify_risk(props.action, props.summary);
    }

    auto state = std::make_shared<td::DialogState>();
    state->props = std::move(props);
    state->selected = 0;
    state->show_details = true;

    // High tier: initialise countdown.
    if (state->props.forced_level == tu::RiskLevel::High) {
        state->countdown_active = true;
        state->countdown_remaining = tu::kHighTierCountdownSeconds;
    }

    dsys::TrustDialogPayload p;
    p.id = "trust_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    p.state = std::move(state);
    return p;
}

/// Push a TrustDialog onto the queue (simple API).
inline void PushTrustDialog(
    dsys::DialogQueue& queue,
    std::string hostname = {},
    std::function<void(bool /*trust*/)> on_response = nullptr)
{
    queue.push(MakeTrustDialogPayload(std::move(hostname), std::move(on_response)));
}

// ============================================================
// Utility: Push from command metadata
// ============================================================

/// Try to handle a command metadata string by pushing the corresponding
/// dialog onto the queue.  Returns true if the metadata was recognized
/// and a dialog was pushed.
///
/// This is the bridge between command results and the dialog queue —
/// commands produce metadata strings (e.g. "CREATE_AGENT",
/// "UI:plugins:manage-plugins") and the UI layer converts them to
/// queue pushes.
inline bool PushFromCommandMetadata(
    dsys::DialogQueue& queue,
    std::string_view metadata,
    const std::function<void(std::string_view /*command*/)>& enqueue_command = nullptr)
{
    using namespace std::string_view_literals;
    (void)enqueue_command;  // reserved for future dialog types that run commands

    // -- Agent wizards --
    if (metadata == "CREATE_AGENT"sv) {
        dsys::CreateAgentWizardPayload p;
        p.id = "create_agent_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_complete = [&queue](bool /*complete*/, std::string_view /*name*/) {
            queue.pop_modal();
        };
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("EDIT_AGENT|"sv)) {
        dsys::EditAgentWizardPayload p;
        p.id = "edit_agent_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        auto name_part = metadata.substr(11); // len("EDIT_AGENT|") = 11
        p.agent_name = std::string(name_part);
        p.on_complete = [&queue](bool /*complete*/) { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }

    // -- Plugin dialogs --
    if (metadata == "UI:plugins:discover-plugins"sv ||
        metadata == "UI:plugins:manage-plugins"sv ||
        metadata.starts_with("UI:plugins:browse-marketplace:"sv) ||
        metadata == "UI:plugins:add-marketplace"sv ||
        metadata.starts_with("UI:plugins:manage-marketplaces"sv))
    {
        dsys::PluginDialogPayload p;
        p.id = "plugin_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_close = [&queue] { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }

    // -- Settings & Help & Config & MCP (modal panels) --
    if (metadata == "UI:settings"sv || metadata == "SETTINGS_PANEL"sv) {
        dsys::SettingsPanelPayload p;
        p.id = "settings_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_close = [&queue] { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata == "UI:help"sv || metadata == "HELP_VIEW"sv) {
        dsys::HelpViewPayload p;
        p.id = "help_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_close = [&queue] { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata == "UI:config"sv || metadata == "CONFIG_DIALOG"sv) {
        dsys::ConfigDialogPayload p;
        p.id = "config_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_close = [&queue] { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata == "UI:mcp"sv || metadata == "MCP_DIALOG"sv) {
        dsys::MCPDialogPayload p;
        p.id = "mcp_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.on_close = [&queue] { queue.pop_modal(); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata == "UI:quick-open"sv || metadata == "QUICK_OPEN"sv) {
        // Build default quick open items
        std::vector<dsys::QuickOpenItem> items;
        items.push_back({"Settings", "Open settings panel", "", "Commands"});
        items.push_back({"Help", "View help and keyboard shortcuts", "", "Commands"});
        items.push_back({"MCP Servers", "Manage MCP servers", "", "Commands"});
        items.push_back({"Plugins", "Browse and manage plugins", "", "Commands"});
        items.push_back({"Export", "Export conversation", "", "Commands"});

        dsys::QuickOpenPayload p;
        p.id = "qf_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.items = std::move(items);
        p.on_result = [&queue](int /*index*/, bool /*confirmed*/) {
            queue.pop_modal();
        };
        queue.push(std::move(p));
        return true;
    }

    // -- Bottom-slot banners (informational) --
    if (metadata.starts_with("UI:undercover|"sv)) {
        auto active_str = metadata.substr(14);  // len("UI:undercover|")
        bool active = active_str == "1" || active_str == "true";
        dsys::UndercoverCalloutPayload p;
        p.id = "undercover_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.is_active = active;
        p.on_dismiss = [&queue, id = p.id] { queue.remove(id); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:effort|"sv)) {
        auto level = std::string(metadata.substr(10));  // len("UI:effort|")
        dsys::EffortCalloutPayload p;
        p.id = "effort_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.effort_level = std::move(level);
        p.on_dismiss = [&queue, id = p.id] { queue.remove(id); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:remote|"sv)) {
        auto host_name = std::string(metadata.substr(10));  // len("UI:remote|")
        dsys::RemoteCalloutPayload p;
        p.id = "remote_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.host = std::move(host_name);
        p.is_connected = true;
        p.on_dismiss = [&queue, id = p.id] { queue.remove(id); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:lsp-rec|"sv)) {
        auto server = std::string(metadata.substr(11));  // len("UI:lsp-rec|")
        dsys::LspRecommendationPayload p;
        p.id = "lsprec_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.server_name = std::move(server);
        p.on_response = [&queue, id = p.id](bool /*install*/) { queue.remove(id); };
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:plugin-hint|"sv)) {
        auto plugin = std::string(metadata.substr(15));  // len("UI:plugin-hint|")
        dsys::PluginHintPayload p;
        p.id = "plughint_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        p.plugin_name = std::move(plugin);
        p.hint_text = "Recommended plugin";
        p.on_response = [&queue, id = p.id](bool /*enable*/) { queue.remove(id); };
        queue.push(std::move(p));
        return true;
    }

    // -- More modal dialogs (M7.5 migration) --
    if (metadata == "UI:about"sv || metadata == "ABOUT_DIALOG"sv) {
        PushAboutDialog(queue, "", [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:tasks"sv || metadata == "TASKS_VIEW"sv) {
        PushTasksView(queue, [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:teams"sv || metadata == "TEAMS_VIEW"sv) {
        PushTeamsView(queue, [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:export"sv || metadata == "EXPORT_DIALOG"sv) {
        PushExportDialog(queue, "", [&queue](bool /*ok*/) { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:diff"sv || metadata == "DIFF_DIALOG"sv) {
        PushDiffDialog(queue, "Changes", "", "",
            [&queue](bool /*accept*/) { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:feedback"sv || metadata == "FEEDBACK_SURVEY"sv) {
        PushFeedbackSurvey(queue, [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:managed-security"sv ||
        metadata == "MANAGED_SETTINGS_SECURITY"sv)
    {
        PushManagedSettingsSecurity(queue, [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata.starts_with("UI:search|"sv)) {
        auto q = std::string(metadata.substr(10));  // len("UI:search|")
        PushGlobalSearch(queue, std::move(q), [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:global-search"sv || metadata == "GLOBAL_SEARCH"sv) {
        PushGlobalSearch(queue, "", [&queue] { queue.pop_modal(); });
        return true;
    }
    if (metadata == "UI:history-search"sv || metadata == "HISTORY_SEARCH"sv) {
        PushHistorySearch(queue, "",
            [&queue](std::string_view /*id*/) { queue.pop_modal(); });
        return true;
    }

    // Unrecognized metadata — not a dialog trigger
    return false;
}

} // namespace cc::ui::dialogs::triggers
