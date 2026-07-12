/// @file triggers.cppm
/// @brief Trigger helpers that push dialog payloads onto a DialogQueue.
///
/// These helpers construct the appropriate payload (matching the
/// P0x3 contracts for each dialog type) and push it.  Slot routing
/// and priority banding are resolved AUTOMATICALLY by the payload's
/// membership in DialogPayloadVariant → type_of() → slot_of() →
/// priority_of().  Callers therefore NEVER pass an explicit
/// DialogType / DialogPriority / slot tag to the push* methods;
/// pass just the payload.
module;
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module cc.ui.dialogs.triggers;

import cc.constants.product;
import cc.ui.dialogs.system;

export namespace cc::ui::dialogs::triggers {

namespace dsys = cc::ui::dialogs::system;

// ---------------------------------------------------------------------------
// ToolPermission (overlay slot, queue.push auto-routes it)
// ---------------------------------------------------------------------------
// 6-arg canonical form: queue, tool_name, description, on_response, on_abort,
//                       can_always_allow
inline void PushToolPermission(
    dsys::DialogQueue& queue,
    std::string tool_name,
    std::string description,
    std::function<void(typename dsys::ToolPermissionPayload::Decision, bool)> on_response,
    std::function<void()> on_abort,
    bool can_always_allow) {
    dsys::ToolPermissionPayload p;
    p.id = "tool-permission";
    p.tool_name = std::move(tool_name);
    p.description = std::move(description);
    p.can_always_allow = can_always_allow;
    p.initial_sandbox_toggle = false;
    p.on_response = std::move(on_response);
    p.on_abort = std::move(on_abort);
    queue.push(std::move(p));
}

// 4-arg convenience form (used by tests) — supplies safe defaults for the
// two trailing parameters.
inline void PushToolPermission(
    dsys::DialogQueue& queue,
    std::string tool_name,
    std::string description,
    std::function<void(typename dsys::ToolPermissionPayload::Decision, bool)> on_response) {
    PushToolPermission(queue, std::move(tool_name), std::move(description),
                       std::move(on_response), []() {}, /*can_always_allow=*/true);
}

// ---------------------------------------------------------------------------
// CostThreshold
// ---------------------------------------------------------------------------
// Canonical P0x3 contract — 0-arg on_done, dollars_spent, optional model_name.
inline void PushCostThreshold(dsys::DialogQueue& queue,
                              double dollars_spent,
                              std::optional<std::string> model_name,
                              std::function<void()> on_done) {
    dsys::CostThresholdPayload p;
    p.id = "cost-threshold";
    p.dollars_spent = dollars_spent;
    p.model_name    = std::move(model_name);
    p.on_done       = std::move(on_done);
    queue.push(std::move(p));
}

// Legacy 5-arg adapter (threshold_usd, dollars_spent, model_name string,
// on_response<bool continue_, bool reset>) — kept so old call sites build
// while being migrated.  Internally wraps on_response(bool,bool) so firing
// on_done() propagates (continue=true, reset=false).
inline void PushCostThreshold(dsys::DialogQueue& queue,
                              double /*threshold_usd*/,
                              double dollars_spent,
                              std::string model_name,
                              std::function<void(bool, bool)> on_response) {
    PushCostThreshold(queue, dollars_spent,
                      std::optional<std::string>{std::move(model_name)},
                      [on_response = std::move(on_response)] {
                          if (on_response) on_response(true, false);
                      });
}

// ---------------------------------------------------------------------------
// SandboxPermission
// ---------------------------------------------------------------------------
inline void PushSandboxPermission(dsys::DialogQueue& queue,
                                  std::string origin,
                                  std::function<void(bool allow, bool always)> on_response) {
    dsys::SandboxPermissionPayload p;
    p.id = "sandbox-permission";
    // SandboxPermissionPayload stores the host under field `host_pattern`
    // (the canonical name for network-origin rules in this codebase).
    p.host_pattern = std::move(origin);
    p.on_response = std::move(on_response);
    queue.push(std::move(p));
}

// ---------------------------------------------------------------------------
// SettingsPanel / HelpView / LspRecommendation / ModelSwitch
// ---------------------------------------------------------------------------
inline void PushSettingsPanel(dsys::DialogQueue& queue,
                              std::string initial_tab,
                              std::function<void()> on_close) {
    dsys::SettingsPanelPayload p;
    p.id = "settings-panel";
    p.initial_tab = std::move(initial_tab);
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

inline void PushHelpView(dsys::DialogQueue& queue,
                         std::string section,
                         std::function<void()> on_close) {
    dsys::HelpViewPayload p;
    p.id = "help-view";
    p.initial_section = std::move(section);
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

inline void PushLspRecommendation(dsys::DialogQueue& queue,
                                  std::string server_name,
                                  std::function<void(bool install)> on_response) {
    dsys::LspRecommendationPayload p;
    p.id = "lsp-rec";
    p.server_name = std::move(server_name);
    p.on_response = std::move(on_response);
    queue.push(std::move(p));
}

inline void PushModelSwitch(dsys::DialogQueue& queue,
                            std::string current_model,
                            std::string target_model,
                            std::function<void(bool confirm)> on_response) {
    dsys::ModelSwitchPayload p;
    p.id = "model-switch";
    p.from_model = std::move(current_model);
    p.to_model   = std::move(target_model);
    p.on_response = std::move(on_response);
    queue.push(std::move(p));
}

inline void PushMessageSelector(dsys::DialogQueue& queue,
                                std::vector<std::string> options,
                                std::string prompt,
                                std::function<void(int idx)> on_select) {
    dsys::MessageSelectorPayload p;
    p.id = "message-selector";
    p.options = std::move(options);
    p.selected_index = 0;
    p.placeholder = std::move(prompt);
    p.on_select = std::move(on_select);
    queue.push(std::move(p));
}

// ---------------------------------------------------------------------------
// Elicitation
// ---------------------------------------------------------------------------
inline void PushElicitation(dsys::DialogQueue& queue,
                            std::string server_name,
                            std::uint64_t request_id,
                            std::string message,
                            std::function<void(bool approve)> on_response) {
    dsys::ElicitationPayload p;
    p.id = "elicitation";
    p.server_name = std::move(server_name);
    p.request_id = request_id;
    p.request_description = std::move(message);
    p.on_response = std::move(on_response);
    p.on_cancel = [on_response] { if (on_response) on_response(false); };
    queue.push(std::move(p));
}

// ---------------------------------------------------------------------------
// QuickOpen
// ---------------------------------------------------------------------------
inline void PushQuickOpen(dsys::DialogQueue& queue,
                          std::vector<dsys::QuickOpenItem> items,
                          std::string query,
                          std::function<void(int index, bool confirmed)> on_result) {
    dsys::QuickOpenPayload p;
    p.id = "quick-open";
    p.query = std::move(query);
    p.items = std::move(items);
    p.selected_index = 0;
    p.on_result = std::move(on_result);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// AboutDialog
// ---------------------------------------------------------------------------
inline void PushAboutDialog(dsys::DialogQueue& queue,
                            std::string version,
                            std::function<void()> on_close) {
    dsys::AboutDialogPayload p;
    p.id = "about-dialog";
    p.version = std::move(version);
    p.build_date = std::string(cc::constants::product::BUILD_DATE);
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// TasksView / TeamsView
// ---------------------------------------------------------------------------
inline void PushTasksView(dsys::DialogQueue& queue,
                          std::function<void()> on_close) {
    dsys::TasksViewPayload p;
    p.id = "tasks-view";
    p.selected_index = 0;
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

inline void PushTeamsView(dsys::DialogQueue& queue,
                          std::function<void()> on_close) {
    dsys::TeamsViewPayload p;
    p.id = "teams-view";
    p.selected_index = 0;
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// ExportDialog
// ---------------------------------------------------------------------------
inline void PushExportDialog(dsys::DialogQueue& queue,
                             std::string format,
                             std::function<void(bool do_export)> on_response) {
    dsys::ExportDialogPayload p;
    p.id = "export-dialog";
    p.format = std::move(format);
    p.on_response = std::move(on_response);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// DiffDialog
// ---------------------------------------------------------------------------
inline void PushDiffDialog(dsys::DialogQueue& queue,
                           std::string title,
                           std::string before_text,
                           std::string after_text,
                           std::function<void(bool accept)> on_response) {
    dsys::DiffDialogPayload p;
    p.id = "diff-dialog";
    p.title = std::move(title);
    p.before_text = std::move(before_text);
    p.after_text  = std::move(after_text);
    p.on_response = std::move(on_response);
    p.on_close    = []() {};
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// GlobalSearch / HistorySearch
// ---------------------------------------------------------------------------
inline void PushGlobalSearch(dsys::DialogQueue& queue,
                             std::string query,
                             std::function<void()> on_close) {
    dsys::GlobalSearchPayload p;
    p.id = "global-search";
    p.query = std::move(query);
    p.selected_index = 0;
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

inline void PushHistorySearch(dsys::DialogQueue& queue,
                              std::string query,
                              std::function<void(std::string_view conversation_id)> on_select) {
    dsys::HistorySearchPayload p;
    p.id = "history-search";
    p.query = std::move(query);
    p.selected_index = 0;
    p.on_select = std::move(on_select);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// FeedbackSurvey
// ---------------------------------------------------------------------------
inline void PushFeedbackSurvey(dsys::DialogQueue& queue,
                               std::function<void()> on_close) {
    dsys::FeedbackSurveyPayload p;
    p.id = "feedback-survey";
    p.state = dsys::FeedbackSurveyState::Open;
    p.on_close = std::move(on_close);
    p.on_submit = [](int) {};
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// ManagedSettingsSecurity
// ---------------------------------------------------------------------------
inline void PushManagedSettingsSecurity(dsys::DialogQueue& queue,
                                        std::function<void()> on_close) {
    dsys::ManagedSettingsSecurityPayload p;
    p.id = "managed-settings-security";
    p.selected_index = 0;
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

// ---------------------------------------------------------------------------
// PluginDialog
// ---------------------------------------------------------------------------
// Full form — accepts initial menu_selected card index and an id_suffix that
// encodes the originating metadata view (e.g. "discover-plugins",
// "manage-plugins?action=uninstall").  The renderer parses id_suffix to
// pre-select a row or auto-open an action panel.
inline void PushPluginDialog(dsys::DialogQueue& queue,
                             int menu_selected,
                             std::string id_suffix,
                             std::function<void()> on_close) {
    dsys::PluginDialogPayload p;
    p.id = "plugins-dialog" + (id_suffix.empty() ? "" : ":" + std::move(id_suffix));
    p.menu_selected = menu_selected;
    p.on_close = std::move(on_close);
    queue.push_modal(std::move(p));
}

// Legacy 2-arg form — defaults to Installed card (index 0).
inline void PushPluginDialog(dsys::DialogQueue& queue,
                             std::function<void()> on_close) {
    PushPluginDialog(queue, 0, "", std::move(on_close));
}

// ---------------------------------------------------------------------------
// TrustDialog — the payload is type-erased via shared_ptr<void> state.
// Callers pass a scope string; the renderer is responsible for interpreting
// `state` via a trust_dialog::DialogState cast.  For push-only callers that
// only need routing (queue.contains_type()), constructing with the scope
// string encoded in id is enough.  The opaque state shared_ptr is default-
// constructed; renderers that actually need the state object check for it
// and build lazily.
// ---------------------------------------------------------------------------
inline void PushTrustDialog(dsys::DialogQueue& queue,
                            std::string scope,
                            std::function<void(bool)> /*on_response_adapter_ignored_by_payload*/) {
    dsys::TrustDialogPayload p;
    p.id = "trust-dialog:" + std::move(scope);
    // p.state is intentionally left null here; the renderer builds the
    // trust_dialog::DialogState from id on first render.
    queue.push_standalone(std::move(p));
}

// ---------------------------------------------------------------------------
// CommandMetadata dispatcher (UI:* tag → push corresponding dialog)
// ---------------------------------------------------------------------------
// 2-arg canonical form (used everywhere).
inline bool PushFromCommandMetadata(dsys::DialogQueue& queue,
                                    std::string_view metadata) {
    // Agents / wizards — standalone-slot fullscreen payloads.
    if (metadata == "CREATE_AGENT") {
        dsys::CreateAgentWizardPayload p;
        p.id = "create-agent-wizard";
        p.on_complete = [](bool, std::string) {};
        queue.push_standalone(std::move(p));
        return true;
    }
    if (metadata == "EDIT_AGENT" || metadata.starts_with("EDIT_AGENT|")) {
        dsys::EditAgentWizardPayload p;
        p.id = "edit-agent-wizard";
        constexpr auto kPrefixLen = sizeof("EDIT_AGENT|") - 1;
        if (metadata.size() > kPrefixLen) {
            p.agent_name = std::string{metadata.substr(kPrefixLen)};
        }
        p.on_complete = [](bool) {};
        queue.push_standalone(std::move(p));
        return true;
    }
    if (metadata == "UI:settings") {
        PushSettingsPanel(queue, "general", [](){});
        return true;
    }
    if (metadata == "UI:permissions") {
        // Opens the SettingsView with the Permissions tab pre-selected.
        // NOTE: the primary handler for this tag lives in app.cppm which
        // sets screen_state_->settings_initial_tab and switches mode to
        // SettingsView directly.  This queue-push form is provided for
        // callers that route exclusively through PushFromCommandMetadata.
        PushSettingsPanel(queue, "Permissions", [](){});
        return true;
    }
    if (metadata == "UI:help") {
        PushHelpView(queue, "commands", [](){});
        return true;
    }
    if (metadata == "UI:config") {
        dsys::ConfigDialogPayload p;
        p.id = "config-dialog";
        p.on_close = [](){};
        queue.push_modal(std::move(p));
        return true;
    }
    if (metadata == "UI:mcp") {
        dsys::MCPDialogPayload p;
        p.id = "mcp-dialog";
        p.on_close = [](){};
        queue.push_modal(std::move(p));
        return true;
    }
    if (metadata == "UI:quick-open") {
        PushQuickOpen(queue, {}, "", [](int, bool){});
        return true;
    }
    if (metadata == "UI:about") {
        PushAboutDialog(queue, "dev", [](){});
        return true;
    }
    if (metadata == "UI:tasks") {
        PushTasksView(queue, [](){});
        return true;
    }
    if (metadata == "UI:teams") {
        PushTeamsView(queue, [](){});
        return true;
    }
    if (metadata == "UI:export") {
        PushExportDialog(queue, "markdown", [](bool){});
        return true;
    }
    if (metadata == "DIFF_DIALOG") {
        PushDiffDialog(queue, "", "", "", [](bool){});
        return true;
    }
    if (metadata == "UI:feedback") {
        PushFeedbackSurvey(queue, [](){});
        return true;
    }
    if (metadata == "GLOBAL_SEARCH") {
        PushGlobalSearch(queue, "", [](){});
        return true;
    }
    if (metadata == "HISTORY_SEARCH") {
        PushHistorySearch(queue, "", [](std::string_view){});
        return true;
    }
    if (metadata == "MANAGED_SETTINGS_SECURITY") {
        PushManagedSettingsSecurity(queue, [](){});
        return true;
    }
    if (metadata == "UI:model-picker") {
        // Model picker: push a ModelSwitchPayload with an empty target so
        // the renderer shows the full model list instead of a single
        // confirmation banner.  The on_response callback is a no-op because
        // the actual model switch is performed by the command handler when
        // the user selects a model (which dispatches SwitchModel via the
        // AppState action system — see cc.commands.model).
        PushModelSwitch(queue, "", "", [](bool){});
        return true;
    }
    // ── Plugin dialog — all "UI:plugins:*" variants map to the same   ──
    //    PluginDialog modal, just with different initial menu cards.    ──
    //    menu_selected indexes k_menu_cards in plugin_dialog.cppm:
    //      0 = Installed (ManagePlugins), 1 = Marketplace (BrowseMarketplace),
    //      2 = Discover (DiscoverPlugins), 3 = Settings (ManageMarketplaces),
    //      4 = Validate
    if (metadata == "UI:plugins:discover-plugins") {
        // Menu card 2 = Discover (trending & recommended plugins)
        PushPluginDialog(queue, 2, "discover-plugins", [](){});
        return true;
    }
    if (metadata == "UI:plugins:manage-plugins") {
        // Menu card 0 = Installed (manage installed: enable/disable/uninstall)
        PushPluginDialog(queue, 0, "manage-plugins", [](){});
        return true;
    }
    if (metadata == "UI:plugins:manage-marketplaces") {
        // Menu card 3 = Settings (marketplace CRUD: add/remove/update)
        PushPluginDialog(queue, 3, "manage-marketplaces", [](){});
        return true;
    }
    if (metadata == "UI:plugins:add-marketplace") {
        // Menu card 3 = Settings, pre-open the add-input form
        PushPluginDialog(queue, 3, "add-marketplace", [](){});
        return true;
    }
    if (metadata.starts_with("UI:plugins:browse-marketplace:")) {
        // Menu card 1 = Marketplace, scoped to a single marketplace
        constexpr auto kPrefixLen = sizeof("UI:plugins:") - 1;  // skip "UI:plugins:"
        PushPluginDialog(queue, 1,
            std::string{metadata.substr(kPrefixLen)}, [](){});
        return true;
    }
    if (metadata.starts_with("UI:plugins:manage-plugins?")) {
        // Menu card 0 = Installed, with action query (e.g. ?action=uninstall)
        constexpr auto kPrefixLen = sizeof("UI:plugins:") - 1;
        PushPluginDialog(queue, 0,
            std::string{metadata.substr(kPrefixLen)}, [](){});
        return true;
    }
    if (metadata.starts_with("UI:plugins:manage-marketplaces?")) {
        // Menu card 3 = Settings, with action query (e.g. ?action=remove)
        constexpr auto kPrefixLen = sizeof("UI:plugins:") - 1;
        PushPluginDialog(queue, 3,
            std::string{metadata.substr(kPrefixLen)}, [](){});
        return true;
    }
    // ── Doctor — standalone fullscreen diagnostics ──
    if (metadata == "UI:doctor") {
        dsys::DoctorDialogPayload p;
        p.id = "doctor-dialog";
        p.on_done = [&queue](std::string /*result*/) {
            queue.pop_standalone();
        };
        queue.push_standalone(std::move(p));
        return true;
    }
    // ── Hooks — modal hooks configuration menu ──
    if (metadata == "UI:hooks") {
        dsys::HooksDialogPayload p;
        p.id = "hooks-config";
        p.on_close = [&queue]() {
            queue.pop_modal();
        };
        queue.push_modal(std::move(p));
        return true;
    }
    // Callouts — bottom-slot chrome, each with their own payload struct.
    if (metadata.starts_with("UI:undercover|")) {
        dsys::UndercoverCalloutPayload p;
        p.id = "undercover-callout";
        p.is_active = true;
        p.on_dismiss = [](){};
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:effort|")) {
        dsys::EffortCalloutPayload p;
        p.id = "effort-callout";
        constexpr auto kPrefixLen = sizeof("UI:effort|") - 1;
        if (metadata.size() > kPrefixLen) {
            p.effort_level = std::string{metadata.substr(kPrefixLen)};
        } else {
            p.effort_level = "medium";
        }
        p.on_dismiss = [](){};
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:remote|")) {
        dsys::RemoteCalloutPayload p;
        p.id = "remote-callout";
        constexpr auto kPrefixLen = sizeof("UI:remote|") - 1;
        if (metadata.size() > kPrefixLen) {
            p.host = std::string{metadata.substr(kPrefixLen)};
        }
        p.is_connected = true;
        p.on_dismiss = [](){};
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:lsp-rec|")) {
        dsys::LspRecommendationPayload p;
        p.id = "lsp-rec-callout";
        constexpr auto kPrefixLen = sizeof("UI:lsp-rec|") - 1;
        if (metadata.size() > kPrefixLen) {
            p.server_name = std::string{metadata.substr(kPrefixLen)};
        } else {
            p.server_name = "LSP server";
        }
        p.on_response = [](bool) {};
        queue.push(std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:plugin-hint|")) {
        dsys::PluginHintPayload p;
        p.id = "plugin-hint";
        constexpr auto kPrefixLen = sizeof("UI:plugin-hint|") - 1;
        if (metadata.size() > kPrefixLen) {
            std::string rest{metadata.substr(kPrefixLen)};
            auto pos = rest.find('|');
            if (pos != std::string::npos) {
                p.plugin_name = rest.substr(0, pos);
                p.hint_text   = rest.substr(pos + 1);
            } else {
                p.plugin_name = std::move(rest);
                p.hint_text   = "Install this plugin for better support.";
            }
        } else {
            p.plugin_name = "plugin";
            p.hint_text   = "Install this plugin for better support.";
        }
        p.on_response = [](bool) {};
        queue.push(std::move(p));
        return true;
    }
    return false;
}

// 3-arg form used ONLY by src/ui/app.cppm:875 — accepts an enqueue_fn that
// normally fires after push routing completes.  Since routing in the 1-arg
// push() is synchronous (no async phase), we just route via PushFromCommandMetadata
// and then invoke enqueue_fn if the route matched.
template <typename EnqueueFn>
inline bool PushFromCommandMetadata(dsys::DialogQueue& queue,
                                    std::string_view metadata,
                                    EnqueueFn&& enqueue_fn) {
    const bool matched = PushFromCommandMetadata(queue, metadata);
    if (matched) {
        if constexpr (std::is_invocable_v<EnqueueFn>) {
            std::invoke(std::forward<EnqueueFn>(enqueue_fn));
        } else if constexpr (std::is_invocable_v<EnqueueFn, std::string>) {
            // enqueue_fn may optionally accept the dialog id; we don't have
            // a direct id to pass here so callers with this signature get
            // the empty placeholder (they rarely inspect it).
            std::invoke(std::forward<EnqueueFn>(enqueue_fn), std::string{});
        }
    }
    return matched;
}

} // namespace cc::ui::dialogs::triggers
