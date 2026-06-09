/// @file agent_cards.cppm
/// @brief Three sizes of agent display cards plus status badges.
///
/// Consolidates card rendering migrated from ~10 of the 26 TS files under
/// src/components/agents/ (MiniCard inline, grid Card, LargeCard detail
/// preview, status-badge helpers, avatar wiring, tool-chips footer).
///
/// 3 sizes:
///   * MiniCard   — 1-line: color dot + name + status dot + short desc
///   * Card       — 25% grid: avatar + name bold + role tag + status badge
///                  + 2-line tools + 3-button footer (Run / Edit / Delete)
///   * LargeCard  — single-column detail preview: avatar + name + multi
///                  role-tags + description block + tools fold + run stats
///                  + 4 action buttons
///
/// Reuses:
///   - cc.ui.agents.shared_widgets  (Avatar, StatusDot, RoleTags,
///                                    RunStatsBar, ToolChips, StepTimeline,
///                                    AgentStatus enum)
///   - cc.tools.agent_color_manager (color hash)
///   - cc.ui.components.spinner_animations (running spinner glyph)
///   - cc.tools.agent_runtime::AgentDefinition  (data model, readonly)
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.agents.agent_cards;

import cc.ui.agents.shared_widgets;
import cc.tools.agent_color_manager;
import cc.utils.swarm_backends;
import cc.ui.components.spinner_animations;

export namespace cc::ui::agents::cards {
using namespace ftxui;

using shared::AgentStatus;
using shared::AgentAvatar;
using shared::StatusDot;
using shared::StatusDotOptions;
using shared::RoleTags;
using shared::RunStatsBar;
using shared::RunStats;
using shared::ToolChipsElement;
using shared::ToolChipsOptions;
using shared::StepTimeline;
using shared::TimelineStep;
using shared::SharedAnimState;
using shared::status_color;
using shared::status_label;

// ============================================================
// Types
// ============================================================

/// Lightweight agent metadata consumed by every card variant.
/// The authoritative source of truth is AgentDefinition in
/// tools/agent_runtime.cppm; this struct keeps the card layer
/// independent of heavy runtime includes.
struct AgentCardData {
    std::string id;
    std::string name;
    std::string agent_type;        ///< e.g. "general-purpose", "explorer"
    std::string description;       ///< 1-liner shown on Mini/Card
    std::string description_long;  ///< Block shown on LargeCard

    std::vector<std::string> role_tags;   ///< e.g. {"coder", "reviewer"}
    std::vector<std::string> tools;       ///< Enabled tool names
    std::vector<std::string> env_vars;    ///< KEY=VALUE summary

    AgentStatus status = AgentStatus::Idle;
    RunStats stats{};

    std::optional<std::string> model_override;
    std::optional<std::string> permission_mode;  ///< "Ask" / "Allow" / "Deny"

    /// Is this a sub-agent (triggers the 🔀 marker on the avatar).
    bool is_subagent = false;

    /// Last run timestamp (ISO-ish, displayed verbatim).
    std::optional<std::string> last_run_at;
};

/// Which card variant to render.
enum class CardSize {
    Mini,   ///< 1-line inline
    Normal, ///< Grid 25% card
    Large,  ///< Single-column detail preview
};

// ============================================================
// StatusBadge — labeled badge (spinner + color + text)
// ============================================================

/// Render a labeled status badge used by Card and LargeCard headers.
/// Running state reuses kDotsAnimation from spinner_animations.cppm.
[[nodiscard]] inline Element StatusBadge(
    AgentStatus status, uint32_t frame = 0)
{
    Color c = status_color(status);
    std::string glyph;

    switch (status) {
        case AgentStatus::Running:
            glyph = std::string(cc::ui::components::kDotsAnimation[
                frame % cc::ui::components::kDotsAnimation.size()]);
            break;
        case AgentStatus::Idle:      glyph = "●"; break;
        case AgentStatus::Scheduled: glyph = "●"; break;
        case AgentStatus::Errored:   glyph = "✗"; break;
        case AgentStatus::Disabled:  glyph = "○"; break;
    }

    std::string label(status_label(status));
    auto box = hbox({
        text(" " + glyph + " "),
        text(label),
        text(" "),
    }) | color(c) | borderLight;

    if (status == AgentStatus::Disabled) box = box | dim;
    if (status == AgentStatus::Errored)  box = box | color(Color::Red);
    return box;
}

/// Footer button used by Card and LargeCard.
[[nodiscard]] inline Element FooterButton(
    std::string_view label, Color col, bool disabled = false)
{
    using ftxui::color;
    auto txt = text(std::format(" {} ", label)) | bold;
    if (disabled) txt = txt | dim;
    else          txt = txt | color(col);
    return txt | borderLight | color(col);
}

// ============================================================
// MiniCard — 1-line inline row
// ============================================================

[[nodiscard]] inline Element MiniCard(
    const AgentCardData& agent,
    bool selected = false,
    uint32_t anim_frame = 0)
{
    // color dot (from agent_type hash)
    Color accent = Color::GrayLight;
    if (!agent.agent_type.empty()) {
        auto opt = cc::tools::agent_color_manager::get_agent_color(agent.agent_type);
        if (opt) accent = shared::agent_color_to_ftxui(*opt);
    }
    auto color_dot = text(" ● ") | color(accent);

    // status dot (spinner if running)
    StatusDotOptions sd_opts;
    sd_opts.status = agent.status;
    sd_opts.spinner_frame = anim_frame;
    auto status_dot = StatusDot(sd_opts);

    // name (bold) + description (dim)
    auto name_txt = text(agent.name) | bold;
    if (agent.status == AgentStatus::Disabled) name_txt = name_txt | dim | strikethrough;

    Element desc = text("");
    if (!agent.description.empty()) {
        desc = text(" — " + agent.description) | dim;
    }

    auto row = hbox({
        color_dot,
        name_txt,
        text(" "),
        status_dot,
        desc | size(WIDTH, LESS_THAN, 60) | flex,
    });

    if (selected) row = row | bgcolor(Color::RGB(25, 35, 50));
    return row;
}

// ============================================================
// Card — 25% grid card (avatar + name + role + tools + 3 btn footer)
// ============================================================

[[nodiscard]] inline Element Card(
    const AgentCardData& agent,
    bool selected = false,
    uint32_t anim_frame = 0)
{
    // --- Header: avatar + name + role + status badge ---
    shared::AvatarOptions av_opts;
    av_opts.name = agent.name;
    av_opts.agent_type = agent.agent_type;
    av_opts.force_general = !agent.is_subagent
                            && agent.agent_type == "general-purpose";
    av_opts.size_cells = 3;

    auto header = hbox({
        AgentAvatar(av_opts),
        text(" "),
        vbox({
            hbox({
                text(agent.name) | bold,
                filler(),
                StatusBadge(agent.status, anim_frame),
            }),
            agent.role_tags.empty()
                ? text(agent.agent_type) | dim
                : RoleTags(agent.role_tags, 2),
        }) | flex,
    });

    // --- Body: 2 lines of tool names ---
    Element tools_block = text("(no tools)") | dim;
    if (!agent.tools.empty()) {
        ToolChipsOptions tc;
        tc.tools = agent.tools;
        tc.collapse_threshold = 6;
        tc.start_collapsed = true;
        tools_block = ToolChipsElement(tc) | size(HEIGHT, LESS_THAN, 3);
    }

    // --- Footer: Run / Edit / Delete ---
    auto footer = hbox({
        FooterButton("Run",  Color::Green),
        text(" "),
        FooterButton("Edit", Color::Cyan),
        text(" "),
        FooterButton("Del",  Color::Red),
        filler(),
        text(std::format("{} runs", agent.stats.run_count)) | dim,
    });

    auto box = vbox({
        header,
        separator() | dim,
        tools_block,
        separator() | dim,
        footer,
    }) | borderRounded | size(WIDTH, GREATER_THAN, 32);

    if (selected) box = box | bgcolor(Color::RGB(25, 35, 50));

    // Colorize the border with the agent color if available.
    if (!agent.agent_type.empty()) {
        auto opt = cc::tools::agent_color_manager::get_agent_color(agent.agent_type);
        if (opt) box = box | color(shared::agent_color_to_ftxui(*opt));
    }

    return box;
}

// ============================================================
// LargeCard — single-column detail preview
// ============================================================

[[nodiscard]] inline Element LargeCard(
    const AgentCardData& agent,
    bool selected = false,
    uint32_t anim_frame = 0)
{
    // --- Header: big avatar + name + multi role tags + model override ---
    shared::AvatarOptions av_opts;
    av_opts.name = agent.name;
    av_opts.agent_type = agent.agent_type;
    av_opts.force_general = !agent.is_subagent
                            && agent.agent_type == "general-purpose";
    av_opts.size_cells = 4;

    Elements name_line = {
        text(agent.name) | bold | size(HEIGHT, EQUAL, 1),
        filler(),
        StatusBadge(agent.status, anim_frame),
    };

    Elements meta_line;
    if (!agent.role_tags.empty()) {
        meta_line.push_back(RoleTags(agent.role_tags, 6));
    } else {
        meta_line.push_back(text(agent.agent_type) | dim);
    }
    if (agent.model_override) {
        meta_line.push_back(text("  model: ") | dim);
        meta_line.push_back(text(*agent.model_override) | color(Color::Yellow));
    }

    auto header = hbox({
        AgentAvatar(av_opts),
        text("  "),
        vbox({
            hbox(std::move(name_line)),
            hbox(std::move(meta_line)),
        }) | flex,
    });

    // --- Description block ---
    Element desc = text("(no description)") | dim;
    if (!agent.description_long.empty()) {
        auto lines = shared::wrap_lines(agent.description_long, 5, 80);
        Elements row_el;
        for (auto& ln : lines) row_el.push_back(text(ln));
        desc = vbox(std::move(row_el));
    } else if (!agent.description.empty()) {
        desc = text(agent.description) | dim;
    }

    // --- Tools (collapsed chip list) ---
    ToolChipsOptions tc_opts;
    tc_opts.tools = agent.tools;
    tc_opts.collapse_threshold = 10;
    tc_opts.start_collapsed = true;
    auto tools_header = hbox({
        text("Tools") | bold | color(Color::Magenta),
        filler(),
        text(std::format(" ({})", agent.tools.size())) | dim,
    });
    auto tools_block = vbox({
        tools_header,
        ToolChipsElement(tc_opts),
    });

    // --- Permissions summary + env vars (2-col) ---
    Elements perm_rows;
    std::string pm = agent.permission_mode.value_or("Ask (default)");
    perm_rows.push_back(hbox({
        text("  Default: ") | dim,
        text(pm) | color(Color::Cyan),
    }));
    perm_rows.push_back(hbox({
        text("  Allow tools: ") | dim,
        text(std::to_string(agent.tools.size()) + " enabled") | color(Color::Green),
    }));
    auto perms_col = vbox({
        text("Permissions") | bold | color(Color::Cyan),
        vbox(std::move(perm_rows)),
    });

    Elements env_rows;
    int env_count = std::min(3, static_cast<int>(agent.env_vars.size()));
    for (int i = 0; i < env_count; ++i) {
        env_rows.push_back(text("  " + agent.env_vars[i]) | dim);
    }
    int env_hidden = static_cast<int>(agent.env_vars.size()) - env_count;
    if (env_hidden > 0) {
        env_rows.push_back(
            text(std::format("  ... +{} more", env_hidden)) | dim);
    }
    if (env_rows.empty()) env_rows.push_back(text("  (none)") | dim);
    auto env_col = vbox({
        text("Env vars") | bold | color(Color::Blue),
        vbox(std::move(env_rows)),
    });

    auto config_cols = hbox({
        perms_col | flex,
        separator() | dim,
        env_col | flex,
    });

    // --- Run stats bar ---
    auto stats_bar = hbox({
        text("Stats  ") | bold | color(Color::Yellow),
        RunStatsBar(agent.stats),
    });

    // --- 4-button footer ---
    auto footer = hbox({
        FooterButton("▶ Run",   Color::Green),
        text(" "),
        FooterButton("✎ Edit",  Color::Cyan),
        text(" "),
        FooterButton("⎘ Dup",   Color::Yellow),
        text(" "),
        FooterButton("⌫ Del",   Color::Red),
        filler(),
        agent.last_run_at
            ? text("last: " + *agent.last_run_at) | dim
            : text("") | dim,
    });

    auto box = vbox({
        header,
        separator() | dim,
        desc,
        separator() | dim,
        tools_block,
        separator() | dim,
        config_cols,
        separator() | dim,
        stats_bar,
        separator() | dim,
        footer,
    }) | borderRounded;

    if (selected) box = box | bgcolor(Color::RGB(25, 35, 50));

    if (!agent.agent_type.empty()) {
        auto opt = cc::tools::agent_color_manager::get_agent_color(agent.agent_type);
        if (opt) box = box | color(shared::agent_color_to_ftxui(*opt));
    }

    return box;
}

// ============================================================
// Interactive Component — wraps a card with keyboard dispatch
// ============================================================

/// Callbacks dispatched by AgentCardComponent.
struct AgentCardCallbacks {
    std::function<void(const std::string& agent_id)> on_run;
    std::function<void(const std::string& agent_id)> on_edit;
    std::function<void(const std::string& agent_id)> on_delete;
    std::function<void(const std::string& agent_id)> on_duplicate;
    std::function<void(const std::string& agent_id)> on_open_detail;
};

/// Create a single interactive agent card. `size` picks the variant; the
/// component handles 'r' (run), 'e' (edit), 'd' (delete), Enter (open).
[[nodiscard]] inline Component AgentCardComponent(
    AgentCardData agent,
    CardSize size,
    std::shared_ptr<SharedAnimState> anim,
    AgentCardCallbacks callbacks = {})
{
    struct Internal {
        AgentCardData data;
        CardSize size;
        std::shared_ptr<SharedAnimState> anim;
        AgentCardCallbacks cbs;
        bool selected = false;
    };

    auto s = std::make_shared<Internal>();
    s->data = std::move(agent);
    s->size = size;
    s->anim = std::move(anim);
    s->cbs = std::move(callbacks);

    return Renderer([s] {
        uint32_t f = s->anim ? s->anim->frame : 0;
        switch (s->size) {
            case CardSize::Mini:   return MiniCard(s->data, s->selected, f);
            case CardSize::Normal: return Card(s->data, s->selected, f);
            case CardSize::Large:  return LargeCard(s->data, s->selected, f);
        }
        return text("");
    }) | CatchEvent([s](Event event) -> bool {
        const std::string& id = s->data.id;
        if (event == Event::Return) {
            if (s->cbs.on_open_detail) s->cbs.on_open_detail(id);
            return true;
        }
        if (event == Event::Character('r')) {
            if (s->cbs.on_run) s->cbs.on_run(id);
            return true;
        }
        if (event == Event::Character('e')) {
            if (s->cbs.on_edit) s->cbs.on_edit(id);
            return true;
        }
        if (event == Event::Character('d')) {
            if (s->cbs.on_delete) s->cbs.on_delete(id);
            return true;
        }
        if (event == Event::Character('D')) {
            if (s->cbs.on_duplicate) s->cbs.on_duplicate(id);
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::agents::cards
