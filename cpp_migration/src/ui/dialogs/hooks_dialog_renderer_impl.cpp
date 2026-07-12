/// @file hooks_dialog_renderer_impl.cpp
/// @brief HooksConfig dialog renderer implementation — module impl unit.
///
/// This file contains the ACTUAL implementation (holder, conversion
/// functions, renderer lambda).  It is a module implementation unit
/// (`module cc.ui.dialogs.hooks_renderer;` without `export`), so its
/// heavy imports (hooks_ui, hooks_config, hooks_registry, registry)
/// have their own independent source-location budget.  Importers of
/// the module interface never see this closure.
module;

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

module cc.ui.dialogs.hooks_renderer;

import cc.ui.hooks_ui;
import cc.utils.hooks_config;
import cc.utils.hooks_registry;
import cc.tools.registry;

namespace cc::ui::dialogs::hooks_renderer {

using namespace ftxui;
namespace dsys = cc::ui::dialogs::system;

// ============================================================
// Conversion helpers — registry model → UI display model
// ============================================================

namespace {

/// Convert a registry HookEventType to the UI's HookEvent enum.
/// Returns nullopt for event types the UI doesn't display.
[[nodiscard]] std::optional<cc::ui::hooks_ui::HookEvent> ConvertHookEvent(
    cc::utils::hooks_registry::HookEventType ev)
{
    using UIEvent = cc::ui::hooks_ui::HookEvent;
    switch (ev) {
        case cc::utils::hooks_registry::HookEventType::PreToolUse:    return UIEvent::PreToolUse;
        case cc::utils::hooks_registry::HookEventType::PostToolUse:   return UIEvent::PostToolUse;
        case cc::utils::hooks_registry::HookEventType::Notification:  return UIEvent::Notification;
        case cc::utils::hooks_registry::HookEventType::Stop:          return UIEvent::Stop;
        case cc::utils::hooks_registry::HookEventType::SubagentStop:  return UIEvent::SubagentStop;
        default: return std::nullopt;
    }
}

/// Convert a registry HookCommand variant to the UI's HookType.
[[nodiscard]] cc::ui::hooks_ui::HookType ConvertHookType(
    const cc::utils::hooks_registry::HookCommand& cmd)
{
    using UIType = cc::ui::hooks_ui::HookType;
    if (std::holds_alternative<cc::utils::hooks_registry::CommandHookConfig>(cmd)) return UIType::Command;
    if (std::holds_alternative<cc::utils::hooks_registry::PromptHookConfig>(cmd))  return UIType::Prompt;
    if (std::holds_alternative<cc::utils::hooks_registry::AgentHookConfig>(cmd))   return UIType::Agent;
    if (std::holds_alternative<cc::utils::hooks_registry::HttpHookConfig>(cmd))    return UIType::Http;
    return UIType::Command;  // FunctionHookConfig fallback
}

/// Extract the display content string from a HookCommand variant.
[[nodiscard]] std::string ExtractHookContent(
    const cc::utils::hooks_registry::HookCommand& cmd)
{
    using namespace cc::utils::hooks_registry;
    if (auto* c = std::get_if<CommandHookConfig>(&cmd)) return c->command;
    if (auto* p = std::get_if<PromptHookConfig>(&cmd))  return p->prompt;
    if (auto* a = std::get_if<AgentHookConfig>(&cmd))   return a->prompt;
    if (auto* h = std::get_if<HttpHookConfig>(&cmd))    return h->url;
    if (auto* f = std::get_if<FunctionHookConfig>(&cmd)) return f->status_message.value_or("(function hook)");
    return "";
}

/// Extract timeout in ms from a HookCommand variant, or nullopt.
[[nodiscard]] std::optional<int> ExtractHookTimeoutMs(
    const cc::utils::hooks_registry::HookCommand& cmd)
{
    using namespace cc::utils::hooks_registry;
    std::optional<int> secs;
    if (auto* c = std::get_if<CommandHookConfig>(&cmd)) secs = c->timeout_seconds;
    else if (auto* p = std::get_if<PromptHookConfig>(&cmd))  secs = p->timeout_seconds;
    else if (auto* a = std::get_if<AgentHookConfig>(&cmd))   secs = a->timeout_seconds;
    else if (auto* h = std::get_if<HttpHookConfig>(&cmd))    secs = h->timeout_seconds;
    else if (auto* f = std::get_if<FunctionHookConfig>(&cmd)) secs = f->timeout_seconds;
    if (!secs) return std::nullopt;
    return *secs * 1000;
}

/// Convert a registry IndividualHookConfig to the UI's display model.
/// Returns nullopt for hooks whose event type the UI doesn't support.
[[nodiscard]] std::optional<cc::ui::hooks_ui::IndividualHookConfig> ConvertHook(
    const cc::utils::hooks_registry::IndividualHookConfig& src,
    int index)
{
    auto ui_event = ConvertHookEvent(src.event);
    if (!ui_event) return std::nullopt;

    cc::ui::hooks_ui::IndividualHookConfig dst;
    dst.id = std::to_string(index);
    dst.type = ConvertHookType(src.config);
    dst.event = *ui_event;
    dst.matcher.tool_name = src.matcher.value_or("*");
    dst.command_or_content = ExtractHookContent(src.config);
    dst.timeout_ms = ExtractHookTimeoutMs(src.config);
    dst.enabled = true;

    // Build a display name from type + matcher
    std::string type_name;
    using UIType = cc::ui::hooks_ui::HookType;
    switch (dst.type) {
        case UIType::Command: type_name = "cmd"; break;
        case UIType::Prompt:  type_name = "prompt"; break;
        case UIType::Agent:   type_name = "agent"; break;
        case UIType::Http:    type_name = "http"; break;
    }
    dst.name = std::format("{}:{}", type_name, dst.matcher.tool_name);
    return dst;
}

/// Holder component that wraps a HooksConfigMenu so the dialog system can
/// render it and route events to its CatchEvent handler.
struct HooksDialogHolder : public ComponentBase {
    Component menu;

    explicit HooksDialogHolder(Component m) : menu(std::move(m)) {
        ComponentBase::Add(menu);
    }

    Element Render() override { return menu->Render(); }

    bool InjectEvent(const Event& event) {
        return ComponentBase::OnEvent(event);
    }
};

} // anonymous namespace

// ============================================================
// Registration (exported function implementation)
// ============================================================

void register_hooks_renderer(dsys::DialogRendererRegistry& registry) {
    registry.register_dialog(
        dsys::DialogType::HooksConfig,
        /*renderer=*/
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& /*ctx*/) -> Element {
            auto* p = std::get_if<dsys::HooksDialogPayload>(&payload);
            if (!p) return text("");

            // Lazily create the HooksConfigMenu component on first render.
            if (!p->component) {
                using namespace cc::ui::hooks_ui;

                // Gather hooks from the config manager singleton and convert
                // each registry IndividualHookConfig to the UI display model.
                auto& mgr = cc::utils::hooks_config::HooksConfigManager::instance();
                auto registry_hooks = mgr.get_all_hooks();

                std::vector<IndividualHookConfig> ui_hooks;
                ui_hooks.reserve(registry_hooks.size());
                for (size_t i = 0; i < registry_hooks.size(); ++i) {
                    auto converted = ConvertHook(registry_hooks[i],
                                                 static_cast<int>(i));
                    if (converted) ui_hooks.push_back(std::move(*converted));
                }

                // Gather available tool names for matcher display.
                auto tool_names = cc::tools::registry::builtin_tool_names();

                HooksUIOptions opts;
                opts.all_hooks = std::move(ui_hooks);
                opts.tool_names = std::move(tool_names);
                opts.mode = ModeSelectEvent{};
                opts.selected_index = 0;
                opts.on_exit = p->on_close;

                auto menu = HooksConfigMenu(std::move(opts));
                auto holder = std::make_shared<HooksDialogHolder>(
                    std::move(menu));
                p->component = holder;
            }

            auto holder = std::static_pointer_cast<HooksDialogHolder>(
                p->component);
            return holder ? holder->Render() : text("");
        },
        /*event_handler=*/
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::HooksDialogPayload>(&payload);
            if (!p || !p->component) return false;

            auto holder = std::static_pointer_cast<HooksDialogHolder>(
                p->component);
            if (!holder) return false;
            return holder->InjectEvent(event);
        }
    );
}

} // namespace cc::ui::dialogs::hooks_renderer
