/// @file mcp_dialog.cppm
/// @brief MCP (Model Context Protocol) server management interface.
/// Displays connected servers, their tools, and allows configuration.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.mcp_dialog;

import cc.types.types;

export namespace cc::ui::dialogs::mcp_dialog {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Connection status of an MCP server
enum class McpServerStatus : std::uint8_t {
    Connected,      // Active and responsive
    Connecting,     // Currently establishing connection
    Disconnected,   // Not connected
    Error,          // Connection error
    Stale,          // Connected but not responsive
};

/// A tool provided by an MCP server
struct McpTool {
    std::string name;
    std::string description;
    bool enabled = true;
    int call_count = 0;
};

/// A resource provided by an MCP server
struct McpResource {
    std::string uri;
    std::string name;
    std::string mime_type;
};

/// Data for a single MCP server entry
struct McpServerEntry {
    std::string name;
    std::string command;            // Launch command
    std::string transport;          // "stdio", "sse", "streamable-http"
    McpServerStatus status;
    std::vector<McpTool> tools;
    std::vector<McpResource> resources;
    std::optional<std::string> error_message;
    std::chrono::steady_clock::time_point last_heartbeat;
    std::string version;
};

/// Options for the MCP dialog
struct McpDialogOptions {
    std::vector<McpServerEntry> servers;
    int selected_server = 0;
    int selected_tool = -1;
    bool show_details = false;

    std::function<void(const std::string& server_name)> on_connect;
    std::function<void(const std::string& server_name)> on_disconnect;
    std::function<void(const std::string& server_name)> on_restart;
    std::function<void(const std::string& server_name, const std::string& tool_name, bool enabled)> on_toggle_tool;
    std::function<void()> on_add_server;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get status icon and color
[[nodiscard]] inline std::pair<std::string, Color> status_display(McpServerStatus s) {
    switch (s) {
        case McpServerStatus::Connected:    return {"●", Color::Green};
        case McpServerStatus::Connecting:   return {"◐", Color::Yellow};
        case McpServerStatus::Disconnected: return {"○", Color::GrayDark};
        case McpServerStatus::Error:        return {"✗", Color::Red};
        case McpServerStatus::Stale:        return {"◌", Color::Yellow};
    }
    return {"?", Color::White};
}

/// Get transport display label
[[nodiscard]] inline std::string transport_label(const std::string& transport) {
    if (transport == "stdio") return "stdio";
    if (transport == "sse") return "SSE";
    if (transport == "streamable-http") return "HTTP";
    return transport;
}

// ============================================================
// Element Rendering
// ============================================================

/// Render the server list panel
[[nodiscard]] inline Element RenderServerList(
    const std::vector<McpServerEntry>& servers, int selected) {

    Elements elements;
    elements.push_back(text(" MCP Servers") | bold | color(Color::Cyan));
    elements.push_back(separator());

    if (servers.empty()) {
        elements.push_back(text(" No servers configured") | dim);
        elements.push_back(text(" Press [a] to add a server") | dim | color(Color::GrayLight));
        return vbox(elements) | border;
    }

    for (int i = 0; i < static_cast<int>(servers.size()); ++i) {
        const auto& s = servers[i];
        auto [icon, icon_color] = status_display(s.status);

        auto line = hbox({
            text(" " + icon + " ") | color(icon_color),
            text(s.name) | (i == selected ? bold : nothing),
            filler(),
            text(std::format("{} tools", s.tools.size())) | dim,
            text(" "),
            text("[" + transport_label(s.transport) + "]") | dim | color(Color::GrayDark),
            text(" "),
        });

        if (i == selected) {
            line = line | inverted;
        }
        elements.push_back(line);
    }

    return vbox(elements) | border;
}

/// Render the server detail panel
[[nodiscard]] inline Element RenderServerDetail(
    const McpServerEntry& server, int selected_tool) {

    Elements elements;

    // Server info header
    auto [status_icon, status_color] = status_display(server.status);
    elements.push_back(hbox({
        text(" " + server.name + " ") | bold | color(Color::White),
        text(status_icon + " ") | color(status_color),
    }));
    elements.push_back(separator());

    // Connection info
    elements.push_back(hbox({
        text("  Command:   ") | dim,
        text(server.command) | color(Color::Cyan),
    }));
    elements.push_back(hbox({
        text("  Transport: ") | dim,
        text(transport_label(server.transport)) | color(Color::Yellow),
    }));
    if (!server.version.empty()) {
        elements.push_back(hbox({
            text("  Version:   ") | dim,
            text(server.version) | color(Color::GrayLight),
        }));
    }

    // Error message
    if (server.error_message) {
        elements.push_back(text(""));
        elements.push_back(hbox({
            text("  ✗ ") | color(Color::Red),
            text(*server.error_message) | color(Color::Red) | dim,
        }));
    }

    elements.push_back(text(""));
    elements.push_back(separator());

    // Tools section
    elements.push_back(text(std::format("  Tools ({})", server.tools.size())) | bold);

    for (int i = 0; i < static_cast<int>(server.tools.size()); ++i) {
        const auto& tool = server.tools[i];
        auto enabled_icon = tool.enabled ? "✓" : "○";
        auto enabled_color = tool.enabled ? Color::Green : Color::GrayDark;

        auto line = hbox({
            text("    " + std::string(enabled_icon) + " ") | color(enabled_color),
            text(tool.name) | (i == selected_tool ? bold : nothing)
                | color(tool.enabled ? Color::White : Color::GrayDark),
            filler(),
            text(std::format("({}×)", tool.call_count)) | dim,
            text(" "),
        });
        if (i == selected_tool) {
            line = line | bgcolor(Color::RGB(40, 40, 60));
        }
        elements.push_back(line);
    }

    // Resources section
    if (!server.resources.empty()) {
        elements.push_back(text(""));
        elements.push_back(text(std::format("  Resources ({})", server.resources.size())) | bold);
        for (const auto& res : server.resources) {
            elements.push_back(hbox({
                text("    📎 ") | dim,
                text(res.name) | color(Color::Cyan),
                text(" (" + res.mime_type + ")") | dim,
            }));
        }
    }

    return vbox(elements) | border;
}

/// Render the full MCP dialog
[[nodiscard]] inline Element RenderMcpDialog(const McpDialogOptions& opts) {
    auto list_panel = RenderServerList(opts.servers, opts.selected_server)
                      | size(WIDTH, EQUAL, 35);

    Element detail_panel;
    if (!opts.servers.empty() && opts.selected_server < static_cast<int>(opts.servers.size())) {
        detail_panel = RenderServerDetail(
            opts.servers[opts.selected_server], opts.selected_tool);
    } else {
        detail_panel = text(" Select a server") | dim | border;
    }

    // Action bar at bottom
    auto actions = hbox({
        text(" [a]") | color(Color::Cyan), text("dd "),
        text("[r]") | color(Color::Cyan), text("estart "),
        text("[d]") | color(Color::Cyan), text("isconnect "),
        text("[t]") | color(Color::Cyan), text("oggle "),
        text("[Esc]") | color(Color::Cyan), text("close "),
    }) | dim;

    return vbox({
        hbox({
            text(" 🔌 MCP Server Management ") | bold | color(Color::Cyan),
            filler(),
        }),
        separator(),
        hbox({list_panel, detail_panel | flex}) | flex,
        separator(),
        actions,
    }) | borderDouble | bgcolor(Color::RGB(15, 15, 25));
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the MCP dialog component
[[nodiscard]] inline Component McpDialog(McpDialogOptions options) {
    struct State {
        McpDialogOptions opts;
        enum class Focus { ServerList, ToolList } focus = Focus::ServerList;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        return RenderMcpDialog(state->opts);
    }) | CatchEvent([state](Event event) -> bool {
        int server_count = static_cast<int>(state->opts.servers.size());

        if (event == Event::Escape) {
            if (state->opts.on_close) state->opts.on_close();
            return true;
        }

        if (state->focus == State::Focus::ServerList) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->opts.selected_server = std::max(0, state->opts.selected_server - 1);
                state->opts.selected_tool = -1;
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->opts.selected_server = std::min(server_count - 1, state->opts.selected_server + 1);
                state->opts.selected_tool = -1;
                return true;
            }
            if (event == Event::ArrowRight || event == Event::Tab || event == Event::Return) {
                if (server_count > 0) {
                    state->focus = State::Focus::ToolList;
                    state->opts.selected_tool = 0;
                }
                return true;
            }
        } else {
            // Tool list focus
            if (server_count > 0) {
                const auto& server = state->opts.servers[state->opts.selected_server];
                int tool_count = static_cast<int>(server.tools.size());

                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    state->opts.selected_tool = std::max(0, state->opts.selected_tool - 1);
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    state->opts.selected_tool = std::min(tool_count - 1, state->opts.selected_tool + 1);
                    return true;
                }
                if (event == Event::ArrowLeft || event == Event::TabReverse) {
                    state->focus = State::Focus::ServerList;
                    state->opts.selected_tool = -1;
                    return true;
                }
                if (event == Event::Character('t') || event == Event::Return) {
                    if (state->opts.on_toggle_tool && state->opts.selected_tool >= 0 &&
                        state->opts.selected_tool < tool_count) {
                        const auto& tool = server.tools[state->opts.selected_tool];
                        state->opts.on_toggle_tool(server.name, tool.name, !tool.enabled);
                    }
                    return true;
                }
            }
            if (event == Event::Escape) {
                state->focus = State::Focus::ServerList;
                state->opts.selected_tool = -1;
                return true;
            }
        }

        // Global shortcuts
        if (event == Event::Character('a')) {
            if (state->opts.on_add_server) state->opts.on_add_server();
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->opts.on_restart && server_count > 0) {
                state->opts.on_restart(state->opts.servers[state->opts.selected_server].name);
            }
            return true;
        }
        if (event == Event::Character('d')) {
            if (state->opts.on_disconnect && server_count > 0) {
                state->opts.on_disconnect(state->opts.servers[state->opts.selected_server].name);
            }
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::dialogs::mcp_dialog
