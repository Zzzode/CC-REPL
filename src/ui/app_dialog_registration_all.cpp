// app_dialog_registration_all.cpp — impl unit for cc.ui.app_dialog_registration.
// Imports ONLY cc.ui.dialogs.all_renderers (22 dialog implementations — the
// largest aggregator, but still a single aggregator's closure, which compiles
// fine on its own). See app_dialog_registration.cppm for the rationale.
module cc.ui.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.dialogs.all_renderers;

namespace cc::ui::app_dialogs {
void register_all_dialog_renderers(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    cc::ui::dialogs::all_renderers::register_all_renderers(registry);
}
}  // namespace cc::ui::app_dialogs
