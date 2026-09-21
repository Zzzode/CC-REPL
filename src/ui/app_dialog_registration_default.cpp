// app_dialog_registration_default.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports cc.ui.dialogs.default_renderers (6 core dialogs) and
// cc.ui.dialogs.plugin_dialog_renderer (thin interface — just a function
// declaration) so this TU's closure stays small.  The heavy plugin_dialog +
// plugin_ui_data + plugin_marketplace imports live in the renderer's
// implementation unit (plugin_dialog_renderer_impl.cpp), which has its own
// independent source-location budget.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.plugin_dialog_renderer;

namespace cc::ui::app_dialogs {
void register_default_dialog_renderers(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::default_renderers::register_default_renderers(registry);
    cc::ui::dialogs::plugin_dialog_renderer::register_plugin_dialog_renderer(
        registry);
}
}  // namespace cc::ui::app_dialogs
