// app_dialog_registration_default.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports ONLY cc.ui.dialogs.default_renderers (6 core dialogs) so this TU's
// closure stays small. See app_dialog_registration.cppm for the rationale.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.default_renderers;

namespace cc::ui::app_dialogs {
void register_default_dialog_renderers(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::default_renderers::register_default_renderers(registry);
}
}  // namespace cc::ui::app_dialogs
