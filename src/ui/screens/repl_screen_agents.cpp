// repl_screen_agents.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). the agent wizard helpers and the whole agents_menu cluster:
// AgentMenuListBase out-of-line ctor/Render/OnEvent + static row helpers,
// the AgentMenuList factory, and the lazy agents-component accessors.
//
// Phase A (#184957): textual FTXUI GMF headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

module cc.ui.screens.repl_screen;

import std;

import cc.ui.features.agents.agent_wizard;
import cc.ui.dialogs.system;
import cc.ui.features.agents.agent_cards;
import cc.tools.agent_display;
import cc.ui.foundation.theme_provider;

namespace cc::ui::repl_screen {
using namespace ftxui;

namespace dialog_router {

// -------------------------------------------------------------------
// Agent wizard helpers
// -------------------------------------------------------------------

namespace wizard_ns = cc::ui::agents::wizard;

using wizard_ns::AgentWizardOptions;
using wizard_ns::WizardDraft;

/// Lazily create (or re-create) the agent wizard component.
/// The mode (create vs edit) and agent_id are read from state.
[[nodiscard]] std::shared_ptr<Component> get_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_agent) {
        AgentWizardOptions opts;
        // M7: Read agent_id from the Standalone-slot EditAgentWizardPayload
        // in dialog_queue (instead of legacy DialogContext bridge struct).
        namespace dsys_gw = cc::ui::dialogs::system;
        auto& q_gw = s->dialog_queue;
        if (cb->load_agent_for_wizard &&
            q_gw.contains_type(dsys_gw::DialogType::EditAgentWizard)) {
            auto st = q_gw.peek_standalone();
            if (st) {
                // peek_standalone() returns optional<reference_wrapper<const V>>.
                // Unwrap with .get() so std::get_if<T> sees a const V*.
                const auto& variant = st->get();
                auto* p = std::get_if<dsys_gw::EditAgentWizardPayload>(&variant);
                if (p && !p->agent_name.empty()) {
                    opts.edit_agent = cb->load_agent_for_wizard(p->agent_name);
                }
            }
        }
        opts.on_save = [s, cb](const WizardDraft& draft) {
            if (cb->save_agent_from_wizard) cb->save_agent_from_wizard(draft);
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        opts.on_cancel = [s, cb] {
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->wizard_agent = std::make_shared<Component>(
            wizard_ns::AgentWizard(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->wizard_agent);
}

/// Forward an event to the agent wizard component.
bool forward_agent(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz && (*wiz)->OnEvent(std::move(ev));
}

/// Render the agent wizard content as an Element.
[[nodiscard]] Element render_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz ? (*wiz)->Render() : text("");
}

/// Reset (destroy) the agent wizard so the next entry starts fresh.
void reset_agent_wizard(const std::shared_ptr<ReplScreenState>& s) {
    s->wizard_agent.reset();
}

// -------------------------------------------------------------------
// Agents menu helpers (UI13)
// -------------------------------------------------------------------

namespace agents_menu {

namespace cards = cc::ui::agents::cards;
namespace agent_display = cc::tools::agent_display;

[[nodiscard]] bool is_built_in(const cards::AgentCardData& agent) {
    return agent.source == "built-in";
}

[[nodiscard]] std::string resolved_model_label(
    const cards::AgentCardData& agent) {
    if (agent.model_override && !agent.model_override->empty()) {
        return *agent.model_override;
    }
    return is_built_in(agent) ? "inherit" : "";
}

[[nodiscard]] std::vector<std::size_t> selectable_indices(
    const std::vector<cards::AgentCardData>& agents) {
    std::vector<std::size_t> out;
    out.reserve(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        if (!is_built_in(agents[i])) out.push_back(i);
    }
    return out;
}


AgentMenuListBase::AgentMenuListBase(AgentMenuOptions opts)
    : opts_(std::move(opts)) {}

Element AgentMenuListBase::Render() {
        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (selected_position_ < 0 || selected_position_ >= item_count) {
            selected_position_ = 0;
        }

        const auto theme = cc::ui::design::theme::current_theme();
        const auto accent = theme.palette->primary;
        const auto muted = theme.palette->muted;
        const auto suggestion = theme.palette->suggestion;
        const int active_count = static_cast<int>(opts_.agents.size());

        Elements rows;
        rows.push_back(render_create_row(selected_position_ == 0, suggestion));

        for (const auto& group : agent_display::agent_source_groups()) {
            if (group.source == "built-in") continue;
            Elements group_rows;
            for (std::size_t i = 0; i < opts_.agents.size(); ++i) {
                const auto& agent = opts_.agents[i];
                if (agent.source != group.source) continue;
                group_rows.push_back(render_agent_row(
                    agent,
                    selected_position_for_index(selectable, i) == selected_position_,
                    /*selectable=*/true,
                    suggestion,
                    muted));
            }
            if (group_rows.empty()) continue;
            rows.push_back(text(""));
            rows.push_back(text("  " + group.label) | bold | color(muted) | dim);
            for (auto& row : group_rows) rows.push_back(std::move(row));
        }

        Elements built_in;
        for (const auto& agent : opts_.agents) {
            if (!is_built_in(agent)) continue;
            built_in.push_back(render_agent_row(
                agent,
                /*selected=*/false,
                /*selectable=*/false,
                suggestion,
                muted));
        }
        if (!built_in.empty()) {
            rows.push_back(text(""));
            rows.push_back(hbox({
                text("  Built-in agents") | bold | color(muted) | dim,
                text(" (always available)") | color(muted) | dim,
            }));
            for (auto& row : built_in) rows.push_back(std::move(row));
        }

        Element body = vbox(std::move(rows))
            | vscroll_indicator
            | yframe
            | size(HEIGHT, LESS_THAN, 26);

        Element title = vbox({
            text("Agents") | bold | color(accent),
            text(std::format("{} agents", active_count)) | color(muted) | dim,
        });

        return vbox({
            title,
            text(""),
            std::move(body),
        }) | flex;
    }

bool AgentMenuListBase::OnEvent(Event ev) {
        if (ev == Event::Escape) {
            if (opts_.on_cancel) opts_.on_cancel();
            return true;
        }

        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (item_count <= 0) return false;

        if (ev == Event::ArrowDown || ev == Event::Character('j')) {
            selected_position_ = (selected_position_ + 1) % item_count;
            return true;
        }
        if (ev == Event::ArrowUp || ev == Event::Character('k')) {
            selected_position_ = (selected_position_ - 1 + item_count) % item_count;
            return true;
        }
        if (ev == Event::Return) {
            if (selected_position_ == 0) {
                if (opts_.on_create_new) opts_.on_create_new();
                return true;
            }
            const int idx = selected_position_ - 1;
            if (idx >= 0 && idx < static_cast<int>(selectable.size())) {
                const auto& agent = opts_.agents[selectable[static_cast<std::size_t>(idx)]];
                if (opts_.on_select) opts_.on_select(agent.id);
            }
            return true;
        }
        return false;
    }

[[nodiscard]] int AgentMenuListBase::selected_position_for_index(
        const std::vector<std::size_t>& selectable,
        std::size_t index) {
        for (std::size_t i = 0; i < selectable.size(); ++i) {
            if (selectable[i] == index) return static_cast<int>(i) + 1;
        }
        return -1;
    }

[[nodiscard]] Element AgentMenuListBase::render_create_row(bool selected, Color suggestion) {
        const auto c = selected ? suggestion : Color::Default;
        return hbox({
            text(selected ? "  › " : "    ") | color(c) | bold,
            text("Create new agent") | color(c),
        });
    }

[[nodiscard]] Element AgentMenuListBase::render_agent_row(
        const cards::AgentCardData& agent,
        bool selected,
        bool selectable,
        Color suggestion,
        Color muted) {
        const bool dimmed = !selectable;
        const auto c = selected ? suggestion : Color::Default;
        const auto model = resolved_model_label(agent);
        Elements parts;
        parts.push_back(text(selectable ? (selected ? "  › " : "    ") : "    ")
            | color(c) | bold);
        Element name = text(agent.name) | color(c);
        if (dimmed) name = name | dim;
        parts.push_back(std::move(name));
        if (!model.empty()) {
            parts.push_back(text(" · " + model)
                | color(selected ? c : muted)
                | dim);
        }
        return hbox(std::move(parts));
    }

[[nodiscard]] Component AgentMenuList(AgentMenuOptions opts) {
    return Make<AgentMenuListBase>(std::move(opts));
}

} // namespace agents_menu

void close_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    s->mode = ReplMode::Normal;
    s->agents_component.reset();
    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
}

[[nodiscard]] std::shared_ptr<Component> get_agents_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->agents_component) {
        agents_menu::AgentMenuOptions opts;
        opts.agents = s->agent_cards;
        opts.on_create_new = [s, cb] {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents create");
        };
        opts.on_select = [s, cb](const std::string& id) {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents configure " + id);
        };
        opts.on_cancel = [s, cb] {
            close_agents_menu(s, cb);
        };
        s->agents_component = std::make_shared<Component>(
            agents_menu::AgentMenuList(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->agents_component);
}

[[nodiscard]] Element render_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_agents_component(s, cb);
    Element footer = text("  Press ↑↓ to navigate · Enter to select · Esc to go back")
        | color(cc::ui::design::theme::current_theme().palette->muted)
        | dim;
    return comp ? vbox({(*comp)->Render(), std::move(footer)}) | flex : text("");
}

bool forward_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_agents_component(s, cb);
    if (comp && (*comp)->OnEvent(ev)) return true;
    if (ev == Event::Escape) {
        close_agents_menu(s, cb);
        return true;
    }
    return false;
}

}  // namespace dialog_router

}  // namespace cc::ui::repl_screen
