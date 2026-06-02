/// @file mcp_dialogs.cppm
/// @brief MCP (Model Context Protocol) server management dialogs.
/// Migrated from components/mcp/ — settings panel, list view, server menus, elicitation.
module;

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <variant>
#include <algorithm>
#include <cstdint>
#include <map>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.mcp_dialogs;

export namespace cc::ui::mcp_dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// MCP server connection state
enum class McpConnectionState {
    connected,
    connecting,
    disconnected,
    error,
};

/// MCP server transport type
enum class McpTransportType {
    stdio,
    sse,
    http,
    claudeai_proxy,
};

/// Config scope for MCP servers
enum class McpConfigScope {
    project,
    local_,   // trailing underscore to avoid keyword clash
    user,
    enterprise,
    dynamic,
};

/// Convert scope to display heading
[[nodiscard]] inline std::string scope_heading(McpConfigScope scope) {
    switch (scope) {
        case McpConfigScope::project: return "Project MCPs";
        case McpConfigScope::local_: return "Local MCPs";
        case McpConfigScope::user: return "User MCPs";
        case McpConfigScope::enterprise: return "Enterprise MCPs";
        case McpConfigScope::dynamic: return "Built-in MCPs";
    }
    return "MCPs";
}

/// MCP tool definition
struct McpTool {
    std::string name;
    std::string description;
    std::string server_name;
    bool enabled = true;
};

/// MCP server information
struct McpServerInfo {
    std::string name;
    McpTransportType transport;
    McpConfigScope scope;
    McpConnectionState state = McpConnectionState::disconnected;
    int tool_count = 0;
    std::optional<std::string> error_message;
    bool is_authenticated = false;
    std::vector<McpTool> tools;
};

/// Agent-owned MCP server info
struct AgentMcpServerInfo {
    std::string agent_name;
    std::string server_name;
    McpConnectionState state = McpConnectionState::disconnected;
    int tool_count = 0;
};

/// MCP view navigation state
enum class McpViewType {
    list,        // Server list panel
    server,      // Single server detail
    tool_list,   // Tools of a server
    tool_detail, // Single tool detail
};

/// Command result display mode
enum class CommandResultDisplay {
    normal,
    system,
    skip,
};

/// Props for the MCP settings dialog
struct McpSettingsProps {
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_complete;
};

/// Elicitation field type
enum class ElicitFieldType {
    text,
    number,
    boolean_,
    enum_select,
    multi_select,
    date_time,
    url,
};

/// Schema for an elicitation field
struct ElicitFieldSchema {
    std::string name;
    std::string title;
    std::string description;
    ElicitFieldType type = ElicitFieldType::text;
    bool required = false;
    std::vector<std::string> enum_values;
    std::optional<std::string> default_value;
};

/// Elicitation dialog action result
enum class ElicitAction {
    submit,
    cancel,
    dismiss,
};

/// Props for elicitation dialog
struct ElicitationDialogProps {
    std::string server_name;
    std::string message;
    std::vector<ElicitFieldSchema> fields;
    std::function<void(ElicitAction, std::map<std::string, std::string>)> on_response;
    std::optional<std::string> url;
};

// ============================================================
// Element Rendering
// ============================================================

/// Render connection state indicator
[[nodiscard]] inline Element RenderConnectionState(McpConnectionState state) {
    switch (state) {
        case McpConnectionState::connected:
            return text("\u25CF") | color(Color::Green);
        case McpConnectionState::connecting:
            return text("\u25CB") | color(Color::Yellow);
        case McpConnectionState::disconnected:
            return text("\u25CB") | color(Color::GrayDark);
        case McpConnectionState::error:
            return text("\u25CF") | color(Color::Red);
    }
    return text("?");
}

/// Render a single server entry in the list
[[nodiscard]] inline Element RenderServerListItem(
    const McpServerInfo& server, bool selected) {

    auto state_el = RenderConnectionState(server.state);
    auto name_el = text(" " + server.name)
                   | (selected ? bold : nothing);
    auto tools_el = text(std::format(" ({} tools)", server.tool_count)) | dim;

    auto line = hbox({text(" "), state_el, name_el, tools_el, filler()});
    if (selected) line = line | bgcolor(Color::RGB(30, 40, 55));
    return line;
}

/// Render the server list panel organized by scope
[[nodiscard]] inline Element RenderServerListPanel(
    const std::vector<McpServerInfo>& servers,
    const std::vector<AgentMcpServerInfo>& agent_servers,
    int selected) {

    Elements items;
    int idx = 0;

    // Group servers by scope
    const std::vector<McpConfigScope> scope_order = {
        McpConfigScope::project, McpConfigScope::local_,
        McpConfigScope::user, McpConfigScope::enterprise,
    };

    for (auto scope : scope_order) {
        bool has_any = false;
        for (const auto& s : servers) {
            if (s.scope == scope) {
                if (!has_any) {
                    items.push_back(text(" " + scope_heading(scope)) | bold | dim);
                    has_any = true;
                }
                items.push_back(RenderServerListItem(s, idx == selected));
                ++idx;
            }
        }
        if (has_any) items.push_back(text(""));
    }

    // Agent servers section
    if (!agent_servers.empty()) {
        items.push_back(text(" Agent MCPs") | bold | dim);
        for (const auto& as : agent_servers) {
            auto state_el = RenderConnectionState(as.state);
            auto line = hbox({
                text(" "), state_el,
                text(" " + as.server_name) | (idx == selected ? bold : nothing),
                text(" (via " + as.agent_name + ")") | dim,
                filler(),
            });
            if (idx == selected) line = line | bgcolor(Color::RGB(30, 40, 55));
            items.push_back(line);
            ++idx;
        }
    }

    if (items.empty()) {
        items.push_back(text("  No MCP servers configured") | dim);
    }

    return vbox(items);
}

/// Render the tool list for a server
[[nodiscard]] inline Element RenderToolListView(
    const McpServerInfo& server, int selected_tool) {

    Elements items;
    items.push_back(hbox({
        text(" Tools for ") | dim,
        text(server.name) | bold,
        text(std::format(" ({})", server.tools.size())) | dim,
    }));
    items.push_back(separator());

    for (int i = 0; i < static_cast<int>(server.tools.size()); ++i) {
        const auto& tool = server.tools[i];
        bool sel = (i == selected_tool);
        auto enabled_el = tool.enabled
            ? text(" \u2713 ") | color(Color::Green)
            : text(" \u2717 ") | color(Color::Red);
        auto name_el = text(tool.name) | (sel ? bold : nothing);
        auto line = hbox({enabled_el, name_el, filler()});
        if (sel) line = line | bgcolor(Color::RGB(30, 40, 55));
        items.push_back(line);
    }

    return vbox(items);
}

/// Render tool detail view
[[nodiscard]] inline Element RenderToolDetailView(const McpTool& tool) {
    return vbox({
        hbox({
            text(" \u2699 ") | color(Color::Cyan),
            text(tool.name) | bold,
        }),
        separator(),
        text(""),
        hbox({text("  Server: "), text(tool.server_name) | dim}),
        hbox({text("  Status: "),
              text(tool.enabled ? "Enabled" : "Disabled")
              | color(tool.enabled ? Color::Green : Color::Red)}),
        text(""),
        text("  Description:") | bold,
        paragraph("  " + tool.description) | dim,
    });
}

/// Render the MCP settings dialog based on view state
[[nodiscard]] inline Element RenderMcpSettings(
    McpViewType view,
    const std::vector<McpServerInfo>& servers,
    const std::vector<AgentMcpServerInfo>& agent_servers,
    int selected,
    std::optional<int> active_server_idx) {

    Element content;
    switch (view) {
        case McpViewType::list:
            content = RenderServerListPanel(servers, agent_servers, selected);
            break;
        case McpViewType::tool_list:
            if (active_server_idx && *active_server_idx < static_cast<int>(servers.size())) {
                content = RenderToolListView(servers[*active_server_idx], selected);
            } else {
                content = text("  No server selected") | dim;
            }
            break;
        case McpViewType::tool_detail:
            if (active_server_idx && *active_server_idx < static_cast<int>(servers.size())) {
                const auto& srv = servers[*active_server_idx];
                if (selected < static_cast<int>(srv.tools.size())) {
                    content = RenderToolDetailView(srv.tools[selected]);
                } else {
                    content = text("  Tool not found") | dim;
                }
            } else {
                content = text("  No server selected") | dim;
            }
            break;
        case McpViewType::server:
            content = text("  Server detail view") | dim;
            break;
    }

    // Action hints
    auto hints = hbox({
        text(" Enter") | color(Color::Cyan), text(": select  "),
        text("Esc") | color(Color::Cyan), text(": back  "),
        text("r") | color(Color::Cyan), text(": reconnect"),
    }) | dim;

    auto body = vbox({
        content | flex,
        separator(),
        hints,
    });

    return window(
        text(" MCP Servers ") | bold | color(Color::Blue),
        body
    ) | color(Color::Blue);
}

/// Render the elicitation form dialog
[[nodiscard]] inline Element RenderElicitationDialog(
    const ElicitationDialogProps& props,
    const std::map<std::string, std::string>& field_values,
    int focused_field) {

    Elements items;
    if (!props.message.empty()) {
        items.push_back(paragraph(props.message));
        items.push_back(text(""));
    }

    for (int i = 0; i < static_cast<int>(props.fields.size()); ++i) {
        const auto& field = props.fields[i];
        bool focused = (i == focused_field);
        auto label = text("  " + field.title + ": ")
                     | (field.required ? bold : nothing);
        auto it = field_values.find(field.name);
        std::string val = (it != field_values.end()) ? it->second : "";
        auto val_el = text(val.empty() ? "(empty)" : val)
                      | (val.empty() ? dim : nothing);
        if (focused) val_el = val_el | inverted;

        items.push_back(hbox({label, val_el}));
        if (!field.description.empty()) {
            items.push_back(text("    " + field.description) | dim);
        }
    }

    items.push_back(text(""));
    items.push_back(hbox({
        text("  Enter") | color(Color::Cyan), text(": submit  "),
        text("Esc") | color(Color::Cyan), text(": cancel"),
    }) | dim);

    auto body = vbox(items);
    auto title_str = " " + props.server_name + " — Input Required ";

    return window(
        text(title_str) | bold | color(Color::Yellow),
        body
    ) | color(Color::Yellow) | size(WIDTH, LESS_THAN, 70);
}

// ============================================================
// Interactive Components
// ============================================================

/// Create the MCP settings panel component
[[nodiscard]] inline Component McpSettingsComponent(McpSettingsProps props) {
    struct State {
        McpSettingsProps props;
        McpViewType view = McpViewType::list;
        std::vector<McpServerInfo> servers;
        std::vector<AgentMcpServerInfo> agent_servers;
        int selected = 0;
        std::optional<int> active_server_idx;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderMcpSettings(
            state->view, state->servers, state->agent_servers,
            state->selected, state->active_server_idx);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected++;
            return true;
        }
        if (event == Event::Return) {
            if (state->view == McpViewType::list) {
                state->active_server_idx = state->selected;
                state->view = McpViewType::tool_list;
                state->selected = 0;
            } else if (state->view == McpViewType::tool_list) {
                state->view = McpViewType::tool_detail;
            }
            return true;
        }
        if (event == Event::Escape) {
            if (state->view == McpViewType::tool_detail) {
                state->view = McpViewType::tool_list;
            } else if (state->view == McpViewType::tool_list) {
                state->view = McpViewType::list;
                state->selected = state->active_server_idx.value_or(0);
                state->active_server_idx = std::nullopt;
            } else {
                if (state->props.on_complete) {
                    state->props.on_complete(std::nullopt, CommandResultDisplay::skip);
                }
            }
            return true;
        }
        // Reconnect
        if (event == Event::Character('r')) {
            // Trigger reconnect for selected server (runtime hook)
            return true;
        }
        return false;
    });
}

/// Create the elicitation dialog component
[[nodiscard]] inline Component ElicitationDialogComponent(
    ElicitationDialogProps props) {

    struct State {
        ElicitationDialogProps props;
        std::map<std::string, std::string> field_values;
        int focused_field = 0;
    };

    auto state = std::make_shared<State>();
    // Initialize default values
    for (const auto& field : props.fields) {
        if (field.default_value) {
            state->field_values[field.name] = *field.default_value;
        }
    }
    state->props = std::move(props);

    return Renderer([state] {
        return RenderElicitationDialog(
            state->props, state->field_values, state->focused_field);
    }) | CatchEvent([state](Event event) -> bool {
        int field_count = static_cast<int>(state->props.fields.size());

        if (event == Event::ArrowUp) {
            state->focused_field = std::max(0, state->focused_field - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Tab) {
            state->focused_field = std::min(field_count - 1, state->focused_field + 1);
            return true;
        }
        if (event == Event::Return) {
            if (state->props.on_response) {
                state->props.on_response(ElicitAction::submit, state->field_values);
            }
            return true;
        }
        if (event == Event::Escape) {
            if (state->props.on_response) {
                state->props.on_response(ElicitAction::cancel, {});
            }
            return true;
        }
        // Character input for focused text field
        if (event.is_character() && state->focused_field < field_count) {
            auto& field = state->props.fields[state->focused_field];
            if (field.type == ElicitFieldType::text ||
                field.type == ElicitFieldType::number ||
                field.type == ElicitFieldType::url) {
                state->field_values[field.name] += event.character();
                return true;
            }
        }
        return false;
    });
}

} // namespace cc::ui::mcp_dialogs
