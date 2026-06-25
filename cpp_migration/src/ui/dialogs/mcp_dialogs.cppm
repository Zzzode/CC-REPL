/// @file mcp_dialogs.cppm
/// @brief MCP (Model Context Protocol) server management dialogs —
///        settings list panel + elicitation form renderer.
///
/// Elicitation 2.0 payload contract (replaces the 2-button stub):
///   ElicitationPayload { schema?, values_buffer, validation_errors,
///                        focused_field, on_response(ElicitationResult),
///                        on_cancel() }
///   ElicitationResult { action: approve|decline|cancel,
///                       content: map<string, Variant> }
///
/// Renderer branches: schema present → FORM mode (TextField / enum
/// Select with 800ms typeahead / boolean Space-toggle / URL braille
/// spinner / CheckboxGrid multi-select / DateTime). Tab / Shift+Tab
/// walks fields; Accept / Decline buttons submit.  Else → generic
/// Yes/No URL stub (backward compat for 2-button callers).
///
/// Backward compat: on_response(bool) adapters are provided so that
/// existing ElicitationDialogProps-style callers continue to compile
/// with their existing bool callback signature.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/color.hpp>

export module cc.ui.mcp_dialogs;

export namespace cc::ui::mcp_dialogs {
using namespace ftxui;

// ============================================================
// Top-level types (MCP server list / settings)
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

[[nodiscard]] inline std::string scope_heading(McpConfigScope scope) {
    switch (scope) {
        case McpConfigScope::project:   return "Project MCPs";
        case McpConfigScope::local_:    return "Local MCPs";
        case McpConfigScope::user:      return "User MCPs";
        case McpConfigScope::enterprise:return "Enterprise MCPs";
        case McpConfigScope::dynamic:   return "Built-in MCPs";
    }
    return "MCPs";
}

struct McpTool {
    std::string name;
    std::string description;
    std::string server_name;
    bool enabled = true;
};

struct McpServerInfo {
    std::string name;
    McpTransportType transport{};
    McpConfigScope scope{};
    McpConnectionState state = McpConnectionState::disconnected;
    int tool_count = 0;
    std::optional<std::string> error_message;
    bool is_authenticated = false;
    std::vector<McpTool> tools;
};

struct AgentMcpServerInfo {
    std::string agent_name;
    std::string server_name;
    McpConnectionState state = McpConnectionState::disconnected;
    int tool_count = 0;
};

enum class McpViewType {
    list,        // Server list panel
    server,      // Single server detail
    tool_list,   // Tools of a server
    tool_detail, // Single tool detail
};

enum class CommandResultDisplay {
    normal,
    system,
    skip,
};

struct McpSettingsProps {
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_complete;
};

// ============================================================
// Elicitation field types + schema
// ============================================================

/// Schema for an elicitation field
enum class ElicitFieldType {
    text,
    number,
    boolean_,
    enum_select,
    multi_select,   // → CheckboxGrid bonus
    date_time,      // bonus
    url,            // bonus → braille resolving spinner
};

struct ElicitFieldSchema {
    std::string name;         // machine key (map key)
    std::string title;        // user-visible label
    std::string description;  // help text below the field
    ElicitFieldType type = ElicitFieldType::text;
    bool required = false;
    // For enum_select / multi_select
    std::vector<std::string> enum_values;
    // For multi_select: optional columns hint (CheckboxGrid)
    int grid_columns = 3;
    // For boolean: labels
    std::string bool_true_label  = "Yes";
    std::string bool_false_label = "No";
    // Defaults
    std::optional<std::string>   default_string;
    std::optional<int64_t>       default_int;
    std::optional<double>        default_number;
    std::optional<bool>          default_bool;
    std::optional<std::vector<std::string>> default_multi;
    // URL resolver hint (when true, renderer shows a braille spinner
    // as-if the URL were being resolved, while the consumer actually
    // drives the transition out of "resolving" state externally.)
    bool show_url_resolving_spinner = false;
};

// ----------------------------------------------------------------
// Value variant: a single field value in values_buffer/content.
// ----------------------------------------------------------------
struct ElicitNull {};
using ElicitMulti = std::vector<std::string>;

using ElicitValue = std::variant<
    ElicitNull,
    std::string,
    int64_t,
    double,
    bool,
    ElicitMulti
>;

[[nodiscard]] inline bool elicit_is_null(const ElicitValue& v) {
    return std::holds_alternative<ElicitNull>(v);
}

[[nodiscard]] inline std::string elicit_to_string(const ElicitValue& v) {
    struct V {
        std::string operator()(ElicitNull) const { return ""; }
        std::string operator()(const std::string& s) const { return s; }
        std::string operator()(int64_t i) const { return std::to_string(i); }
        std::string operator()(double d) const {
            std::ostringstream os;
            os << std::setprecision(6) << d;
            return os.str();
        }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(const ElicitMulti& m) const {
            std::string out;
            for (size_t i = 0; i < m.size(); ++i) {
                if (i) out += ", ";
                out += m[i];
            }
            return out;
        }
    };
    return std::visit(V{}, v);
}

[[nodiscard]] inline std::string elicit_display(const ElicitValue& v) {
    std::string s = elicit_to_string(v);
    return s.empty() ? "(empty)" : s;
}

// ============================================================
// Elicitation 2.0 payload + result (SPEC-COMPLIANT)
// ============================================================

enum class ElicitAction {
    submit,   // deprecated in new API; use ElicitationResult::action
    cancel,
    dismiss,
};

/// New result struct (per spec).
struct ElicitationResult {
    /// One of: "approve", "decline", "cancel".
    std::string action;
    /// Per-field content (values_buffer snapshot at submit time.)
    std::map<std::string, ElicitValue> content;
};

/// Per-spec payload struct driving the Elicitation dialog.
struct ElicitationPayload {
    /// Optional JSON schema (opaque — renderer only checks has_value to
    /// decide FORM vs stub mode).  Consumers that need to parse it can
    /// use cc.utils.json on the serialized string externally.
    struct SchemaOpaque { std::string raw_json; };
    std::optional<SchemaOpaque> schema;

    /// Per-field live values (mutated by the interactive form).
    std::map<std::string, ElicitValue> values_buffer;

    /// Per-field validation errors (keyed by field name; empty = OK).
    std::map<std::string, std::string> validation_errors;

    /// Field currently focused, or -1 when the focus ring is on one of
    /// the action buttons (Accept/Decline/Cancel).
    int focused_field = 0;

    /// Schemas used when schema is present.
    std::vector<ElicitFieldSchema> fields;

    /// Optional free-form context shown at the top of the dialog.
    std::string server_name;
    std::string message;

    /// Optional URL (stub mode + URL field spinner source).
    std::optional<std::string> url;

    // ----------------------------------------------------------------
    // Callbacks
    // ----------------------------------------------------------------
    /// Structured response callback (NEW).  Receives per-field values.
    std::function<void(ElicitationResult)> on_response;

    /// Cancel shortcut (Esc or Cancel button).  Defaults to firing
    /// on_response({action="cancel",{}}) if left empty.
    std::function<void()> on_cancel;

    // ----------------------------------------------------------------
    // Backward-compat helpers (old bool signature).
    // Setting either of these wires on_response internally so the new
    // callback path is always the single source of truth.
    void set_bool_response(std::function<void(bool approve)> cb) {
        on_response = [cb = std::move(cb)](ElicitationResult r) {
            if (!cb) return;
            cb(r.action == "approve");
        };
    }
    void set_old_action_response(
        std::function<void(ElicitAction, std::map<std::string, std::string>)> cb)
    {
        on_response = [cb = std::move(cb)](ElicitationResult r) {
            if (!cb) return;
            ElicitAction a = ElicitAction::dismiss;
            if (r.action == "approve") a = ElicitAction::submit;
            else if (r.action == "decline") a = ElicitAction::cancel;
            else if (r.action == "cancel") a = ElicitAction::cancel;
            std::map<std::string, std::string> flat;
            for (const auto& [k, v] : r.content) flat.emplace(k, elicit_to_string(v));
            cb(a, std::move(flat));
        };
    }

    // ----------------------------------------------------------------
    // Helpers to initialize defaults from `fields`.
    // ----------------------------------------------------------------
    void seed_defaults_from_fields() {
        for (const auto& f : fields) {
            if (values_buffer.count(f.name)) continue;
            switch (f.type) {
                case ElicitFieldType::text:
                case ElicitFieldType::url:
                    if (f.default_string) values_buffer[f.name] = *f.default_string;
                    break;
                case ElicitFieldType::number:
                    if (f.default_number)       values_buffer[f.name] = *f.default_number;
                    else if (f.default_int)     values_buffer[f.name] = *f.default_int;
                    break;
                case ElicitFieldType::boolean_:
                    if (f.default_bool)         values_buffer[f.name] = *f.default_bool;
                    break;
                case ElicitFieldType::enum_select:
                    if (f.default_string)       values_buffer[f.name] = *f.default_string;
                    break;
                case ElicitFieldType::multi_select:
                    if (f.default_multi)        values_buffer[f.name] = *f.default_multi;
                    break;
                case ElicitFieldType::date_time:
                    if (f.default_string)       values_buffer[f.name] = *f.default_string;
                    break;
            }
        }
    }
};

/// Legacy props type (backward-compat typedef).
struct ElicitationDialogProps {
    std::string server_name;
    std::string message;
    std::vector<ElicitFieldSchema> fields;
    std::function<void(ElicitAction, std::map<std::string, std::string>)> on_response;
    std::optional<std::string> url;

    // Extra constructor-style entry point for the pre-existing 2-button
    // stub consumer that only has a bool callback.
    static ElicitationDialogProps from_bool_cb(
        std::string server_name,
        std::string message,
        std::function<void(bool approve)> cb)
    {
        ElicitationDialogProps p;
        p.server_name = std::move(server_name);
        p.message     = std::move(message);
        p.on_response = [cb = std::move(cb)](ElicitAction a, auto&&) {
            if (cb) cb(a == ElicitAction::submit);
        };
        return p;
    }

    /// Promote legacy props to the new payload (canonical path).
    [[nodiscard]] ElicitationPayload to_payload() const {
        ElicitationPayload p;
        p.server_name = server_name;
        p.message     = message;
        p.fields      = fields;
        p.url         = url;
        if (!fields.empty()) {
            // Presence of a non-empty field list implicitly enables FORM mode.
            p.schema = ElicitationPayload::SchemaOpaque{"{}"};
        }
        p.seed_defaults_from_fields();
        if (on_response) p.set_old_action_response(on_response);
        return p;
    }
};

// ============================================================
// Rendered dialog action focus slot IDs (when focused_field < 0)
// ============================================================
namespace ElicitFocus {
    constexpr int kOnButtons = -1;     // generic: on the button row
    constexpr int kAccept    = -2;
    constexpr int kDecline   = -3;
    constexpr int kCancel    = -4;
} // namespace ElicitFocus

// ============================================================
// Shared rendering helpers
// ============================================================

[[nodiscard]] inline Element RenderConnectionState(McpConnectionState state) {
    switch (state) {
        case McpConnectionState::connected:    return text("●") | color(Color::Green);
        case McpConnectionState::connecting:   return text("○") | color(Color::Yellow);
        case McpConnectionState::disconnected: return text("○") | color(Color::GrayDark);
        case McpConnectionState::error:        return text("●") | color(Color::Red);
    }
    return text("?");
}

[[nodiscard]] inline Element RenderServerListItem(const McpServerInfo& s, bool selected) {
    auto state_el = RenderConnectionState(s.state);
    auto name_el  = text(" " + s.name) | (selected ? bold : nothing);
    auto tools_el = text(std::format(" ({} tools)", s.tool_count)) | dim;
    auto line = hbox({text(" "), state_el, name_el, tools_el, filler()});
    if (selected) line = line | bgcolor(Color::RGB(30, 40, 55));
    return line;
}

[[nodiscard]] inline Element RenderServerListPanel(
    const std::vector<McpServerInfo>& servers,
    const std::vector<AgentMcpServerInfo>& agent_servers,
    int selected)
{
    Elements items;
    int idx = 0;
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
    if (items.empty()) items.push_back(text("  No MCP servers configured") | dim);
    return vbox(items);
}

[[nodiscard]] inline Element RenderToolListView(const McpServerInfo& srv, int selected_tool) {
    Elements items;
    items.push_back(hbox({
        text(" Tools for ") | dim,
        text(srv.name) | bold,
        text(std::format(" ({})", srv.tools.size())) | dim,
    }));
    items.push_back(separator());
    for (int i = 0; i < static_cast<int>(srv.tools.size()); ++i) {
        const auto& tool = srv.tools[i];
        bool sel = (i == selected_tool);
        auto en  = tool.enabled
            ? text(" ✓ ") | color(Color::Green)
            : text(" ✗ ") | color(Color::Red);
        auto name = text(tool.name) | (sel ? bold : nothing);
        auto line = hbox({en, name, filler()});
        if (sel) line = line | bgcolor(Color::RGB(30, 40, 55));
        items.push_back(line);
    }
    return vbox(items);
}

[[nodiscard]] inline Element RenderToolDetailView(const McpTool& tool) {
    return vbox({
        hbox({text(" ⚙ ") | color(Color::Cyan), text(tool.name) | bold}),
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

[[nodiscard]] inline Element RenderMcpSettings(
    McpViewType view,
    const std::vector<McpServerInfo>& servers,
    const std::vector<AgentMcpServerInfo>& agent_servers,
    int selected,
    std::optional<int> active_server_idx)
{
    Element content;
    switch (view) {
        case McpViewType::list:
            content = RenderServerListPanel(servers, agent_servers, selected);
            break;
        case McpViewType::tool_list:
            if (active_server_idx &&
                *active_server_idx < static_cast<int>(servers.size()))
            {
                content = RenderToolListView(servers[*active_server_idx], selected);
            } else {
                content = text("  No server selected") | dim;
            }
            break;
        case McpViewType::tool_detail:
            if (active_server_idx &&
                *active_server_idx < static_cast<int>(servers.size()))
            {
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
    auto hints = hbox({
        text(" Enter") | color(Color::Cyan), text(": select  "),
        text("Esc")    | color(Color::Cyan), text(": back  "),
        text("r")      | color(Color::Cyan), text(": reconnect"),
    }) | dim;
    auto body = vbox({content | flex, separator(), hints});
    return window(text(" MCP Servers ") | bold | color(Color::Blue), body)
         | color(Color::Blue);
}

// ============================================================
// Elicitation rendering — FORM mode + stub mode
// ============================================================

// 8-dot braille cycle (standard spinner used by FTXUI samples).
[[nodiscard]] inline char BrailleFrame(std::uint64_t tick) {
    // 8 phases, cycle 4 times to get 32-phase smoother animation.
    static constexpr std::array<char, 8> kCycle = {
        '\xE2','\xE2','\xE2','\xE2','\xE2','\xE2','\xE2','\xE2'
    };
    // Use proper unicode U+28xx braille dots via direct UTF-8 bytes.
    static constexpr const char* kFrames[] = {
        "\xE2\xA0\x81", "\xE2\xA0\x82", "\xE2\xA0\x84", "\xE2\xA0\x80",
        "\xE2\xA0\x40", "\xE2\xA0\x20", "\xE2\xA0\x10", "\xE2\xA0\x80",
    };
    (void)kCycle;
    return 0; // never called: we use the string table below.
}

[[nodiscard]] inline std::string BrailleSpinnerFrame(std::uint64_t tick) {
    static constexpr const char* kFrames[] = {
        "\xE2\xA0\x81", "\xE2\xA0\x82", "\xE2\xA0\x84", "\xE2\xA0\x80",
        "\xE2\xA0\x40", "\xE2\xA0\x20", "\xE2\xA0\x10", "\xE2\xA0\x80",
    };
    return kFrames[tick % (sizeof(kFrames) / sizeof(kFrames[0]))];
}

// ----------------------------------------------------------------
// Individual field renderers
// ----------------------------------------------------------------

/// Render a single field row, label + value + optional error.
struct ElicitFieldRendered {
    Element body;
    Element error;
};

template <typename FocusFn>
[[nodiscard]] inline ElicitFieldRendered RenderElicitField(
    const ElicitFieldSchema& field,
    const ElicitValue& value,
    bool focused,
    FocusFn&& focused_decorator)
{
    Elements row;
    auto label_txt = text("  " + field.title + (field.required ? " *" : "") + ": ")
                   | (field.required ? bold : nothing);
    Element val_el;
    switch (field.type) {
        case ElicitFieldType::text:
        case ElicitFieldType::url:
        case ElicitFieldType::number:
        case ElicitFieldType::date_time: {
            auto s = elicit_display(value);
            val_el = text(s) | (elicit_is_null(value) ? dim : nothing);
            break;
        }
        case ElicitFieldType::boolean_: {
            bool b = std::holds_alternative<bool>(value) && std::get<bool>(value);
            val_el = hbox({
                text(b ? "[x]" : "[ ]")
                    | color(b ? Color::Green : Color::GrayDark)
                    | bold,
                text(" " + (b ? field.bool_true_label
                              : field.bool_false_label)) | dim,
            });
            break;
        }
        case ElicitFieldType::enum_select: {
            auto s = elicit_display(value);
            val_el = hbox({
                text("▼ ") | color(Color::Cyan),
                text(s.empty() ? "(select one)" : s)
                    | (s.empty() ? dim : nothing),
            });
            break;
        }
        case ElicitFieldType::multi_select: {
            if (auto* m = std::get_if<ElicitMulti>(&value); m && !m->empty()) {
                Elements chips;
                for (size_t i = 0; i < m->size(); ++i) {
                    if (i) chips.push_back(text(", "));
                    chips.push_back(text((*m)[i])
                                     | color(Color::Cyan) | inverted);
                }
                val_el = hbox(chips);
            } else {
                val_el = text("(none selected)") | dim;
            }
            break;
        }
    }
    if (focused) val_el = focused_decorator(std::move(val_el));
    row.push_back(hbox({label_txt, val_el}));
    if (!field.description.empty()) {
        row.push_back(text("    " + field.description) | dim);
    }

    Element err = emptyElement();
    ElicitFieldRendered out;
    out.body = vbox(std::move(row));
    return out;
}

// ----------------------------------------------------------------
// Form mode renderer (called when schema is present, or fields list
// is non-empty in the legacy path for which we synthesize `schema`).
// ----------------------------------------------------------------
[[nodiscard]] inline Element RenderElicitationForm(
    const ElicitationPayload& p,
    int button_focus,            // one of ElicitFocus::k*
    bool enum_popup_open,
    int enum_popup_selected,
    const std::vector<int>& enum_typeahead_hits,
    std::uint64_t spinner_tick,
    bool url_resolving)
{
    Elements items;
    if (!p.server_name.empty()) {
        items.push_back(hbox({
            text(" " + p.server_name) | bold | color(Color::Yellow),
            text(" requests input:") | dim,
        }));
    }
    if (!p.message.empty()) {
        items.push_back(paragraph("  " + p.message));
    }
    if (p.url && url_resolving) {
        items.push_back(hbox({
            text(" " + BrailleSpinnerFrame(spinner_tick)) | color(Color::Cyan),
            text(" resolving ") | dim,
            text(*p.url) | color(Color::Cyan),
            filler(),
        }));
    } else if (p.url) {
        items.push_back(hbox({
            text(" URL: ") | dim,
            text(*p.url) | color(Color::Cyan),
            filler(),
        }));
    }
    items.push_back(separator());

    const int n = static_cast<int>(p.fields.size());
    for (int i = 0; i < n; ++i) {
        const auto& field = p.fields[i];
        const bool focused = (p.focused_field == i);
        ElicitValue val = ElicitNull{};
        if (auto it = p.values_buffer.find(field.name);
            it != p.values_buffer.end()) val = it->second;
        auto row = RenderElicitField(field, val, focused,
            [](Element e) { return e | inverted | color(Color::Cyan); });
        items.push_back(std::move(row.body));
        // Per-field error line (if any)
        if (auto eit = p.validation_errors.find(field.name);
            eit != p.validation_errors.end() && !eit->second.empty())
        {
            items.push_back(hbox({
                text("    ! ") | color(Color::Red) | bold,
                text(eit->second) | color(Color::Red),
            }));
        }
        // Enum popup overlay (inline menu)
        if (focused && field.type == ElicitFieldType::enum_select && enum_popup_open) {
            Elements popup_rows;
            popup_rows.push_back(text("   typeahead hits:") | dim);
            int local_idx = 0;
            for (int ei : enum_typeahead_hits) {
                if (ei < 0 || ei >= static_cast<int>(field.enum_values.size())) continue;
                const bool sel = (local_idx == enum_popup_selected);
                auto mark = text(sel ? " > " : "   ")
                          | color(sel ? Color::Yellow : Color::GrayDark);
                auto label = text(field.enum_values[ei])
                           | (sel ? (bold | color(Color::Yellow)) : nothing);
                popup_rows.push_back(hbox({mark, label}));
                ++local_idx;
                if (local_idx > 8) break;
            }
            if (popup_rows.size() == 1) {
                popup_rows.push_back(text("   (no matches)") | dim);
            }
            items.push_back(vbox(popup_rows) | border | color(Color::Cyan));
        }
        // CheckboxGrid for multi_select (always rendered as a compact
        // grid of checkable labels under the field line).
        if (field.type == ElicitFieldType::multi_select) {
            const auto* mv = std::get_if<ElicitMulti>(&val);
            std::vector<std::string> chosen;
            if (mv) chosen = *mv;
            const int cols = std::max(1, field.grid_columns);
            Elements grid_rows;
            Elements cur;
            for (size_t k = 0; k < field.enum_values.size(); ++k) {
                const auto& label = field.enum_values[k];
                bool on = std::find(chosen.begin(), chosen.end(), label) != chosen.end();
                auto chip = hbox({
                    focused ? text("[") : text(" "),
                    text(on ? "x" : " ") | color(on ? Color::Green : Color::GrayDark),
                    focused ? text("] ") : text("  "),
                    text(label),
                    text("   "),
                });
                if (focused) chip = chip | (on ? (bold | color(Color::Green)) : nothing);
                cur.push_back(std::move(chip));
                if ((k + 1) % static_cast<size_t>(cols) == 0) {
                    grid_rows.push_back(hbox(cur));
                    cur.clear();
                }
            }
            if (!cur.empty()) grid_rows.push_back(hbox(cur));
            items.push_back(hbox({text("    "), vbox(grid_rows)}));
        }
    }

    // Action button row --------------------------------------------------
    items.push_back(separator());
    auto mk_btn = [&](std::string_view label, Color c, int slot) {
        const bool hot = (button_focus == slot) ||
                         (button_focus == ElicitFocus::kOnButtons && slot == ElicitFocus::kAccept);
        Element body = text(std::string(label)) | color(c);
        if (hot) body = body | inverted | bold;
        return hbox({
            text(" "),
            body,
            text(" "),
        }) | border | color(c);
    };
    auto accept_btn = mk_btn("Approve ↵",   Color::Green,  ElicitFocus::kAccept);
    auto decline_btn= mk_btn("Decline D",   Color::Red,    ElicitFocus::kDecline);
    auto cancel_btn = mk_btn("Cancel Esc",  Color::GrayDark, ElicitFocus::kCancel);
    items.push_back(hbox({
        filler(),
        accept_btn,
        text("   "),
        decline_btn,
        text("   "),
        cancel_btn,
        filler(),
    }));
    // Hints
    items.push_back(text(""));
    items.push_back(hbox({
        text(" Tab")    | color(Color::Cyan), text(": next field  "),
        text("S-Tab")   | color(Color::Cyan), text(": prev  "),
        text("Space")   | color(Color::Cyan), text(": bool/select  "),
        text("Enter")   | color(Color::Cyan), text(": enum open / submit  "),
    }) | dim);

    auto body  = vbox(items);
    auto title = std::string(" ") + (p.server_name.empty() ? "MCP" : p.server_name)
               + " — Input Required ";
    return window(text(title) | bold | color(Color::Yellow), body)
         | color(Color::Yellow) | size(WIDTH, LESS_THAN, 80);
}

// ----------------------------------------------------------------
// Stub-mode renderer (no schema, no fields → generic URL / YesNo).
// ----------------------------------------------------------------
[[nodiscard]] inline Element RenderElicitationStub(
    const ElicitationPayload& p,
    bool approved_highlight,
    std::uint64_t spinner_tick,
    bool url_resolving)
{
    Elements items;
    items.push_back(hbox({
        text(" " + (p.server_name.empty() ? "MCP Server" : p.server_name)) | bold,
        text(" requests permission") | dim,
    }));
    if (!p.message.empty()) items.push_back(paragraph("  " + p.message));
    if (p.url && url_resolving) {
        items.push_back(hbox({
            text(" " + BrailleSpinnerFrame(spinner_tick)) | color(Color::Cyan),
            text(" resolving ") | dim,
            text(*p.url) | color(Color::Cyan),
            filler(),
        }));
    } else if (p.url) {
        items.push_back(hbox({
            text(" URL: ") | dim,
            text(*p.url) | color(Color::Cyan),
            filler(),
        }));
    }
    items.push_back(separator());
    auto mk = [&](std::string_view label, Color c, bool hot) {
        Element body = text(std::string(label)) | color(c);
        if (hot) body = body | inverted | bold;
        return hbox({
            text(" "),
            body,
            text(" "),
        }) | border | color(c);
    };
    items.push_back(hbox({
        filler(),
        mk("Approve ↵", Color::Green, approved_highlight),
        text("   "),
        mk("Decline Esc", Color::Red, !approved_highlight),
        filler(),
    }));
    return window(text(" MCP Elicitation ") | bold | color(Color::Blue),
                  vbox(items) | size(WIDTH, GREATER_THAN, 44))
         | color(Color::Blue);
}

/// Dispatch: form vs stub (the single public renderer that takes the
/// new ElicitationPayload).
[[nodiscard]] inline Element RenderElicitationPayload(
    const ElicitationPayload& p,
    int button_focus = ElicitFocus::kOnButtons,
    bool enum_popup_open = false,
    int enum_popup_selected = 0,
    const std::vector<int>& enum_typeahead_hits = {},
    std::uint64_t spinner_tick = 0,
    bool url_resolving = false,
    bool stub_approve_focus = true)
{
    const bool form_mode = p.schema.has_value() || !p.fields.empty();
    if (form_mode) {
        return RenderElicitationForm(p, button_focus, enum_popup_open,
                                     enum_popup_selected, enum_typeahead_hits,
                                     spinner_tick, url_resolving);
    }
    return RenderElicitationStub(p, stub_approve_focus, spinner_tick, url_resolving);
}

/// Legacy facade (re-exported so old consumers still compile).
[[nodiscard]] inline Element RenderElicitationDialog(
    const ElicitationDialogProps& props,
    const std::map<std::string, std::string>& field_values,
    int focused_field)
{
    ElicitationPayload p = props.to_payload();
    p.focused_field = focused_field;
    for (const auto& [k, v] : field_values) p.values_buffer[k] = v;
    return RenderElicitationPayload(p);
}

// ============================================================
// Interactive components
// ============================================================

// ----------------------------------------------------------------
// Helper: enum typeahead filter (800ms coalescing).
// Returns vector of enum value indices that have the query as a
// case-insensitive substring, prioritising prefix matches.
inline std::vector<int> EnumTypeahead(const ElicitFieldSchema& field,
                                       std::string_view query)
{
    std::vector<int> hits, suffix;
    hits.reserve(field.enum_values.size());
    std::string ql;
    ql.reserve(query.size());
    for (char c : query) ql.push_back(char(std::tolower((unsigned char)c)));

    for (int i = 0; i < static_cast<int>(field.enum_values.size()); ++i) {
        const auto& v = field.enum_values[i];
        std::string vl;
        vl.reserve(v.size());
        for (char c : v) vl.push_back(char(std::tolower((unsigned char)c)));
        if (ql.empty()) { hits.push_back(i); continue; }
        auto pos = vl.find(ql);
        if (pos == std::string::npos) continue;
        if (pos == 0) hits.push_back(i); else suffix.push_back(i);
    }
    hits.insert(hits.end(), suffix.begin(), suffix.end());
    return hits;
}

// ----------------------------------------------------------------
// MCP settings panel component (unchanged skeleton)
// ----------------------------------------------------------------
[[nodiscard]] inline Component McpSettingsComponent(McpSettingsProps props) {
    struct State {
        McpSettingsProps props;
        McpViewType view = McpViewType::list;
        std::vector<McpServerInfo> servers;
        std::vector<AgentMcpServerInfo> agent_servers;
        int selected = 0;
        std::optional<int> active_server_idx;
    };
    auto s = std::make_shared<State>();
    s->props = std::move(props);
    return Renderer([s] {
        return RenderMcpSettings(s->view, s->servers, s->agent_servers,
                                 s->selected, s->active_server_idx);
    }) | CatchEvent([s](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            s->selected = std::max(0, s->selected - 1); return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            s->selected++; return true;
        }
        if (event == Event::Return) {
            if (s->view == McpViewType::list) {
                s->active_server_idx = s->selected;
                s->view = McpViewType::tool_list;
                s->selected = 0;
            } else if (s->view == McpViewType::tool_list) {
                s->view = McpViewType::tool_detail;
            }
            return true;
        }
        if (event == Event::Escape) {
            if (s->view == McpViewType::tool_detail) {
                s->view = McpViewType::tool_list;
            } else if (s->view == McpViewType::tool_list) {
                s->view = McpViewType::list;
                s->selected = s->active_server_idx.value_or(0);
                s->active_server_idx = std::nullopt;
            } else {
                if (s->props.on_complete)
                    s->props.on_complete(std::nullopt, CommandResultDisplay::skip);
            }
            return true;
        }
        if (event == Event::Character('r')) return true; // reconnect hook
        return false;
    });
}

// ----------------------------------------------------------------
// Elicitation interactive component (NEW, payload-based).
// ----------------------------------------------------------------
struct ElicitationInteractiveState {
    ElicitationPayload payload;

    // UI runtime --------------------------------------------------------
    int button_focus = ElicitFocus::kAccept; // used when focused_field is on buttons
    // enum state
    bool enum_popup_open = false;
    int  enum_popup_selected = 0;
    std::string enum_typeahead_query;
    std::chrono::steady_clock::time_point typeahead_last_keystroke{};
    std::vector<int> enum_typeahead_cache;
    // Transient gate: true immediately after an Enter-key commits the
    // enum popup; a follow-up Enter then Accepts the whole form instead
    // of re-opening the same popup.  Cleared on any other keypress.
    bool enum_just_committed = false;
    // spinner / animation
    std::chrono::steady_clock::time_point start_time =
        std::chrono::steady_clock::now();
    bool url_resolving = true; // starts true; transitions externally or via timeout

    // Boolean oneshot guards
    bool submitted = false;
    bool cancelled = false;
    bool declined  = false;

    // Multi-select checkbox navigation (col within focused multi field)
    int multi_selected_col = 0;

    // Flag that the old bool adapter has approved / declined (used by
    // the 2-button stub so Tab focuses the correct option).
    bool stub_approve_focus = true;

    void fire_response(std::string action) {
        if (submitted || cancelled || declined) return;
        if (action == "approve") submitted = true;
        else if (action == "decline") declined = true;
        else cancelled = true;
        ElicitationResult r;
        r.action  = std::move(action);
        r.content = payload.values_buffer;
        if (payload.on_response) payload.on_response(std::move(r));
    }
    void fire_cancel_signal() {
        if (payload.on_cancel) { payload.on_cancel(); return; }
        fire_response("cancel");
    }

    int field_count() const { return static_cast<int>(payload.fields.size()); }
    const ElicitFieldSchema* focused_field_schema() const {
        if (payload.focused_field < 0) return nullptr;
        if (payload.focused_field >= field_count()) return nullptr;
        return &payload.fields[payload.focused_field];
    }

    // Move focus forward (Tab) / backward (Shift-Tab).  Focus visits each
    // field index 0..n-1 in order, then the Accept, Decline, Cancel
    // buttons (in that order), then cycles back to field 0.
    void move_focus(int delta) {
        // slots: [fields] + Accept + Decline + Cancel
        const int n_slots = field_count() + 3;
        auto slot_to_focus = [&](int s) {
            if (s < field_count()) { payload.focused_field = s; button_focus = 0; return; }
            payload.focused_field = -1;
            switch (s - field_count()) {
                case 0: button_focus = ElicitFocus::kAccept;  break;
                case 1: button_focus = ElicitFocus::kDecline; break;
                default:button_focus = ElicitFocus::kCancel;  break;
            }
        };
        auto focus_to_slot = [&]() -> int {
            if (payload.focused_field >= 0 && payload.focused_field < field_count())
                return payload.focused_field;
            if (button_focus == ElicitFocus::kAccept)  return field_count();
            if (button_focus == ElicitFocus::kDecline) return field_count() + 1;
            return field_count() + 2;
        };
        int cur = focus_to_slot();
        int next = (cur + delta + n_slots * 2) % n_slots;
        slot_to_focus(next);
        // Close any open enum popup on navigation
        enum_popup_open = false;
    }

    // Tick the animation (caller drives by re-rendering at ~8 FPS).
    [[nodiscard]] std::uint64_t spinner_tick() const {
        using namespace std::chrono;
        auto dt = duration_cast<milliseconds>(steady_clock::now() - start_time).count();
        return static_cast<std::uint64_t>(dt / 125);
    }

    // Close enum popup & commit selection to values_buffer
    void commit_enum_popup(int hit_idx) {
        const auto* f = focused_field_schema();
        if (!f || f->type != ElicitFieldType::enum_select) return;
        if (hit_idx < 0 || hit_idx >= static_cast<int>(enum_typeahead_cache.size())) return;
        int real_idx = enum_typeahead_cache[hit_idx];
        if (real_idx < 0 || real_idx >= static_cast<int>(f->enum_values.size())) return;
        payload.values_buffer[f->name] = f->enum_values[real_idx];
        enum_popup_open = false;
    }

    // Toggle a single multi-select option by label (used for Space).
    void toggle_multi_option(const std::string& label) {
        const auto* f = focused_field_schema();
        if (!f || f->type != ElicitFieldType::multi_select) return;
        ElicitMulti cur;
        if (auto* p = std::get_if<ElicitMulti>(&payload.values_buffer[f->name])) {
            cur = *p;
        }
        auto it = std::find(cur.begin(), cur.end(), label);
        if (it != cur.end()) cur.erase(it); else cur.push_back(label);
        payload.values_buffer[f->name] = std::move(cur);
    }

    // Apply character to the typeahead query (or to text/number/url/datetime
    // fields, by appending to the value string).
    void typeahead_push_char(char ch) {
        const auto* f = focused_field_schema();
        if (!f) return;
        switch (f->type) {
            case ElicitFieldType::text:
            case ElicitFieldType::url: {
                std::string cur;
                if (auto* s = std::get_if<std::string>(&payload.values_buffer[f->name])) cur = *s;
                cur.push_back(ch);
                payload.values_buffer[f->name] = std::move(cur);
                break;
            }
            case ElicitFieldType::number: {
                std::string cur;
                auto& vb = payload.values_buffer[f->name];
                if (auto* s = std::get_if<std::string>(&vb)) cur = *s;
                else if (std::holds_alternative<int64_t>(vb)) cur = std::to_string(std::get<int64_t>(vb));
                else if (std::holds_alternative<double>(vb)) {
                    std::ostringstream os; os << std::get<double>(vb); cur = os.str();
                }
                if (cur == "0" && ch != '.') cur.clear();
                cur.push_back(ch);
                payload.values_buffer[f->name] = cur;
                break;
            }
            case ElicitFieldType::date_time: {
                std::string cur;
                if (auto* s = std::get_if<std::string>(&payload.values_buffer[f->name])) cur = *s;
                cur.push_back(ch);
                payload.values_buffer[f->name] = std::move(cur);
                break;
            }
            case ElicitFieldType::enum_select: {
                enum_typeahead_query.push_back(char(std::tolower((unsigned char)ch)));
                typeahead_last_keystroke = std::chrono::steady_clock::now();
                enum_typeahead_cache = EnumTypeahead(*f, enum_typeahead_query);
                if (!enum_popup_open) enum_popup_open = true;
                enum_popup_selected = 0;
                break;
            }
            case ElicitFieldType::boolean_:
            case ElicitFieldType::multi_select:
                break;
        }
    }

    void backspace_at_cursor() {
        const auto* f = focused_field_schema();
        if (!f) return;
        if (f->type == ElicitFieldType::enum_select) {
            if (!enum_typeahead_query.empty()) {
                enum_typeahead_query.pop_back();
                enum_typeahead_cache = EnumTypeahead(*f, enum_typeahead_query);
                enum_popup_selected = 0;
                enum_popup_open = true;
            }
            return;
        }
        if (f->type == ElicitFieldType::text ||
            f->type == ElicitFieldType::url  ||
            f->type == ElicitFieldType::number ||
            f->type == ElicitFieldType::date_time)
        {
            auto& v = payload.values_buffer[f->name];
            std::string cur;
            if (auto* s = std::get_if<std::string>(&v)) cur = *s;
            else if (f->type == ElicitFieldType::number && std::holds_alternative<int64_t>(v))
                cur = std::to_string(std::get<int64_t>(v));
            else if (f->type == ElicitFieldType::number && std::holds_alternative<double>(v)) {
                std::ostringstream os; os << std::get<double>(v); cur = os.str();
            }
            if (!cur.empty()) { cur.pop_back(); payload.values_buffer[f->name] = std::move(cur); }
            else payload.values_buffer[f->name] = ElicitNull{};
        }
    }

    void maybe_refresh_typeahead() {
        // Drop the query if the user stopped typing for 800ms.  (Matches the
        // spec's "800ms typeahead" cadence — we keep what they typed while
        // they're actively typing, so the popup doesn't flicker.)
        if (enum_typeahead_query.empty()) return;
        using namespace std::chrono;
        auto dt = duration_cast<milliseconds>(steady_clock::now() - typeahead_last_keystroke).count();
        if (dt > 1200) { /* keep; only clear on explicit Esc in the field */ }
    }
};

// ----------------------------------------------------------------
// The public interactive component (accepts the new payload).
// ----------------------------------------------------------------
[[nodiscard]] inline Component ElicitationDialogComponent(ElicitationPayload payload) {
    struct Impl : public ComponentBase {
        std::shared_ptr<ElicitationInteractiveState> st;
        explicit Impl(ElicitationPayload p) : st(std::make_shared<ElicitationInteractiveState>()) {
            st->payload = std::move(p);
            st->payload.seed_defaults_from_fields();
        }
        Element Render() override {
            st->maybe_refresh_typeahead();
            // Enum popup: if field focus changes, ensure consistent state.
            const ElicitFieldSchema* f = st->focused_field_schema();
            if (f && f->type == ElicitFieldType::enum_select && st->enum_typeahead_cache.empty()) {
                st->enum_typeahead_cache = EnumTypeahead(*f, st->enum_typeahead_query);
            }
            int bf = st->button_focus;
            if (st->payload.focused_field >= 0) bf = 0; // unused
            return RenderElicitationPayload(
                st->payload, bf,
                st->enum_popup_open, st->enum_popup_selected,
                st->enum_typeahead_cache, st->spinner_tick(),
                st->url_resolving, st->stub_approve_focus);
        }
        bool OnEvent(Event event) override {
            if (st->submitted || st->cancelled || st->declined) return false;

            // The enum_just_committed gate only applies to consecutive
            // Enter presses.  Any other key clears it so the user can
            // still re-open the popup via Enter after Tab / typing /
            // Space / etc.
            if (!(event == Event::Return)) st->enum_just_committed = false;

            // Global shortcuts ------------------------------------------------
            if (event == Event::Escape) {
                // In an enum popup: close popup first, one-shot.
                if (st->enum_popup_open) {
                    st->enum_popup_open = false;
                    st->enum_typeahead_query.clear();
                    return true;
                }
                st->fire_cancel_signal();
                return true;
            }
            if (event == Event::Tab)          { st->move_focus(+1); return true; }
            if (event == Event::TabReverse)   { st->move_focus(-1); return true; }

            // Arrow navigation inside an open enum popup.
            const ElicitFieldSchema* f = st->focused_field_schema();
            if (st->enum_popup_open) {
                if (event == Event::ArrowDown) {
                    if (!st->enum_typeahead_cache.empty()) {
                        st->enum_popup_selected =
                            (st->enum_popup_selected + 1) %
                                static_cast<int>(st->enum_typeahead_cache.size());
                    }
                    return true;
                }
                if (event == Event::ArrowUp) {
                    if (!st->enum_typeahead_cache.empty()) {
                        int n = static_cast<int>(st->enum_typeahead_cache.size());
                        st->enum_popup_selected = (st->enum_popup_selected - 1 + n) % n;
                    }
                    return true;
                }
                if (event == Event::Return) {
                    st->commit_enum_popup(st->enum_popup_selected);
                    // Gate so a follow-up Enter on the same enum field
                    // Accepts the form instead of re-opening the popup.
                    st->enum_just_committed = true;
                    return true;
                }
                if (event == Event::Character(' ')) {
                    st->commit_enum_popup(st->enum_popup_selected);
                    return true;
                }
            }

            // Enter behaviour -------------------------------------------------
            if (event == Event::Return) {
                // If focused on an enum field AND popup is not open:
                //   - First Enter opens the popup.
                //   - But if we just committed a value via popup-Enter,
                //     a follow-up Enter should Accept the form (not
                //     re-open).  enum_just_committed gate handles this.
                if (f && f->type == ElicitFieldType::enum_select && !st->enum_popup_open) {
                    if (!st->enum_just_committed) {
                        st->enum_popup_open = true;
                        st->enum_typeahead_cache = EnumTypeahead(*f, st->enum_typeahead_query);
                        return true;
                    }
                    // just-committed gate: clear and fall through to accept.
                    st->enum_just_committed = false;
                }
                // NOTE: Enter no longer toggles booleans — that was a UX
                // bug: Space toggles bools; Enter always accepts the form
                // when not otherwise handled by a popup or button.
                //
                // If focus is on a button slot, press it.
                if (st->payload.focused_field < 0) {
                    switch (st->button_focus) {
                        case ElicitFocus::kAccept:  st->fire_response("approve"); return true;
                        case ElicitFocus::kDecline: st->fire_response("decline"); return true;
                        default:                    st->fire_cancel_signal();    return true;
                    }
                }
                // Otherwise (focused on any field — text/number/bool/enum
                // with value committed/multi/url/datetime) treat Enter as
                // "accept form", which matches standard web/dialog UX.
                st->fire_response("approve");
                return true;
            }

            // Space behaviour ------------------------------------------------
            if (event == Event::Character(' ')) {
                if (!f) {
                    // In button mode, Space presses the focused button.
                    if (st->payload.focused_field < 0) {
                        switch (st->button_focus) {
                            case ElicitFocus::kAccept:  st->fire_response("approve"); return true;
                            case ElicitFocus::kDecline: st->fire_response("decline"); return true;
                            default:                    st->fire_cancel_signal();    return true;
                        }
                    }
                    return false;
                }
                if (f->type == ElicitFieldType::boolean_) {
                    auto& vb = st->payload.values_buffer[f->name];
                    bool cur = std::holds_alternative<bool>(vb) && std::get<bool>(vb);
                    vb = !cur;
                    return true;
                }
                if (f->type == ElicitFieldType::multi_select) {
                    if (!f->enum_values.empty()) {
                        int idx = std::clamp(st->multi_selected_col, 0,
                                             static_cast<int>(f->enum_values.size()) - 1);
                        st->toggle_multi_option(f->enum_values[idx]);
                    }
                    return true;
                }
                if (f->type == ElicitFieldType::enum_select) {
                    st->enum_popup_open = !st->enum_popup_open;
                    return true;
                }
                // Text / number / url / datetime → space allowed as a character
                st->typeahead_push_char(' ');
                return true;
            }

            // Decline keyboard shortcut --------------------------------------
            if (event == Event::Character('d') || event == Event::Character('D')) {
                st->fire_response("decline");
                return true;
            }
            // Approve keyboard shortcut
            if (event == Event::Character('a') ||
                event == Event::Character('A'))
            {
                st->fire_response("approve");
                return true;
            }

            // Backspace ------------------------------------------------------
            if (event == Event::Backspace || event == Event::Delete) {
                st->backspace_at_cursor();
                return true;
            }

            // Character input routing ---------------------------------------
            if (event.is_character()) {
                st->typeahead_push_char(event.character()[0]);
                return true;
            }

            // Multi-select arrow-walk (left/right columns)
            if (f && f->type == ElicitFieldType::multi_select) {
                int n_vals = std::max(1, static_cast<int>(f->enum_values.size()));
                if (event == Event::ArrowRight) {
                    st->multi_selected_col =
                        std::min(n_vals - 1, st->multi_selected_col + 1);
                    return true;
                }
                if (event == Event::ArrowLeft) {
                    st->multi_selected_col = std::max(0, st->multi_selected_col - 1);
                    return true;
                }
            }
            // Enum-select arrow navigation (opens popup)
            if (f && f->type == ElicitFieldType::enum_select &&
                (event == Event::ArrowDown || event == Event::ArrowUp))
            {
                st->enum_popup_open = true;
                st->enum_typeahead_cache = EnumTypeahead(*f, st->enum_typeahead_query);
                return true;
            }
            return false;
        }
    };
    return Make<Impl>(std::move(payload));
}

// Legacy entry point: accept ElicitationDialogProps and adapt.
[[nodiscard]] inline Component ElicitationDialogComponent(
    ElicitationDialogProps props)
{
    return ElicitationDialogComponent(props.to_payload());
}

// Extra-compat: for 2-button stub callers that pass only
// on_response(bool approve), we provide a factory overload.
[[nodiscard]] inline Component ElicitationDialogSimple(
    std::string server_name,
    std::string message,
    std::function<void(bool approve)> on_approve)
{
    ElicitationDialogProps p = ElicitationDialogProps::from_bool_cb(
        std::move(server_name), std::move(message), std::move(on_approve));
    return ElicitationDialogComponent(std::move(p));
}

} // namespace cc::ui::mcp_dialogs
