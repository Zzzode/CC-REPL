// app_dialog_registration_bottom.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports ONLY cc.ui.dialogs.bottom_renderers (11 bottom-slot callouts) so this
// TU's closure stays small. See app_dialog_registration.cppm for the rationale.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.bottom_renderers;

namespace cc::ui::app_dialogs {
void register_bottom_dialog_renderers(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::bottom_renderers::register_bottom_renderers(registry);
}
}  // namespace cc::ui::app_dialogs
