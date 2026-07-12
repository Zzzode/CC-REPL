/// @file hooks_dialog_renderer.cppm
/// @brief HooksConfig dialog renderer — thin module interface.
///
/// MODULE:   cc.ui.dialogs.hooks_renderer
/// LICENCE:  Exported.  The interface is thin (just a function declaration)
///           so importers never see the heavy hooks_ui / hooks_config /
///           hooks_registry transitive closure.  The actual implementation
///           lives in hooks_dialog_renderer_impl.cpp (module impl unit),
///           which has its own source-location budget.
module;

#include <string>

export module cc.ui.dialogs.hooks_renderer;

import cc.ui.dialogs.system;

export namespace cc::ui::dialogs::hooks_renderer {

/// Register the HooksConfig dialog renderer into a registry.
/// Implementation lives in hooks_dialog_renderer_impl.cpp.
void register_hooks_renderer(
    cc::ui::dialogs::system::DialogRendererRegistry& registry);

} // namespace cc::ui::dialogs::hooks_renderer
