// app_dialog_registration_modal.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports ONLY cc.ui.dialogs.modal_renderers (5 modal dialogs) so this TU's
// closure stays small. See app_dialog_registration.cppm for the rationale.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.modal_renderers;

namespace cc::ui::app_dialogs {
void register_modal_dialog_renderers(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::modal_renderers::register_modal_renderers(registry);
}
}  // namespace cc::ui::app_dialogs
