/// @file all_renderers.cppm
/// @brief Aggregator import: pulls in every renderer module.
///
/// This module exists so call sites can import a single module to
/// have every dialog renderer available.  Currently the only concrete
/// registry implementations live in `default_renderers`.
module;
#include <vector>
export module cc.ui.dialogs.all_renderers;

import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.bottom_renderers;
import cc.ui.dialogs.modal_renderers;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.sandbox_permission;
import cc.ui.dialogs.quick_open;

export namespace cc::ui::dialogs::all_renderers {
using cc::ui::dialogs::default_renderers::register_default_renderers;
}
