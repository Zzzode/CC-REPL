/// @file bottom_renderers.cppm
/// @brief Renderer registration for BOTTOM-slot dialogs.
///
/// Historically this module was split out for performance.  For build
/// integrity it now simply re-exports the default renderers (all of
/// which delegate to the single source-of-truth modules).
module;
#include <vector>
export module cc.ui.dialogs.bottom_renderers;
import cc.ui.dialogs.default_renderers;

export namespace cc::ui::dialogs::bottom_renderers {
using cc::ui::dialogs::default_renderers::register_default_renderers;
}
