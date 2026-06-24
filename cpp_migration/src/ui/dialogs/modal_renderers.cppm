/// @file modal_renderers.cppm
/// @brief Renderer registration for MODAL (overlay/standalone)-slot dialogs.
///
/// Historically this module was split out for performance.  For build
/// integrity it now simply re-exports the default renderers.
module;
#include <vector>
export module cc.ui.dialogs.modal_renderers;
import cc.ui.dialogs.default_renderers;

export namespace cc::ui::dialogs::modal_renderers {
using cc::ui::dialogs::default_renderers::register_default_renderers;
}
