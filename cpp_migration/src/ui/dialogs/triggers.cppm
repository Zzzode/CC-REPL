/// @file triggers.cppm
/// @brief Trigger helpers that push dialog payloads onto a DialogQueue.
///
/// These helpers construct the appropriate payload (matching the
/// P0x3 contracts for each dialog type) and push it with correct
/// slot / priority band assignments.
module;
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

export module cc.ui.dialogs.triggers;

import cc.ui.dialogs.system;

export namespace cc::ui::dialogs::triggers {

namespace dsys = cc::ui::dialogs::system;

// ---------------------------------------------------------------------------
// CostThreshold
// ---------------------------------------------------------------------------
// P0x3 CONTRACT — 0-arg on_done, dollars_spent, optional model_name.
// The legacy (threshold, current, bool,bool) signature is intentionally
// NOT provided — callers MUST migrate to the new contract.
inline void PushCostThreshold(dsys::DialogQueue& queue,
                              double dollars_spent,
                              std::optional<std::string> model_name,
                              std::function<void()> on_done) {
    dsys::CostThresholdPayload p;
    p.id = "cost-threshold";
    p.dollars_spent = dollars_spent;
    p.model_name    = std::move(model_name);
    p.on_done       = std::move(on_done);
    queue.push_bottom(dsys::DialogType::CostThreshold,
                      dsys::DialogPriority::Band4,
                      std::move(p));
}

// Legacy adapter — kept ONLY so old call sites still build while being
// migrated to the 0-arg on_done contract.  Internally calls the new
// contract above with on_done wrapped to trigger on_response with
// (true, false) = "continue, don't reset".
inline void PushCostThreshold(dsys::DialogQueue& queue,
                              double /*threshold_usd*/,
                              double dollars_spent,
                              std::optional<std::string> model_name,
                              std::function<void(bool /*continue_*/, bool /*reset*/)> on_response) {
    PushCostThreshold(queue, dollars_spent, std::move(model_name),
        [on_response = std::move(on_response)] {
            if (on_response) on_response(/*continue_=*/true, /*reset=*/false);
        });
}

// String overload of model_name for convenience.
inline void PushCostThreshold(dsys::DialogQueue& queue,
                              double /*threshold_usd*/,
                              double dollars_spent,
                              std::string model_name,
                              std::function<void(bool, bool)> on_response) {
    PushCostThreshold(queue, dollars_spent,
                      std::optional<std::string>{std::move(model_name)},
                      std::move(on_response));
}

// ---------------------------------------------------------------------------
// SandboxPermission
// ---------------------------------------------------------------------------
inline void PushSandboxPermission(dsys::DialogQueue& queue,
                                  std::string origin,
                                  std::function<void(bool allow, bool always)> on_response) {
    dsys::SandboxPermissionPayload p;
    p.id = "sandbox-permission";
    p.origin = std::move(origin);
    p.on_response = std::move(on_response);
    queue.push_bottom(dsys::DialogType::SandboxPermission,
                      dsys::DialogPriority::Band1,
                      std::move(p));
}

// ---------------------------------------------------------------------------
// SettingsPanel / HelpView / LspRecommendation / ModelSwitch
// ---------------------------------------------------------------------------
inline void PushSettingsPanel(dsys::DialogQueue& queue,
                              std::string initial_tab,
                              std::function<void()> on_close) {
    dsys::GenericDialogPayload p;
    p.id = "settings-panel";
    p.title = "Settings";
    p.message = std::move(initial_tab);
    p.buttons = {"Close"};
    p.default_button = 0;
    p.on_button = [on_close = std::move(on_close)](int) {
        if (on_close) on_close();
    };
    queue.push_modal(dsys::DialogType::SettingsPanel, std::move(p));
}

inline void PushHelpView(dsys::DialogQueue& queue,
                         std::string section,
                         std::function<void()> on_close) {
    dsys::GenericDialogPayload p;
    p.id = "help-view";
    p.title = "Help";
    p.message = std::move(section);
    p.buttons = {"Close"};
    p.default_button = 0;
    p.on_button = [on_close = std::move(on_close)](int) {
        if (on_close) on_close();
    };
    queue.push_modal(dsys::DialogType::SettingsPanel, std::move(p));
}

inline void PushLspRecommendation(dsys::DialogQueue& queue,
                                  std::string server_name,
                                  std::function<void(bool install)> on_response) {
    dsys::PromptDialogPayload p;
    p.id = "lsp-rec";
    p.title = "LSP Recommendation";
    p.prompt_text = "Install " + server_name + " for better IDE support?";
    p.default_value = std::string{"yes"};
    p.on_response = [on_response = std::move(on_response)](std::optional<std::string> v) {
        if (on_response) on_response(v.has_value());
    };
    queue.push_bottom(dsys::DialogType::PromptDialog,
                      dsys::DialogPriority::Band5,
                      std::move(p));
}

inline void PushModelSwitch(dsys::DialogQueue& queue,
                            std::string current_model,
                            std::string target_model,
                            std::function<void(bool confirm)> on_response) {
    dsys::PromptDialogPayload p;
    p.id = "model-switch";
    p.title = "Switch model";
    p.prompt_text = "Switch from " + current_model + " to " + target_model + "?";
    p.default_value = std::string{"yes"};
    p.on_response = [on_response = std::move(on_response)](std::optional<std::string> v) {
        if (on_response) on_response(v.has_value());
    };
    queue.push_bottom(dsys::DialogType::PromptDialog,
                      dsys::DialogPriority::Band5,
                      std::move(p));
}

inline void PushMessageSelector(dsys::DialogQueue& queue,
                                std::vector<std::string> options,
                                std::string prompt,
                                std::function<void(int idx)> on_select) {
    dsys::PromptDialogPayload p;
    p.id = "message-selector";
    p.title = std::move(prompt);
    std::string text;
    for (const auto& o : options) {
        text += " - " + o + "\n";
    }
    p.prompt_text = std::move(text);
    p.default_value = std::string{"0"};
    p.on_response = [on_select = std::move(on_select)](std::optional<std::string> v) {
        int idx = 0;
        if (v) {
            try { idx = std::stoi(*v); } catch (...) { idx = 0; }
        }
        if (on_select) on_select(idx);
    };
    queue.push_bottom(dsys::DialogType::PromptDialog,
                      dsys::DialogPriority::Band5,
                      std::move(p));
}

// ---------------------------------------------------------------------------
// CommandMetadata dispatcher (UI:* tag → push corresponding dialog)
// ---------------------------------------------------------------------------
inline bool PushFromCommandMetadata(dsys::DialogQueue& queue,
                                    std::string_view metadata) {
    if (metadata == "CREATE_AGENT" || metadata == "EDIT_AGENT" ||
        metadata.starts_with("EDIT_AGENT|")) {
        dsys::GenericDialogPayload p;
        p.id = "agent-wizard";
        p.title = metadata.starts_with("CREATE") ? "Create Agent" : "Edit Agent";
        p.message = std::string{metadata};
        p.buttons = {"Cancel"};
        p.default_button = 0;
        queue.push_standalone(dsys::DialogType::GenericDialog, std::move(p));
        return true;
    }
    if (metadata == "UI:settings") {
        PushSettingsPanel(queue, "general", [](){});
        return true;
    }
    if (metadata == "UI:help") {
        PushHelpView(queue, "commands", [](){});
        return true;
    }
    if (metadata == "UI:config") {
        dsys::GenericDialogPayload p;
        p.id = "config";
        p.title = "Config";
        p.message = "Edit configuration";
        p.buttons = {"Close"};
        queue.push_modal(dsys::DialogType::SettingsPanel, std::move(p));
        return true;
    }
    if (metadata == "UI:mcp") {
        dsys::GenericDialogPayload p;
        p.id = "mcp";
        p.title = "MCP";
        p.message = "MCP servers";
        p.buttons = {"Close"};
        queue.push_modal(dsys::DialogType::SettingsPanel, std::move(p));
        return true;
    }
    if (metadata == "UI:plugins:manage-plugins") {
        dsys::GenericDialogPayload p;
        p.id = "plugins";
        p.title = "Plugins";
        p.message = "Manage plugins";
        p.buttons = {"Close"};
        queue.push_modal(dsys::DialogType::SettingsPanel, std::move(p));
        return true;
    }
    if (metadata.starts_with("UI:undercover|") ||
        metadata.starts_with("UI:effort|") ||
        metadata.starts_with("UI:remote|") ||
        metadata.starts_with("UI:lsp-rec|") ||
        metadata.starts_with("UI:plugin-hint|")) {
        dsys::PromptDialogPayload p;
        p.id = "callout";
        p.title = "Tip";
        p.prompt_text = std::string{metadata};
        p.default_value = std::string{"ok"};
        queue.push_bottom(dsys::DialogType::PromptDialog,
                          dsys::DialogPriority::Band6,
                          std::move(p));
        return true;
    }
    return false;
}

} // namespace cc::ui::dialogs::triggers
