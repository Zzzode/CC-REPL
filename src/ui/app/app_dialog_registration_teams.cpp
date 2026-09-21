// app_dialog_registration_teams.cpp — impl unit for cc.ui.app_dialog_registration.
//
// TeamsView modal renderer. Kept in its own TU (not appended to
// app_dialog_registration_default.cpp): aggregating dialog closures in one
// translation unit crashes Clang codegen, and the live_teammates + repl_screen
// import closure is independent of every other aggregator's. The renderer
// borrows ReplScreenState from DialogRenderContext for Render() only and
// never retains the pointer; selection index lives in the queue-owned
// TeamsViewPayload.
module;

#include <algorithm>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

module cc.ui.app.app_dialog_registration;

import cc.ui.dialogs.system;
import cc.ui.screens.repl_screen;
import cc.ui.features.teams.live_teammates;
import cc.utils.team_helpers;

using namespace ftxui;

namespace cc::ui::app_dialogs {

void register_teams_dialog_renderer(
    cc::ui::dialogs::system::DialogRendererRegistry& registry) {
    namespace dsys = cc::ui::dialogs::system;

    registry.register_dialog(
        dsys::DialogType::TeamsView,
        /*renderer=*/
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& ctx) -> Element {
            auto* p = std::get_if<dsys::TeamsViewPayload>(&payload);
            if (!p) return text("");
            const auto* s = static_cast<
                const cc::ui::repl_screen::ReplScreenState*>(ctx.repl_state);
            if (!s) return text("");
            const std::string team =
                cc::utils::get_team_name().value_or("default");
            return cc::ui::teams::live::RenderTeamsOverview(
                team, s->live_teammates, p->selected_index,
                ctx.term_cols, ctx.term_rows);
        },
        /*event_handler=*/
        [](dsys::DialogPayloadVariant& payload,
           const Event& ev) -> bool {
            auto* p = std::get_if<dsys::TeamsViewPayload>(&payload);
            if (!p) return false;
            if (ev == Event::ArrowUp || ev == Event::Character('k')) {
                p->selected_index = std::max(0, p->selected_index - 1);
                return true;
            }
            if (ev == Event::ArrowDown || ev == Event::Character('j')) {
                // Roster size is not visible to the handler; RenderTeamsOverview
                // clamps the cursor visually, and the payload cursor simply
                // tracks how far down the user moved.
                if (p->selected_index < 1'000'000) ++p->selected_index;
                return true;
            }
            if (ev == Event::Return) {
                // Enter is reserved for a future pane-view detail surface.
                return true;
            }
            // Escape needs NO handler: DispatchDialogQueueEvents pops the
            // modal stack on unhandled Escape, invoking on_close semantics.
            return false;
        });
}

}  // namespace cc::ui::app_dialogs
