// app_dialog_registration_hooks.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports ONLY cc.ui.dialogs.hooks_renderer (thin interface — just a function
// declaration) so this TU's closure stays small.  The heavy hooks_ui +
// hooks_config + hooks_registry + registry imports live in the hooks_renderer
// module implementation unit (hooks_dialog_renderer_impl.cpp), which has its
// own independent source-location budget.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.hooks_renderer;

namespace cc::ui::app_dialogs {
void register_hooks_dialog_renderer(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::hooks_renderer::register_hooks_renderer(registry);
}
}  // namespace cc::ui::app_dialogs
