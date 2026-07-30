/// @file plugin_dialog_renderer.cppm
/// @brief PluginDialog dialog renderer — thin module interface.
///
/// MODULE:   cc.ui.dialogs.plugin_dialog_renderer
/// LICENCE:  Exported.  The interface is thin (just a function declaration)
///           so importers never see the heavy plugin_dialog / plugin_ui_data /
///           plugin_marketplace transitive closure.  The actual implementation
///           lives in plugin_dialog_renderer_impl.cpp (module impl unit),
///           which has its own source-location budget.
module;

#include <string>

export module cc.ui.dialogs.plugin_dialog_renderer;

import cc.ui.dialogs.system;

export namespace cc::ui::dialogs::plugin_dialog_renderer {

/// Register the PluginDialog dialog renderer into a registry.
/// Implementation lives in plugin_dialog_renderer_impl.cpp.
void register_plugin_dialog_renderer(
    cc::ui::dialogs::system::DialogRendererRegistry& registry);

} // namespace cc::ui::dialogs::plugin_dialog_renderer
