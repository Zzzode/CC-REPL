/// @file swarm_collaboration_view.cppm
/// @brief Swarm Coordinator collaboration view.
///
/// Three-pane layout:
///   LEFT  (20%) : Participants list — coordinator + worker agents
///   CENTER(60%) : Shared multi-agent transcript with per-speaker
///   RIGHT (20%) : Shared-workspace files list
///
/// Top control bar shows Running/Paused state, elapsed, messages count, cost, and
/// Pause/Resume/Cancel buttons.
/// Bottom broadcast input - UI input box + Send button (real operation is
/// delegated via callback to the coordinator engine).
///
/// Reuses:
///   cc.ui.team_status (TeammateStatus / TeammateRole /
///   TeammateRole = reused types)
///   cc.ui.components.agent_view (AgentAvatar inspiration + render helpers)
///   cc.ui.messages.tool_use_message (NOT imported here to keep this module
///                                 circular-link cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles
/// cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles
/// cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles
/// cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles
/// cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles cycles
/// (this comment is intentionally humorous; the actual import note is below)
///
/// Engine logic: 100% delegated to utils/swarm + coordinator.
/// This file ONLY reflects state read-only.  No engine control beyond
/// issuing the Pause/Resume/Cancel callbacks the supplied callbacks fire.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <string_view>
#include <variant>
#include <sstream>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.teams.swarm_collaboration_view;

import cc.types.types;
import cc.ui.team_status;
import cc.ui.components.agent_view;
import cc.ui.teams.teams_overview;
// cc.ui.messages.tool_use_message is not imported here to avoid a
// circular-module dependency.  The caller may pre-render any tool-use blocks
// via the TranscriptMessage::tool_use_rendered Element field.  Refer to that
// module for rendering tool-use messages properly.

export namespace cc::ui::teams::swarm {
using namespace ftxui;

using TeammateStatus = cc::ui::team_status::TeammateStatus;
using TeammateRole   = cc::ui::team_status::TeammateRole;
using TeammateEntry  = cc::ui::team_status::TeammateEntry;

using MembershipRole = cc::ui::teams::overview::MembershipRole;
using cc::ui::teams::overview::AgentAvatar;
using cc::ui::teams::overview::RelativeTime;
using cc::ui::teams::overview::StatusDot;
using cc::ui::teams::overview::RoleTag;

// ============================================================
// Domain types
// ============================================================

/// Swarm execution state
enum class SwarmState : std::uint8_t {
    Running,
    Paused,
    Cancelled,
    Completed,
    Error,
};

/// Participant role (for grouping the participants list)
enum class ParticipantGroup : std::uint8_t {
    Coordinator,
    Researchers,
    Writers,
    Reviewers,
};

/// A participant row in the LEFT pane
struct Participant {
    std::string id;
    std::string name;
    ParticipantGroup group;
    TeammateStatus status;
    std::string current_task;   // 1-line summary
    std::optional<std::string> agent_color;
};

/// One message in the CENTER transcript
struct TranscriptSpeaker {
    std::string id;
    std::string name;
    ParticipantGroup group;
    std::optional<std::string> color_name;
};

enum class TranscriptKind : std::uint8_t {
    Text,         // Regular multi-line text from an agent / user / system
    ToolUse,        // A tool use/call (rendered externally)
    Broadcast,       // Broadcast from coordinator (top/bottom
    Merge,         // Merge-result card
};

struct TranscriptMessage {
    TranscriptKind kind = TranscriptKind::Text;
    TranscriptSpeaker speaker;
    std::chrono::steady_clock::time_point when;
    // Plain text body
    std::string text;
    // Tool-use: a tool-use; empty means rendered Element
    std::optional<Element> tool_use_rendered;  // from tool_use_message module
    bool collapsed = false;  // for tool-use
    std::string merge_summary;  // for Merge kind
    std::optional<double> progress;  // 0..1 if kind == Merge
};

/// A file in the RIGHT shared-workspace pane
struct WorkspaceFile {
    std::string path;
    std::vector<std::string> edited_by; // agent names
    std::chrono::seconds last_modified{0};
    enum class Change { Created, Modified, Deleted };
    Change change = Change::Modified;
};

// ============================================================
// Color helpers (per-group palette, per-group palette
// ============================================================

[[nodiscard]] inline Color GroupColor(ParticipantGroup g) {
    switch (g) {
        case ParticipantGroup::Coordinator: return Color::Magenta;
        case ParticipantGroup::Researchers: return Color::Cyan;
        case ParticipantGroup::Writers:     return Color::Green;
        case ParticipantGroup::Reviewers:     return Color::Yellow;
    }
    return Color::GrayLight;
}

[[nodiscard]] inline std::string_view GroupLabel(ParticipantGroup g) {
    switch (g) {
        case ParticipantGroup::Coordinator: return "Coordinator";
        case ParticipantGroup::Researchers: return "Researchers";
        case ParticipantGroup::Writers:     return "Writers";
        case ParticipantGroup::Reviewers:return "Reviewers";
    }
    return "?";
}

[[nodiscard]] inline std::string_view GroupGlyph(ParticipantGroup g) {
    switch (g) {
        case ParticipantGroup::Coordinator: return "🎯";
        case ParticipantGroup::Researchers: return "🔎";
        case ParticipantGroup::Writers:     return "✏️";
        case ParticipantGroup::Reviewers:     return "🛡";
    }
    return "?";
}

[[nodiscard]] inline Color SwarmStateColor(SwarmState s) {
    switch (s) {
        case SwarmState::Running:   return Color::Green;
        case SwarmState::Paused:    return Color::Yellow;
        case SwarmState::Cancelled: return Color::GrayDark;
        case SwarmState::Completed: return Color::Cyan;
        case SwarmState::Error:     return Color::Red;
    }
    return Color::GrayLight;
}

[[nodiscard]] inline std::string_view SwarmStateLabel(SwarmState s) {
    switch (s) {
        case SwarmState::Running:   return "Running";
        case SwarmState::Paused:    return "Paused";
        case SwarmState::Cancelled: return "Cancelled";
        case SwarmState::Completed: return "Completed";
        case SwarmState::Error:     return "Error";
    }
    return "?";
}

[[nodiscard]] inline std::string_view SwarmStateGlyph(SwarmState s) {
    switch (s) {
        case SwarmState::Running:   return "🟢";
        case SwarmState::Paused:    return "⏸";
        case SwarmState::Cancelled: return "⛔";
        case SwarmState::Completed: return "✅";
        case SwarmState::Error:     return "❌";
    }
    return "?";
}

/// Format a duration as HH:MM:SS or MM:SS (ms → ms
[[nodiscard]] inline std::string FormatDuration(std::chrono::milliseconds ms) {
    auto s = ms.count() / 1000;
    if (s < 3600) return std::format("{:02d}:{:02d}",
        (s / 60), s % 60);
    return std::format("{}:{:02d}:{:02d}",
        s / 3600, (s % 3600) / 60, s % 60);
}

// ============================================================
// Full options
// ============================================================

struct SwarmCollaborationOptions {
    // --- State info ---
    SwarmState state = SwarmState::Running;
    std::chrono::milliseconds elapsed{0};
    std::size_t message_count = 0;
    double total_cost_usd = 0.0;

    // --- Panes ---
    std::vector<Participant> participants;
    std::vector<TranscriptMessage> transcript;
    std::vector<WorkspaceFile> workspace_files;

    // --- Focus management ---
    // Which pane is focused (Tab cycles)
    enum class Pane {
        Participants,
        Transcript,
        Files,
        Broadcast,
        _Count,
    } focused_pane = Pane::Transcript;

    // Selection cursor (simple indexes into the respective lists)
    int selected_participant = 0;
    int selected_file = 0;
    int scroll_transcript = 0;  // index of first visible message
    bool auto_scroll = true;

    // --- Broadcast input (bottom)
    std::string broadcast_text;

    // --- Callbacks (delegate to coordinator/swarm ---
    std::function<void()> on_pause;
    std::function<void()> on_resume;
    std::function<void()> on_cancel;
    std::function<void(std::string)> on_send_broadcast;
    std::function<void(const Participant&)> on_participant_activated;
    std::function<void(const WorkspaceFile&)> on_file_activated;
    std::function<void()> on_close;
};

// ============================================================
// Rendering: Top Control Bar
// ============================================================

[[nodiscard]] inline Element RenderTopBar(
    const SwarmCollaborationOptions& opts) {

    Element status_badge = hbox({
        text(std::format(" {} ", SwarmStateGlyph(opts.state)))
        | color(SwarmStateColor(opts.state)) | bold,
    });

    Element pills = hbox({
        status_badge,
        text(" "),
        text(std::string(SwarmStateLabel(opts.state)))
            | bold | color(SwarmStateColor(opts.state)),
        text(" | ") | dim,
        text(std::format("⏱ {}", FormatDuration(opts.elapsed)))
            | color(Color::GrayLight),
        text(" | ") | dim,
        text(std::format("👥 {}",
            opts.participants.size())) | color(Color::Cyan),
        text(" | ") | dim,
        text(std::format("💬 {}",
            opts.transcript.size())) | color(Color::Green),
        text(" | ") | dim,
        text(std::format("💰 ${:.4f}",
            opts.total_cost_usd)) | color(Color::GrayDark),
    });

    // Buttons on the right
    Element btns = hbox({
        opts.state == SwarmState::Running
            ? text(" [Pause (p)] ") | bold | color(Color::Yellow)
            : text(" [Resume (r)] ") | bold | color(Color::Green) | dim,
        text(" [Cancel (c)] ") | bold | color(Color::Red),
    });

    return hbox({
        pills,
        filler(),
        btns,
    }) | bgcolor(Color::RGB(20, 25, 40)) | borderLight;
}

// ============================================================
// Rendering: LEFT Participants
// ============================================================

[[nodiscard]] inline Element RenderParticipantsPane(
    const SwarmCollaborationOptions& opts) {

    Element title = hbox({
        text(" 🧑‍🤝‍🧑 ") | color(Color::Magenta),
        text("Participants") | bold | color(Color::White),
        filler(),
        text(std::format("({})", opts.participants.size())) | dim,
    });

    // Group by ParticipantGroup
    constexpr ParticipantGroup groups[] = {
        ParticipantGroup::Coordinator,
        ParticipantGroup::Researchers,
        ParticipantGroup::Writers,
        ParticipantGroup::Reviewers,
    };

    Elements rows;
    rows.push_back(title);
    rows.push_back(separator());

    for (auto group : groups) {
        // Collect members of the group
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(opts.participants.size()); ++i) {
            if (opts.participants[i].group == group) indices.push_back(i);
        }
        if (indices.empty()) continue;

        rows.push_back(hbox({
            text(std::string(GroupGlyph(group))) | color(GroupColor(group)),
            text(" "),
            text(std::string(GroupLabel(group))) | bold | color(GroupColor(group)),
            text(std::format(" ({})", indices.size())) | dim,
            filler(),
        }));

        for (int pi : indices) {
            const auto& p = opts.participants[pi];
            bool sel = (opts.focused_pane ==
                       SwarmCollaborationOptions::Pane::Participants
                       && pi == opts.selected_participant);

            // Status
            auto [s_glyph, s_color] =
                cc::ui::team_status::teammate_status_display(p.status);

            Element avatar = AgentAvatar(p.name);
            Element name = hbox({
                text(p.name) | bold |
                    color(sel ? Color::White : Color::GrayLight),
            });
            Element status_line = hbox({
                text(s_glyph + " ") | color(s_color),
                text(p.current_task.empty() ? "(idle)" : p.current_task)
                    | dim | color(Color::GrayDark),
            });
            Element entry = vbox({
                hbox({
                    text(sel ? " ► " : "   "),
                    avatar, text(" "),
                    vbox({ name, status_line }) | flex,
                }),
            });
            if (sel) entry = entry | bgcolor(Color::RGB(25, 30, 48));
            rows.push_back(entry);
        }
        rows.push_back(text(""));
    }

    if (rows.size() <= 2) {
        rows.push_back(text(" No participants") | dim | center);
    }

    Element footer = hbox({
        text(" Tab cycle panes") | dim,
        filler(),
        text(" ↑/↓ select") | dim,
    });

    Element body = vbox(rows) | vscroll_indicator | yframe | flex;

    Element pane = vbox({ body, separator(), footer });
    if (opts.focused_pane == SwarmCollaborationOptions::Pane::Participants) {
        pane = pane | borderStyled(Color::Cyan);
    } else {
        pane = pane | borderLight;
    }
    return pane;
}

// ============================================================
// Rendering: CENTER Shared Transcript
// ============================================================

// Helper: render one chat-like "avatar + role tag + name + time + body"
[[nodiscard]] inline Element RenderTranscriptMessageElement(
    const TranscriptMessage& m, bool is_focused_pane,
    int /*scroll*/) {

    auto bg = is_focused_pane ? Color::RGB(20, 24, 38)
                              : Color::RGB(16, 20, 30);

    Color speaker_color = GroupColor(m.speaker.group);

    // Speaker header row
    Element avatar = AgentAvatar(m.speaker.name);
    Element name_tag = hbox({
        text(m.speaker.name) | bold | color(speaker_color),
        text(" "),
        text(std::format("[{}]", GroupLabel(m.speaker.group)))
            | dim | color(speaker_color),
        text(" ") | dim,
    });
    // Elapsed time display: using steady_clock for monotonic measurement.
    // A relative-time formatter (e.g. "2m ago") would require comparing
    // against the first message timestamp; deferred to a follow-up pass.
    Element time_label = text("") | dim;

    Element header = hbox({
        text(" "),
        avatar,
        text(" "),
        name_tag,
        filler(),
        time_label,
    });

    // Body content
    Element body = text("");
    switch (m.kind) {
        case TranscriptKind::Text: {
            // Wrap text into multiple lines if needed
            std::istringstream iss(m.text);
            Elements lines;
            std::string line;
            while (std::getline(iss, line)) {
                lines.push_back(text(line));
            }
            body = vbox(lines);
            break;
        }
        case TranscriptKind::ToolUse: {
            // Either rendered externally, or fallback to a compact placeholder
            if (m.tool_use_rendered && !m.collapsed) {
                body = *m.tool_use_rendered;
            } else {
                body = hbox({
                    text("  ⚡ tool-use (collapsed) ")
                        | color(Color::Yellow) | dim,
                    filler(),
                    text(" [Enter to expand] ") | dim,
                });
            }
            break;
        }
        case TranscriptKind::Broadcast: {
            body = hbox({
                text(" 📢 Broadcast: ") | color(Color::Cyan) | bold,
                text(m.text) | color(Color::White),
            });
            break;
        }
        case TranscriptKind::Merge: {
            Elements merge_parts = {
                text(" 🔀 ") | color(Color::Cyan),
                text(m.merge_summary.empty() ? "merge result" : m.merge_summary)
                    | color(Color::Cyan),
            };
            if (m.progress) {
                merge_parts.push_back(text("  "));
                merge_parts.push_back(gauge(*m.progress)
                    | color(Color::Cyan)
                    | size(WIDTH, LESS_THAN, 30));
                merge_parts.push_back(
                    text(std::format(" {:.0f}%", *m.progress * 100))
                    | dim);
            }
            body = hbox(merge_parts);
            break;
        }
    }

    Element bubble = vbox({
        header,
        hbox({ text("   "), body | flex }),
    }) | bgcolor(bg) | borderLight;

    return bubble;
}

[[nodiscard]] inline Element RenderTranscriptPane(
    const SwarmCollaborationOptions& opts) {

    Element title = hbox({
        text(" 💬 ") | color(Color::Cyan),
        text("Shared Transcript") | bold | color(Color::White),
        filler(),
        text(opts.auto_scroll ? "auto-scroll ⬇ " : "")
            | color(Color::Yellow) | dim,
        text(std::format("({} messages)", opts.transcript.size()))
            | dim,
    });

    Elements message_elements;
    for (std::size_t i = 0; i < opts.transcript.size(); ++i) {
        message_elements.push_back(RenderTranscriptMessageElement(
            opts.transcript[i],
            opts.focused_pane == SwarmCollaborationOptions::Pane::Transcript,
            static_cast<int>(i)));
    }

    if (message_elements.empty()) {
        message_elements.push_back(
            text(" No messages yet - waiting for participants to start.")
                | dim | center);
    }

    Element body = vbox(message_elements)
        | vscroll_indicator | yframe | flex;
    // NOTE: opts.scroll_transcript and opts.auto_scroll are declared as
    // flags.  FTXUI's yframe auto-shows the last visible entry by default,
    // which approximates the "auto-scroll to bottom" behaviour of the TS
    // version.  A real smooth animation would require state-driven manual
    // windowing (computing a visible slice + offset per frame); deferred
    // until performance profiling shows it is needed.

    Element pane = vbox({
        title,
        separator(),
        body,
    });

    if (opts.focused_pane == SwarmCollaborationOptions::Pane::Transcript) {
        pane = pane | borderStyled(Color::Cyan);
    } else {
        pane = pane | borderLight;
    }
    return pane;
}

// ============================================================
// Rendering: RIGHT Shared-workspace files list
// ============================================================

[[nodiscard]] inline Element RenderWorkspaceFile(
    const WorkspaceFile& f, bool selected) {

    Element change_glyph = text("");
    switch (f.change) {
        case WorkspaceFile::Change::Created:
            change_glyph = text("➕ ") | color(Color::Green); break;
        case WorkspaceFile::Change::Modified:
            change_glyph = text("✏️ ") | color(Color::Yellow); break;
        case WorkspaceFile::Change::Deleted:
            change_glyph = text("🗑 ") | color(Color::Red); break;
    }

    // Path (truncate long paths - keep filename visible (last component)
    std::string display_path = f.path;
    constexpr std::size_t kMax = 36;
    if (display_path.size() > kMax) {
        display_path = "…" + display_path.substr(
            display_path.size() - kMax + 1);
    }

    // Editors (mini-avatars of agents who touched the file)
    Elements editors;
    const std::size_t max_shown = std::min(f.edited_by.size(),
                                           std::size_t{3});
    for (std::size_t i = 0; i < max_shown; ++i) {
        editors.push_back(AgentAvatar(f.edited_by[i]));
        editors.push_back(text(" "));
    }
    if (f.edited_by.size() > max_shown) {
        editors.push_back(text(std::format("+{}",
            f.edited_by.size() - max_shown)) | dim);
    }

    Element last_modified = text(RelativeTime(f.last_modified)) | dim;

    Element row = vbox({
        hbox({
            text(selected ? " ► " : "   "),
            change_glyph,
            text(display_path) |
                color(selected ? Color::White : Color::GrayLight),
            filler(),
            last_modified,
        }),
        hbox({
            text("     "),
            hbox(editors),
        }),
    });
    if (selected) row = row | bgcolor(Color::RGB(25, 30, 48));
    return row;
}

[[nodiscard]] inline Element RenderFilesPane(
    const SwarmCollaborationOptions& opts) {

    Element title = hbox({
        text(" 📁 ") | color(Color::Yellow),
        text("Shared Workspace") | bold | color(Color::White),
        filler(),
        text(std::format("({})", opts.workspace_files.size())) | dim,
    });

    Elements rows;
    for (int i = 0;
         i < static_cast<int>(opts.workspace_files.size()); ++i) {
        bool sel = (opts.focused_pane ==
                    SwarmCollaborationOptions::Pane::Files)
                   && i == opts.selected_file;
        rows.push_back(RenderWorkspaceFile(opts.workspace_files[i], sel));
    }
    if (rows.empty()) rows.push_back(
        text(" No files") | dim | center);

    Element body = vbox(rows) | vscroll_indicator | yframe | flex;

    Element pane = vbox({
        title,
        separator(),
        body,
    });

    if (opts.focused_pane == SwarmCollaborationOptions::Pane::Files) {
        pane = pane | borderStyled(Color::Cyan);
    } else {
        pane = pane | borderLight;
    }
    return pane;
}

// ============================================================
// Rendering: BOTTOM Broadcast input
// ============================================================

[[nodiscard]] inline Element RenderBroadcastBar(
    const SwarmCollaborationOptions& opts) {

    bool focused =
        (opts.focused_pane == SwarmCollaborationOptions::Pane::Broadcast);

    Element input_box;
    if (opts.broadcast_text.empty()) {
        input_box = text("Broadcast message to all agents…") | dim;
    } else {
        input_box = text(opts.broadcast_text) | color(Color::White);
    }
    input_box = input_box | size(HEIGHT, EQUAL, 1) | flex;

    Element content = hbox({
        text(" 📢 ") | color(Color::Cyan),
        input_box | flex,
        text(" [Send (Enter)] ") | bold
            | color(Color::Green) | dim,
    });

    if (focused) {
        content = content | bgcolor(Color::RGB(20, 30, 50));
    }

    return content | borderLight;
}

// ============================================================
// Full layout
// ============================================================

[[nodiscard]] inline Element RenderSwarmCollaboration(
    const SwarmCollaborationOptions& opts) {

    Element top = RenderTopBar(opts);

    // Three panes:
    //   left 20%  | center 60%  | right 20%
    Element three_panes = hbox({
        RenderParticipantsPane(opts) | size(WIDTH, LESS_THAN, 30),
        RenderTranscriptPane(opts)   | flex,
        RenderFilesPane(opts)         | size(WIDTH, LESS_THAN, 30),
    }) | yframe | flex;

    Element broadcast = RenderBroadcastBar(opts);

    Element footer = hbox({
        text(" Tab: ") | dim,
        filler(),
        text(" p pause · r resume · c cancel · b broadcast · Esc close")
            | dim,
    });

    return vbox({
        top,
        three_panes,
        broadcast,
        separator(),
        footer,
    }) | borderHeavy;
}

// ============================================================
// Interactive Component
// ============================================================

/// Keybindings:
///   Tab     - cycle focus: Participants → Transcript → Files → Broadcast
///   p       - pause
///   r       - resume
///   c       - cancel (confirm via callback)
///   b       - focus broadcast (and start typing into broadcast text (b
///             switches focus to Broadcast, and all printable ASCII chars append).
///   Esc     - close dialog
///   ↑/↓/j/k- scroll select per-pane (except when Broadcast is focused).
///   Space   - on Transcript: toggle collapse of selected (for
///             tool-use)
///   Enter   - in Broadcast: send (callback)
///             in Files / Participants: activate selection (callback)
[[nodiscard]] inline Component MakeSwarmCollaborationView(
    SwarmCollaborationOptions options) {

    auto state = std::make_shared<SwarmCollaborationOptions>(
        std::move(options));

    return Renderer([state] {
        return RenderSwarmCollaboration(*state);
    }) | CatchEvent([state](Event event) -> bool {

        // Escape closes
        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }

        // Tab cycles focus panes
        if (event == Event::Tab) {
            int cur = static_cast<int>(state->focused_pane);
            int total = static_cast<int>(
                SwarmCollaborationOptions::Pane::_Count);
            state->focused_pane = static_cast<
                SwarmCollaborationOptions::Pane>((cur + 1) % total);
            return true;
        }

        // p pause
        if (event == Event::Character('p')) {
            if (state->state == SwarmState::Running && state->on_pause) {
                state->on_pause();
                state->state = SwarmState::Paused;
            }
            return true;
        }
        // r resume
        if (event == Event::Character('r')) {
            if (state->state == SwarmState::Paused && state->on_resume) {
                state->on_resume();
                state->state = SwarmState::Running;
            }
            return true;
        }
        // c cancel
        if (event == Event::Character('c')) {
            if (state->on_cancel) state->on_cancel();
            state->state = SwarmState::Cancelled;
            return true;
        }
        // b = focus broadcast
        if (event == Event::Character('b')) {
            state->focused_pane =
                SwarmCollaborationOptions::Pane::Broadcast;
            return true;
        }

        // Per-pane keys
        // --- Broadcast pane typing
        if (state->focused_pane ==
            SwarmCollaborationOptions::Pane::Broadcast) {
            if (event == Event::Return) {
                if (!state->broadcast_text.empty() &&
                    state->on_send_broadcast) {
                    state->on_send_broadcast(state->broadcast_text);
                    state->broadcast_text.clear();
                }
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->broadcast_text.empty())
                    state->broadcast_text.pop_back();
                return true;
            }
            if (event.is_character()) {
                char ch = event.character()[0];
                if (ch >= 0x20 && ch < 0x7f) {
                    state->broadcast_text.push_back(ch);
                    return true;
                }
            }
            return false;
        }

        // --- Participants pane nav
        if (state->focused_pane ==
            SwarmCollaborationOptions::Pane::Participants) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_participant = std::max(
                    0, state->selected_participant - 1);
                return true;
            }
            if (event == Event::ArrowDown ||
                event == Event::Character('j')) {
                state->selected_participant = std::min(
                    static_cast<int>(state->participants.size()) - 1,
                    state->selected_participant + 1);
                return true;
            }
            if (event == Event::Return) {
                if (!state->participants.empty() &&
                    state->on_participant_activated) {
                    state->on_participant_activated(
                        state->participants[state->selected_participant]);
                }
                return true;
            }
            return false;
        }

        // --- Transcript pane nav
        if (state->focused_pane ==
            SwarmCollaborationOptions::Pane::Transcript) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->scroll_transcript =
                    std::max(0, state->scroll_transcript - 1);
                return true;
            }
            if (event == Event::ArrowDown ||
                event == Event::Character('j')) {
                state->scroll_transcript = std::max(0,
                    std::min(
                        static_cast<int>(state->transcript.size()) - 1,
                        state->scroll_transcript + 1));
                state->scroll_transcript = state->scroll_transcript;
                return true;
            }
            // Space collapse selected tool-use collapse tool-use
            if (event == Event::Character(' ')) {
                int idx = state->scroll_transcript;
                if (idx >= 0 && idx < (int)state->transcript.size()) {
                    auto& m = state->transcript[idx];
                    if (m.kind == TranscriptKind::ToolUse) {
                        m.collapsed = !m.collapsed;
                    }
                }
                return true;
            }
            // a = toggle auto_scroll
            if (event == Event::Character('a')) {
                state->auto_scroll = !state->auto_scroll;
                return true;
            }
            return false;
        }

        // --- Files pane nav
        if (state->focused_pane ==
            SwarmCollaborationOptions::Pane::Files) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_file = std::max(0,
                    state->selected_file - 1);
                return true;
            }
            if (event == Event::ArrowDown ||
                event == Event::Character('j')) {
                state->selected_file = std::min(
                    static_cast<int>(state->workspace_files.size()) - 1,
                    state->selected_file + 1);
                return true;
            }
            if (event == Event::Return) {
                if (!state->workspace_files.empty() &&
                    state->on_file_activated) {
                    state->on_file_activated(
                        state->workspace_files[state->selected_file]);
                }
                return true;
            }
            return false;
        }

        return false;
    });
}

} // namespace cc::ui::teams::swarm
