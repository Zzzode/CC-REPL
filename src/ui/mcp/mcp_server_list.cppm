/// @file mcp_server_list.cppm
/// @brief MCP Server List panel — migrated from TS components/mcp/ (MCPListPanel,
/// MCPSettings, MCPToolListView). Left-side server list grouped by scope with
/// status lights, right-side detail preview, toolbar, search, keyboard nav.
///
/// All engine calls (connect/disconnect/restart/reload/config) are delegated
/// to cc::services::mcp::ConnectionManager and cc::services::mcp::ConfigLoader.
module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <format>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.mcp.mcp_server_list;

import cc.services.mcp.types;
import cc.services.mcp.connection_manager;
import cc.services.mcp.config;

export namespace cc::ui::mcp {
using namespace ftxui;
using namespace cc::services::mcp;

// ============================================================
// Row-level display types (flattened view-model, not engine state)
// ============================================================

/// Row kind so keyboard nav works uniformly across scope headings + real rows.
enum class ListRowKind : std::uint8_t {
    Heading,
    Server,
    AgentServer,
};

struct ListRow {
    ListRowKind kind = ListRowKind::Server;
    std::string heading_label;
    std::string heading_sub;        // scope path / agent name
    // --- server fields ---
    std::string server_name;
    ConnectionStatus status = ConnectionStatus::Disconnected;
    TransportType transport = TransportType::Stdio;
    ConfigScope scope = ConfigScope::Project;
    int tool_count = 0;
    int resource_count = 0;
    int prompt_count = 0;
    std::optional<std::string> last_error;
    bool is_authenticated = false;
    // --- agent server fields ---
    std::string agent_name;
    bool agent_needs_auth = false;
};

// ============================================================
// Props / factory options
// ============================================================

struct ServerListProps {
    // Engine hooks (all optional, caller wires to ConnectionManager).
    std::function<std::vector<McpServerSnapshot>()> on_refresh;
    std::function<void(const std::string& server_name)> on_connect;
    std::function<void(const std::string& server_name)> on_disconnect;
    std::function<void(const std::string& server_name)> on_restart;
    std::function<void(const std::string& server_name)> on_delete;
    std::function<void(const std::string& server_name)> on_open_details;
    std::function<void()> on_open_add_wizard;

    // Exit handlers.
    std::function<void()> on_cancel;
    std::function<void(std::string message)> on_complete;
};

// ============================================================
// Rendering helpers
// ============================================================

inline std::pair<std::string, Color> status_badge(ConnectionStatus s) {
    switch (s) {
        case ConnectionStatus::Connected:    return {"●", Color::Green};
        case ConnectionStatus::Connecting:   return {"◐", Color::Yellow};
        case ConnectionStatus::Disconnected: return {"○", Color::GrayDark};
        case ConnectionStatus::NeedsAuth:    return {"△", Color::YellowLight};
        case ConnectionStatus::Error:        return {"✗", Color::Red};
    }
    return {"?", Color::White};
}

inline std::string transport_icon(TransportType t) {
    switch (t) {
        case TransportType::Stdio:         return "⟷";
        case TransportType::Sse:           return "⬈";
        case TransportType::Http:          return "⇅";
        case TransportType::StreamableHttp:return "⇆";
    }
    return "·";
}

inline std::string transport_label(TransportType t) {
    switch (t) {
        case TransportType::Stdio:         return "stdio";
        case TransportType::Sse:           return "SSE";
        case TransportType::Http:          return "HTTP";
        case TransportType::StreamableHttp:return "WS";
    }
    return "?";
}

inline std::string scope_heading(ConfigScope scope) {
    switch (scope) {
        case ConfigScope::Global:  return "Global MCPs";
        case ConfigScope::User:    return "User MCPs";
        case ConfigScope::Project: return "Project MCPs";
        case ConfigScope::Local:   return "Local MCPs";
    }
    return "MCPs";
}

/// Count summary used by bottom status line.
struct Totals {
    int total = 0;
    int connected = 0;
    int tools = 0;
};

// ============================================================
// Element rendering
// ============================================================

[[nodiscard]] inline Element RenderToolbar(const std::string& search_query, bool focus_search) {
    auto add_btn = hbox({
        text(" [") | color(Color::Cyan),
        text("+") | bold | color(Color::Cyan),
        text("] Add") | color(Color::Cyan),
    });
    auto reload_btn = hbox({
        text(" [") | dim,
        text("R") | bold | dim,
        text("] Reload all") | dim,
    });

    auto search_el = hbox({
        text("/") | color(Color::Cyan),
        text(search_query.empty() ? " filter…" : search_query)
            | (focus_search ? inverted : nothing),
    });

    return hbox({
        add_btn,
        text("  "),
        reload_btn,
        filler(),
        search_el | size(WIDTH, GREATER_THAN, 20),
    });
}

[[nodiscard]] inline Element RenderServerRow(const ListRow& row, bool selected, bool hover_actions) {
    auto [icon, icon_color] = status_badge(row.status);

    Elements left;
    left.push_back(text(icon + " ") | color(icon_color));
    left.push_back(text(row.server_name) | (selected ? bold : nothing));
    if (row.is_authenticated) {
        left.push_back(text(" 🔒") | dim);
    }

    auto transport_badge = hbox({
        text(" " + transport_icon(row.transport) + " ") | dim,
        text(transport_label(row.transport)) | dim | size(WIDTH, EQUAL, 5),
    });

    Elements counts;
    counts.push_back(text("🔧") | dim);
    counts.push_back(text(std::format("{}", row.tool_count)) | dim);
    counts.push_back(text("  📦") | dim);
    counts.push_back(text(std::format("{}", row.resource_count)) | dim);
    counts.push_back(text("  📋") | dim);
    counts.push_back(text(std::format("{}", row.prompt_count)) | dim);

    Element actions = text("");
    if (selected && hover_actions) {
        actions = hbox({
            text("   [D]el") | color(Color::RedLight) | dim,
            text("  [R]st") | color(Color::Yellow) | dim,
            text("  [C]on") | color(Color::Green) | dim,
        });
    }

    auto line = hbox({
        text(" "),
        hbox(std::move(left)),
        text("  "),
        transport_badge,
        text("   "),
        hbox(std::move(counts)),
        filler(),
        actions,
    });

    if (selected) line = line | bgcolor(Color::RGB(30, 45, 65));

    // Append error preview on a second line when selected & error present.
    if (selected && row.last_error) {
        return vbox({
            line,
            text("    ✗ " + *row.last_error) | color(Color::Red) | dim,
        });
    }
    return line;
}

[[nodiscard]] inline Element RenderAgentServerRow(const ListRow& row, bool selected) {
    auto badge = row.agent_needs_auth
        ? hbox({text("△ ") | color(Color::Yellow), text("may need auth") | dim})
        : hbox({text("○ ") | color(Color::GrayDark), text("agent-only") | dim});

    auto line = hbox({
        text(" @"),
        text(row.agent_name) | color(Color::Magenta) | dim,
        text("  "),
        text(row.server_name) | (selected ? bold : nothing),
        text("   "),
        badge,
        filler(),
    });
    if (selected) line = line | bgcolor(Color::RGB(40, 30, 55));
    return line;
}

[[nodiscard]] inline Element RenderHeadingRow(const ListRow& row) {
    Elements parts;
    parts.push_back(text(" " + row.heading_label) | bold | dim | color(Color::Cyan));
    if (!row.heading_sub.empty()) {
        parts.push_back(text("  (" + row.heading_sub + ")") | dim);
    }
    return vbox({
        text(""),
        hbox(std::move(parts)),
    });
}

[[nodiscard]] inline Element RenderListPanel(
    const std::vector<ListRow>& rows,
    int selected,
    const std::string& filter) {

    Elements items;
    items.push_back(RenderToolbar(filter, false));

    if (rows.empty()) {
        items.push_back(text(""));
        items.push_back(text("  No MCP servers configured.") | dim);
        items.push_back(text("  Press [a] to add your first server.") | dim | color(Color::GrayLight));
    }

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[i];
        bool is_sel = (i == selected);
        switch (r.kind) {
            case ListRowKind::Heading:
                items.push_back(RenderHeadingRow(r));
                break;
            case ListRowKind::Server:
                items.push_back(RenderServerRow(r, is_sel, is_sel));
                break;
            case ListRowKind::AgentServer:
                items.push_back(RenderAgentServerRow(r, is_sel));
                break;
        }
    }

    return vbox(items) | yframe;
}

[[nodiscard]] inline Element RenderDetailPreview(
    const ListRow* sel_row,
    const std::vector<McpTool>& recent_tools) {

    Elements content;
    content.push_back(hbox({
        text(" Server Details") | bold | color(Color::Blue),
    }));
    content.push_back(separator());

    if (!sel_row) {
        content.push_back(text(""));
        content.push_back(text("  Select a server on the left.") | dim);
        return vbox(content) | border;
    }

    const auto& r = *sel_row;
    auto [icon, ic] = status_badge(r.status);

    content.push_back(hbox({
        text("  " + icon + " ") | color(ic),
        text(r.server_name) | bold,
        filler(),
        text(transport_label(r.transport)) | dim,
        text(" / ") | dim,
        text(scope_heading(r.scope)) | dim,
    }));
    content.push_back(text(""));

    content.push_back(hbox({
        text("  Tools: ") | dim,
        text(std::format("{}", r.tool_count)) | bold,
        text("   Resources: ") | dim,
        text(std::format("{}", r.resource_count)) | bold,
        text("   Prompts: ") | dim,
        text(std::format("{}", r.prompt_count)) | bold,
    }));

    if (r.last_error) {
        content.push_back(text(""));
        content.push_back(hbox({
            text("  ✗ ") | color(Color::Red),
            paragraph("  " + *r.last_error) | color(Color::RedLight),
        }));
    }

    // Recent tools preview
    if (!recent_tools.empty()) {
        content.push_back(text(""));
        content.push_back(separator());
        content.push_back(text("  Recent tools:") | dim);
        for (std::size_t i = 0; i < std::min<std::size_t>(6, recent_tools.size()); ++i) {
            const auto& t = recent_tools[i];
            content.push_back(hbox({
                text("    🔧 ") | dim,
                text(t.name) | bold,
                text(" — ") | dim,
                paragraph(t.description.size() > 60 ? t.description.substr(0, 60) + "…" : t.description) | dim,
            }));
        }
    }

    content.push_back(text(""));
    content.push_back(text("  [Enter] open details   [d] delete   [r] restart   [c] reconnect") | dim);

    return vbox(content) | border;
}

[[nodiscard]] inline Element RenderStatusBar(const Totals& totals) {
    return hbox({
        text(" Total: ") | dim,
        text(std::format("{}", totals.total)) | bold,
        text(" servers, ") | dim,
        text(std::format("{}", totals.connected)) | color(Color::Green) | bold,
        text(" connected, ") | dim,
        text(std::format("{}", totals.tools)) | color(Color::Cyan) | bold,
        text(" tools available globally") | dim,
        filler(),
    }) | dim;
}

[[nodiscard]] inline Element RenderServerList(
    const std::vector<ListRow>& rows,
    int selected,
    const std::string& filter,
    const ListRow* sel_row,
    const std::vector<McpTool>& preview_tools,
    const Totals& totals) {

    auto left = RenderListPanel(rows, selected, filter) | size(WIDTH, GREATER_THAN, 42);
    auto right = RenderDetailPreview(sel_row, preview_tools) | flex;

    return vbox({
        hbox({
            text(" 🔌 MCP Server Management ") | bold | color(Color::Cyan),
            filler(),
        }),
        separator(),
        hbox({left | flex, separator(), right}) | flex,
        RenderStatusBar(totals),
        hbox({
            text(" j/k") | color(Color::Cyan), text(":nav "),
            text("Enter") | color(Color::Cyan), text(":details "),
            text("a") | color(Color::Cyan), text(":add "),
            text("r") | color(Color::Cyan), text(":restart "),
            text("d") | color(Color::Cyan), text(":delete "),
            text("/") | color(Color::Cyan), text(":search "),
            text("Esc") | color(Color::Cyan), text(":close"),
        }) | dim,
    });
}

// ============================================================
// View-model builder: pull from engine snapshot into flat rows.
// Declared inline so callers with ConnectionManager access can re-use it.
// ============================================================

[[nodiscard]] inline std::vector<ListRow> BuildRowsFromSnapshots(
    const std::vector<McpServerSnapshot>& servers,
    const std::string& filter) {

    std::vector<ListRow> rows;
    auto match = [&](std::string_view name) {
        if (filter.empty()) return true;
        std::string lower_name;
        lower_name.reserve(name.size());
        for (char c : name) lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        std::string lower_f;
        lower_f.reserve(filter.size());
        for (char c : filter) lower_f.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return lower_name.find(lower_f) != std::string::npos;
    };

    // Group by scope in stable order.
    const std::vector<ConfigScope> order = {
        ConfigScope::Project, ConfigScope::Local,
        ConfigScope::User, ConfigScope::Global,
    };

    for (auto scope : order) {
        std::vector<McpServerSnapshot> in_scope;
        for (const auto& s : servers) {
            // Snapshot doesn't carry scope — callers can enrich via config lookup.
            // If scope unknown we use Project as default.
            if (match(s.name)) in_scope.push_back(s);
        }
        // Because scope info is richer in ServerConfig than McpServerSnapshot,
        // a fully wired caller should pre-group; we only do a best-effort render.
        if (!in_scope.empty() && rows.empty()) {
            ListRow heading;
            heading.kind = ListRowKind::Heading;
            heading.heading_label = "Servers";
            rows.push_back(std::move(heading));
        }
        for (const auto& s : in_scope) {
            ListRow r;
            r.kind = ListRowKind::Server;
            r.server_name = s.name;
            r.status = s.status;
            r.tool_count = static_cast<int>(s.tools.size());
            r.resource_count = static_cast<int>(s.resources.size());
            r.prompt_count = static_cast<int>(s.prompts.size());
            r.last_error = s.last_error;
            r.scope = scope;
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

// ============================================================
// Interactive component
// ============================================================

/// Build a fully interactive MCP server list.  All real engine mutations are
/// routed through the ServerListProps callbacks.
[[nodiscard]] inline Component McpServerList(ServerListProps props) {
    struct State {
        ServerListProps props;
        std::vector<ListRow> rows;
        int selected = 0;
        std::string search_query;
        bool in_search_mode = false;
        Totals totals;
        std::vector<McpTool> preview_tools;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    // Initial refresh to populate rows & totals.
    // (We don't block the constructor — caller may invoke on_refresh synchronously
    // before mounting; otherwise rows stay empty until first [R] press.)

    auto find_first_server_index = [](const std::vector<ListRow>& rows, int from) -> int {
        for (int i = from; i < static_cast<int>(rows.size()); ++i) {
            if (rows[i].kind == ListRowKind::Server || rows[i].kind == ListRowKind::AgentServer) {
                return i;
            }
        }
        return -1;
    };

    auto find_last_server_index = [](const std::vector<ListRow>& rows, int from) -> int {
        for (int i = from; i >= 0; --i) {
            if (rows[i].kind == ListRowKind::Server || rows[i].kind == ListRowKind::AgentServer) {
                return i;
            }
        }
        return -1;
    };

    auto current_server_row = [state]() -> const ListRow* {
        if (state->selected < 0 || state->selected >= static_cast<int>(state->rows.size()))
            return nullptr;
        const auto& r = state->rows[state->selected];
        if (r.kind == ListRowKind::Heading) return nullptr;
        return &r;
    };

    auto recompute_totals = [state] {
        Totals t;
        t.total = 0; t.connected = 0; t.tools = 0;
        for (const auto& r : state->rows) {
            if (r.kind != ListRowKind::Server) continue;
            ++t.total;
            if (r.status == ConnectionStatus::Connected) ++t.connected;
            t.tools += r.tool_count;
        }
        state->totals = t;
    };

    auto refresh_rows = [state, recompute_totals, find_first_server_index] {
        if (!state->props.on_refresh) return;
        auto snaps = state->props.on_refresh();
        state->rows = BuildRowsFromSnapshots(
            {snaps.begin(), snaps.end()}, state->search_query);
        // Normalize selection to first server row.
        int first = find_first_server_index(state->rows, 0);
        if (first >= 0) state->selected = first;
        recompute_totals();
    };

    return Renderer([state, current_server_row] {
        return RenderServerList(
            state->rows, state->selected, state->search_query,
            current_server_row(), state->preview_tools, state->totals);
    }) | CatchEvent([state, find_first_server_index, find_last_server_index,
                      current_server_row, recompute_totals, refresh_rows](Event event) -> bool {

        // --- Search mode capture ---
        if (state->in_search_mode) {
            if (event == Event::Return || event == Event::Escape) {
                state->in_search_mode = false;
                // Re-apply filter on Enter.
                if (event == Event::Return) {
                    if (state->props.on_refresh) {
                        refresh_rows();
                    }
                }
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->search_query.empty())
                    state->search_query.pop_back();
                return true;
            }
            if (event.is_character()) {
                state->search_query += event.character();
                return true;
            }
            // Swallow arrow events while typing.
            if (event == Event::ArrowUp || event == Event::ArrowDown ||
                event == Event::ArrowLeft || event == Event::ArrowRight) return true;
        }

        // --- Enter search ---
        if (event == Event::Character('/')) {
            state->in_search_mode = true;
            return true;
        }

        // --- Navigation (j/k/arrows) ---
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            int next = find_last_server_index(state->rows, state->selected - 1);
            if (next < 0) next = find_last_server_index(state->rows, static_cast<int>(state->rows.size()) - 1);
            if (next >= 0) state->selected = next;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            int next = find_first_server_index(state->rows, state->selected + 1);
            if (next < 0) next = find_first_server_index(state->rows, 0);
            if (next >= 0) state->selected = next;
            return true;
        }

        // --- Add server (a) ---
        if (event == Event::Character('a')) {
            if (state->props.on_open_add_wizard)
                state->props.on_open_add_wizard();
            return true;
        }

        // --- Reload (R uppercase or Ctrl+r) ---
        if (event == Event::Character('R')) {
            refresh_rows();
            return true;
        }

        // --- Per-row actions (d/r/c/Enter) ---
        auto* row = current_server_row();
        if (row) {
            if (event == Event::Character('d')) {
                if (state->props.on_delete) state->props.on_delete(row->server_name);
                refresh_rows();
                return true;
            }
            if (event == Event::Character('r')) {
                if (state->props.on_restart) state->props.on_restart(row->server_name);
                refresh_rows();
                return true;
            }
            if (event == Event::Character('c')) {
                if (row->status == ConnectionStatus::Connected ||
                    row->status == ConnectionStatus::Connecting) {
                    if (state->props.on_disconnect) state->props.on_disconnect(row->server_name);
                } else {
                    if (state->props.on_connect) state->props.on_connect(row->server_name);
                }
                refresh_rows();
                return true;
            }
            if (event == Event::Return) {
                if (state->props.on_open_details)
                    state->props.on_open_details(row->server_name);
                return true;
            }
        }

        // --- Exit ---
        if (event == Event::Escape) {
            if (state->props.on_cancel) state->props.on_cancel();
            else if (state->props.on_complete)
                state->props.on_complete("MCP server list closed");
            return true;
        }

        return false;
    });
}

// Convenience factory for direct instantiation with defaults.
[[nodiscard]] inline Component MakeMcpDialog(ServerListProps props = {}) {
    return McpServerList(std::move(props));
}

} // namespace cc::ui::mcp
