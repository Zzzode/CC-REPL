/// @file bottom_renderers.cppm
/// @brief Renderer registration for BOTTOM-slot dialogs.
///
/// Historically this module was split out for performance.  For build
/// integrity it now simply re-exports the default renderers (all of
/// which delegate to the single source-of-truth modules).
module;
#include <utility>
#include <vector>
export module cc.ui.dialogs.bottom_renderers;
import cc.ui.dialogs.default_renderers;

export namespace cc::ui::dialogs::bottom_renderers {
using cc::ui::dialogs::default_renderers::register_default_renderers;

/// Named registration entry-point matching the module name.
///
/// Delegates to register_default_renderers with the same argument pack.
/// Callers that write `register_bottom_renderers(reg)` bind here; the
/// dual `using` + template keeps both calling conventions valid.
template <typename... Args>
inline void register_bottom_renderers(Args&&... args) {
    cc::ui::dialogs::default_renderers::register_default_renderers(
        std::forward<Args>(args)...);
}
}  // namespace cc::ui::dialogs::bottom_renderers
