/// @file mcp_server_details.cppm
/// @brief Per-server detail dialog with 4 Tabs:
///   Tab 1 – Tools        (list + JSON-schema based Test-call form)
///   Tab 2 – Resources    (list + preview / open-in-external)
///   Tab 3 – Prompts      (list + arguments + try prompt)
///   Tab 4 – Settings     (rename, edit command/URL, autostart, log level,
///                         env var editing, delete server)
///
/// Data sources (100% delegation — no engine logic reimplemented):
///   - cc.services.mcp.types.McpTool / McpResource / McpPrompt
///   - cc.services.mcp.connection_manager (list_tools / list_resources /
///     list_prompts / call_tool / snapshot_server)
///   - cc.services.mcp.config.ServerConfig  (settings tab write-back)
module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <format>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.mcp.mcp_server_details;

import cc.services.mcp.types;
import cc.services.mcp.connection_manager;
import cc.services.mcp.config;
import cc.ui.mcp.mcp_server_list;

export namespace cc::ui::mcp {
using namespace ftxui;
using namespace cc::services::mcp;

// ============================================================
// Types
// ============================================================

enum class DetailTab : std::uint8_t {
    Tools = 0,
    Resources = 1,
    Prompts = 2,
    Settings = 3,
};
constexpr int kTabCount = 4;

struct ToolCallFormState {
    bool open = false;
    int tool_index = -1;
    // Flat key/value JSON map edited by the user; we don't attempt full schema
    // rendering (nested objects) in the first cut — just top-level key=value
    // pairs that the engine re-serializes as {"k": "v", …}.
    std::vector<std::pair<std::string, std::string>> fields;
    std::string last_result_json;
    bool last_ok = false;
    int focus_field = 0;
    enum class Stage { Build, Waiting, Done } stage = Stage::Build;
};

struct ResourcePreviewState {
    bool open = false;
    int resource_index = -1;
    std::string content_text;
    bool preview_ok = false;
};

struct PromptTryState {
    bool open = false;
    int prompt_index = -1;
    std::vector<std::pair<std::string, std::string>> arg_values;
    std::string rendered_messages_text;
};

struct ServerDetailsProps {
    std::string server_name;   // initial server (caller sets it)

    // Engine hooks.
    std::function<std::optional<McpServerSnapshot>()> on_snapshot;
    std::function<ToolCallResult(const std::string& tool_name,
                                 const std::string& args_json)> on_call_tool;
    std::function<ResourceReadResult(const std::string& uri)> on_read_resource;
    std::function<std::string(const std::string& prompt_name,
                              const std::vector<std::pair<std::string,std::string>>& args)>
        on_render_prompt;
    std::function<void()> on_open_external;  // for "open resource externally"
    std::function<std::expected<void, std::string>(const ServerConfig& patch)>
        on_update_config;
    std::function<void()> on_delete;
    std::function<void()> on_restart;

    // Completion.
    std::function<void()> on_cancel;
    std::function<void(const std::string& note)> on_done;
};

// ============================================================
// Tab rendering: header + Tabs
// ============================================================

[[nodiscard]] inline Element RenderTabsRow(DetailTab active, ConnectionStatus st) {
    auto pill = [](DetailTab cur, DetailTab me, std::string_view label,
                   Color accent) {
        bool a = cur == me;
        return hbox({
            text(" "),
            text(std::string{label}) | color(a ? accent : Color::GrayDark)
                | (a ? bold : nothing),
            text(" "),
        }) | (a ? bgcolor(Color::RGB(25, 40, 55)) | border
                 : bgcolor(Color::RGB(12, 14, 20)));
    };
    auto [icon, ic] = status_badge(st);  // defined in mcp_server_list (linker)
    return hbox({
        text(" " + icon + " ") | color(ic),
        pill(active, DetailTab::Tools,     "🔧 Tools",     Color::Cyan),
        text(" "),
        pill(active, DetailTab::Resources, "📦 Resources", Color::Blue),
        text(" "),
        pill(active, DetailTab::Prompts,   "📋 Prompts",   Color::Green),
        text(" "),
        pill(active, DetailTab::Settings,  "⚙ Settings",  Color::Yellow),
        filler(),
    });
}

inline Element status_badge_decor(ConnectionStatus s) {
    // Standalone re-declaration of status_badge to avoid cross-module inline
    // ordering issues.
    switch (s) {
        case ConnectionStatus::Connected:    return text("●") | color(Color::Green);
        case ConnectionStatus::Connecting:   return text("◐") | color(Color::Yellow);
        case ConnectionStatus::Disconnected: return text("○") | color(Color::GrayDark);
        case ConnectionStatus::NeedsAuth:    return text("△") | color(Color::YellowLight);
        case ConnectionStatus::Error:        return text("✗") | color(Color::Red);
    }
    return text("?");
}

[[nodiscard]] inline Element RenderHeader(
    const std::string& name,
    const McpServerSnapshot* snap) {

    auto status_el = snap ? status_badge_decor(snap->status)
                          : text("○") | color(Color::GrayDark);
    auto ver = (snap && snap->server_info) ? *snap->server_info : std::string{};
    std::string duration;
    return hbox({
        text(" ") | bold,
        text(name) | bold | color(Color::White),
        text("   ") | dim,
        status_el,
        text(" ") | dim,
        text(snap ? [&]{
            switch (snap->status) {
                case ConnectionStatus::Connected:    return "connected";
                case ConnectionStatus::Connecting:   return "connecting…";
                case ConnectionStatus::Disconnected: return "disconnected";
                case ConnectionStatus::NeedsAuth:    return "needs auth";
                case ConnectionStatus::Error:        return "error";
            }
            return "unknown";
        }() : "unknown") | dim,
        text("   ") | dim,
        text(ver.empty() ? std::string{} : "v" + ver) | color(Color::Cyan) | dim,
        filler(),
        text("[Esc]") | color(Color::Cyan) | bold,
        text(" back") | dim,
    });
}

// ============================================================
// Tab 1: Tools
// ============================================================

[[nodiscard]] inline Element RenderToolList(
    const std::vector<McpTool>& tools,
    int selected,
    int focus) {

    Elements e;
    e.push_back(hbox({
        text(" Registered tools ") | bold,
        text(std::format("({})", tools.size())) | dim,
        filler(),
        text("[↵] test call") | dim,
    }));
    e.push_back(separatorLight());

    if (tools.empty()) {
        e.push_back(text("   No tools advertised by this server.") | dim);
        return vbox(e);
    }

    for (int i = 0; i < static_cast<int>(tools.size()); ++i) {
        const auto& t = tools[i];
        bool sel = (i == selected);
        bool focused = (i == focus);

        auto row = vbox({
            hbox({
                text(focused ? " › " : "   ") | color(Color::Cyan),
                text(t.name) | (sel || focused ? bold : nothing),
                filler(),
                text(" [T]est") | color(Color::Cyan) | dim,
            }),
            hbox({
                text("     ") | dim,
                paragraph(t.description.empty() ? "(no description)" : t.description) | dim,
            }),
        });
        if (focused) row = row | bgcolor(Color::RGB(20, 35, 50));
        e.push_back(row);
    }

    return vbox(e) | yframe;
}

// Very small JSON-kv editor (top-level string fields only).
[[nodiscard]] inline Element RenderToolCallForm(
    const McpTool& tool,
    const ToolCallFormState& s) {

    Elements e;
    e.push_back(hbox({
        text(" Test call: ") | bold | color(Color::Cyan),
        text(tool.name) | bold,
    }));
    e.push_back(separatorLight());
    e.push_back(text(" Description:") | dim);
    e.push_back(paragraph("   " + tool.description) | dim);
    e.push_back(text(""));
    e.push_back(text(" Arguments (top-level key=value, JSON string values)") | bold | dim);
    e.push_back(text(""));

    if (s.fields.empty()) {
        e.push_back(text("   This tool declares no arguments — click [R]un to call.") | dim);
    }
    for (std::size_t i = 0; i < s.fields.size(); ++i) {
        bool f = (static_cast<int>(i) == s.focus_field);
        auto& [k, v] = s.fields[i];
        e.push_back(hbox({
            text(std::format(" {:>2}. ", i + 1)) | dim,
            text(k + " = ") | color(Color::Cyan) | bold,
            text(v.empty() ? "(empty string)" : v) | (f ? inverted : nothing),
            filler(),
        }));
    }

    if (s.stage == ToolCallFormState::Stage::Waiting) {
        e.push_back(text(""));
        e.push_back(hbox({text(" ◐ ") | color(Color::Yellow) | blink,
                          text("Calling tool…") | color(Color::Yellow)}));
    } else if (s.stage == ToolCallFormState::Stage::Done) {
        e.push_back(text(""));
        auto title_el = s.last_ok
            ? hbox({text(" ✓ ") | color(Color::Green),
                    text("Result") | color(Color::Green) | bold})
            : hbox({text(" ✗ ") | color(Color::Red),
                    text("Error") | color(Color::Red) | bold});
        e.push_back(title_el);
        e.push_back(separatorLight());
        std::string trimmed = s.last_result_json;
        if (trimmed.size() > 600) trimmed = trimmed.substr(0, 600) + "…";
        e.push_back(paragraph(trimmed) | color(s.last_ok ? Color::GrayLight : Color::RedLight) | dim);
    }

    e.push_back(text(""));
    e.push_back(hbox({
        text("[↵/R]") | color(Color::Green), text("un  "),
        text("[Tab]") | color(Color::Cyan), text(" field  "),
        text("[+]") | color(Color::Cyan), text(" add field  "),
        text("[-]") | color(Color::Red), text(" del field  "),
        text("[Esc]") | color(Color::GrayLight), text(" close"),
    }) | dim);

    return window(text(" Test call ") | bold | color(Color::Cyan),
                  vbox(e) | xflex) | color(Color::Cyan);
}

// ============================================================
// Tab 2: Resources
// ============================================================

[[nodiscard]] inline Element RenderResourceList(
    const std::vector<McpResource>& resources,
    int,
    int focus) {

    Elements e;
    e.push_back(hbox({
        text(" Registered resources ") | bold,
        text(std::format("({})", resources.size())) | dim,
        filler(),
        text("[P]rev  [O]pen") | dim,
    }));
    e.push_back(separatorLight());

    if (resources.empty()) {
        e.push_back(text("   No resources advertised by this server.") | dim);
        return vbox(e);
    }

    for (int i = 0; i < static_cast<int>(resources.size()); ++i) {
        const auto& r = resources[i];
        bool focused = (i == focus);
        auto row = hbox({
            text(focused ? " › " : "   ") | color(Color::Blue),
            text("📎 ") | dim,
            text(r.name.empty() ? "(unnamed)" : r.name) | (focused ? bold : nothing),
            text("  ") | dim,
            text(r.mime_type) | dim | color(Color::GrayLight),
            filler(),
            text(r.uri) | dim | color(Color::Blue),
        });
        if (focused) row = row | bgcolor(Color::RGB(18, 28, 48));
        e.push_back(row);
    }
    return vbox(e) | yframe;
}

[[nodiscard]] inline Element RenderResourcePreview(const McpResource& res,
                                                   const ResourcePreviewState& s) {
    Elements e;
    e.push_back(hbox({text(" Preview: ") | bold | color(Color::Blue),
                      text(res.name) | bold}));
    e.push_back(text(" URI: " + res.uri) | dim);
    e.push_back(separatorLight());
    if (!s.preview_ok) {
        e.push_back(text("   Preview not loaded — press [P] to fetch.") | dim);
    } else {
        std::string t = s.content_text;
        if (t.size() > 1500) t = t.substr(0, 1500) + "\n… (truncated)";
        e.push_back(paragraph(t) | color(Color::GrayLight));
    }
    e.push_back(text(""));
    e.push_back(hbox({
        text("[P]") | color(Color::Blue), text(" review  "),
        text("[O]") | color(Color::Green), text(" open externally  "),
        text("[Esc]") | dim, text(" close"),
    }) | dim);
    return window(text(" Resource preview ") | bold | color(Color::Blue),
                  vbox(e) | xflex) | color(Color::Blue);
}

// ============================================================
// Tab 3: Prompts
// ============================================================

[[nodiscard]] inline Element RenderPromptList(
    const std::vector<McpPrompt>& prompts,
    int,
    int focus) {

    Elements e;
    e.push_back(hbox({
        text(" Registered prompts ") | bold,
        text(std::format("({})", prompts.size())) | dim,
        filler(),
        text("[T]ry") | dim,
    }));
    e.push_back(separatorLight());

    if (prompts.empty()) {
        e.push_back(text("   No prompts advertised by this server.") | dim);
        return vbox(e);
    }

    for (int i = 0; i < static_cast<int>(prompts.size()); ++i) {
        const auto& p = prompts[i];
        bool focused = (i == focus);
        Elements arg_els;
        for (const auto& a : p.arguments) {
            arg_els.push_back(text(a.name + (a.required ? "*" : "")) | dim
                              | color(Color::MagentaLight));
            arg_els.push_back(text(" ") | dim);
        }
        auto row = vbox({
            hbox({
                text(focused ? " › " : "   ") | color(Color::Green),
                text("📋 ") | dim,
                text(p.name) | (focused ? bold : nothing),
                text(" ") | dim,
                hbox(std::move(arg_els)),
                filler(),
                text("[T]ry") | dim,
            }),
            p.description.empty() ? text("") : hbox({
                text("     ") | dim,
                paragraph(p.description) | dim,
            }),
        });
        if (focused) row = row | bgcolor(Color::RGB(18, 40, 32));
        e.push_back(row);
    }
    return vbox(e) | yframe;
}

[[nodiscard]] inline Element RenderPromptTry(const McpPrompt& p,
                                             const PromptTryState& s) {
    Elements e;
    e.push_back(hbox({text(" Try prompt: ") | bold | color(Color::Green),
                      text(p.name) | bold}));
    e.push_back(separatorLight());
    e.push_back(paragraph("   " + p.description) | dim);
    e.push_back(text(""));
    e.push_back(text(" Arguments") | bold | dim);

    if (p.arguments.empty()) {
        e.push_back(text("   (no arguments)") | dim);
    } else {
        for (std::size_t i = 0; i < p.arguments.size(); ++i) {
            bool focused = (static_cast<int>(i) == s.prompt_index);  // reuse
            std::string val;
            if (i < s.arg_values.size()) val = s.arg_values[i].second;
            e.push_back(hbox({
                text(std::format(" {:>2}. ", i + 1)) | dim,
                text(p.arguments[i].name) | color(Color::Magenta) | bold,
                p.arguments[i].required ? text(" *") | color(Color::Red) : text(""),
                text(" = ") | dim,
                text(val.empty() ? "(empty)" : val) | (focused ? inverted : nothing),
                filler(),
            }));
            if (!p.arguments[i].description.empty()) {
                e.push_back(text("     " + p.arguments[i].description) | dim);
            }
        }
    }

    if (!s.rendered_messages_text.empty()) {
        e.push_back(text(""));
        e.push_back(text(" Rendered preview:") | bold | dim);
        std::string r = s.rendered_messages_text;
        if (r.size() > 800) r = r.substr(0, 800) + "…";
        e.push_back(paragraph(r) | color(Color::GrayLight));
    }

    e.push_back(text(""));
    e.push_back(hbox({
        text("[↵]") | color(Color::Green), text(" render  "),
        text("[Tab]") | color(Color::Cyan), text(" arg  "),
        text("[Esc]") | dim, text(" close"),
    }) | dim);
    return window(text(" Try prompt ") | bold | color(Color::Green),
                  vbox(e) | xflex) | color(Color::Green);
}

// ============================================================
// Tab 4: Settings
// ============================================================

[[nodiscard]] inline Element RenderSettingsTab(
    const ServerConfig* cfg,
    ConnectionStatus,
    int focus_idx) {

    Elements e;
    e.push_back(text(" Server settings") | bold | color(Color::Yellow));
    e.push_back(separatorLight());

    auto row = [&](int idx, std::string_view label, const std::string& value,
                   bool editable = true) {
        bool f = (idx == focus_idx && editable);
        return vbox({
            hbox({
                text(" " + std::string{label}) | bold,
                filler(),
                text(value.empty() ? "(unset)" : value)
                    | (f ? inverted : (value.empty() ? dim : nothing)),
            }),
            separatorLight(),
        });
    };

    if (!cfg) {
        e.push_back(text("   Config not loaded.") | dim);
        return vbox(e);
    }

    int i = 0;
    e.push_back(row(i++, "Name",            cfg->name));
    if (cfg->transport == TransportType::Stdio) {
        e.push_back(row(i++, "Command",     cfg->command));
        std::string args;
        for (auto& a : cfg->args) { if (!args.empty()) args += ' '; args += a; }
        e.push_back(row(i++, "Args",        args));
        e.push_back(row(i++, "Working dir", "(project)"));  // cwd not always stored
    } else {
        e.push_back(row(i++, "URL",         cfg->url));
        e.push_back(row(i++, "Auth",        cfg->oauth ? "OAuth" : (cfg->headers.empty() ? "None" : "Headers")));
    }
    e.push_back(row(i++, "Auto-start",    cfg->auto_start ? "enabled" : "disabled"));
    e.push_back(row(i++, "Enabled",       cfg->enabled ? "yes" : "no"));
    e.push_back(row(i++, "Timeout (ms)",  std::to_string(cfg->timeout.count())));
    e.push_back(row(i++, "Scope",         [&]{
        switch (cfg->scope) {
            case ConfigScope::Global: return "Global";
            case ConfigScope::User:   return "User";
            case ConfigScope::Project:return "Project";
            case ConfigScope::Local:  return "Local";
        } return "?";
    }()));

    // Env vars
    e.push_back(text(""));
    e.push_back(text(" Environment variables") | bold | dim);
    if (cfg->env.empty()) {
        e.push_back(text("   (none)") | dim);
    } else {
        for (const auto& [k, v] : cfg->env) {
            e.push_back(hbox({
                text("   " + k + "=") | dim,
                text(std::string(v.size(), '*')) | color(Color::MagentaLight) | dim,
            }));
        }
    }

    // Action row
    e.push_back(text(""));
    e.push_back(hbox({
        text(" [↵]") | color(Color::Cyan), text(" edit field  "),
        text("[A]") | color(Color::Cyan), text(" toggle auto  "),
        text("[E]") | color(Color::Cyan), text("dit env  "),
        text("[R]") | color(Color::Yellow), text("estart  "),
        text("[Del]") | color(Color::Red), text("ETE SERVER"),
    }) | dim);

    return vbox(e) | yframe;
}

// ============================================================
// Main component
// ============================================================

[[nodiscard]] inline Component McpServerDetails(ServerDetailsProps props) {
    struct State {
        ServerDetailsProps props;
        DetailTab tab = DetailTab::Tools;

        // Cached snapshot (may be refreshed with [U]).
        std::optional<McpServerSnapshot> snap;
        // Cached ServerConfig patch (Settings tab).
        std::optional<ServerConfig> cfg_patch;

        // Per-tab focus / selection.
        int sel_tool = 0;
        int sel_resource = 0;
        int sel_prompt = 0;
        int focus_setting = 0;

        // Sub-dialogs
        ToolCallFormState call_state;
        ResourcePreviewState preview_state;
        PromptTryState try_state;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    auto refresh_snapshot = [state] {
        if (!state->props.on_snapshot) return;
        auto s = state->props.on_snapshot();
        if (s) state->snap = *s;
    };
    refresh_snapshot();

    // Seed a ServerConfig patch from the snapshot / name.
    // (In a fully wired build the engine exposes ServerConfig directly.)
    if (state->snap) {
        ServerConfig c;
        c.name = state->snap->name;
        state->cfg_patch = c;
    }

    auto current_status = [state] {
        return state->snap ? state->snap->status : ConnectionStatus::Disconnected;
    };

    // --- Tool call form helpers ---
    auto open_tool_form = [state](int idx) {
        if (!state->snap || idx < 0 || idx >= (int)state->snap->tools.size()) return;
        state->call_state.open = true;
        state->call_state.tool_index = idx;
        state->call_state.stage = ToolCallFormState::Stage::Build;
        state->call_state.fields.clear();
        state->call_state.last_result_json.clear();
        state->call_state.focus_field = 0;
        // In a real wiring we would parse t.input_schema_json and pre-create
        // fields from the "properties" object. Here we leave the field list
        // empty — user can add fields with [+].
    };

    auto run_tool_call = [state] {
        if (state->call_state.tool_index < 0 || !state->snap) return;
        const auto& t = state->snap->tools[state->call_state.tool_index];
        state->call_state.stage = ToolCallFormState::Stage::Waiting;
        // Serialize flat fields → JSON object.
        std::string json = "{";
        for (std::size_t i = 0; i < state->call_state.fields.size(); ++i) {
            if (i) json += ",";
            auto& [k, v] = state->call_state.fields[i];
            json += "\"" + k + "\":\"" + v + "\"";
        }
        json += "}";

        auto result = state->props.on_call_tool
            ? state->props.on_call_tool(t.name, json)
            : ToolCallResult{.is_error = true, .content = {ContentItem{"text", "no handler", std::nullopt, std::nullopt}}};
        state->call_state.stage = ToolCallFormState::Stage::Done;
        state->call_state.last_ok = !result.is_error;
        std::string out;
        for (auto& c : result.content) out += c.text + "\n";
        state->call_state.last_result_json = out.empty() ? "(empty result)" : out;
    };

    // --- Resource preview helpers ---
    auto open_resource_preview = [state](int idx) {
        if (!state->snap || idx < 0 || idx >= (int)state->snap->resources.size()) return;
        state->preview_state.open = true;
        state->preview_state.resource_index = idx;
    };
    auto fetch_resource_preview = [state] {
        if (!state->snap || !state->preview_state.open ||
            state->preview_state.resource_index < 0) return;
        const auto& r = state->snap->resources[state->preview_state.resource_index];
        state->preview_state.preview_ok = false;
        state->preview_state.content_text.clear();
        if (state->props.on_read_resource) {
            auto rr = state->props.on_read_resource(r.uri);
            for (const auto& c : rr.contents) {
                state->preview_state.content_text += c.text;
            }
            state->preview_state.preview_ok = true;
        }
    };

    // --- Prompt try helpers ---
    auto open_prompt_try = [state](int idx) {
        if (!state->snap || idx < 0 || idx >= (int)state->snap->prompts.size()) return;
        const auto& p = state->snap->prompts[idx];
        state->try_state.open = true;
        state->try_state.prompt_index = idx;
        state->try_state.arg_values.clear();
        state->try_state.rendered_messages_text.clear();
        for (const auto& a : p.arguments) {
            state->try_state.arg_values.emplace_back(a.name, "");
        }
    };
    auto render_prompt = [state] {
        if (!state->try_state.open || state->try_state.prompt_index < 0 ||
            !state->snap) return;
        const auto& p = state->snap->prompts[state->try_state.prompt_index];
        state->try_state.rendered_messages_text =
            state->props.on_render_prompt
                ? state->props.on_render_prompt(p.name, state->try_state.arg_values)
                : "(render handler not wired)";
    };

    return Renderer([state, current_status] {
        Element body;

        // If a sub-dialog is open, overlay it.
        if (state->call_state.open && state->snap &&
            state->call_state.tool_index >= 0 &&
            state->call_state.tool_index < (int)state->snap->tools.size()) {
            body = dbox({
                RenderToolList(state->snap->tools,
                               state->call_state.tool_index,
                               state->sel_tool)
                    | clear_under | yframe,
                RenderToolCallForm(state->snap->tools[state->call_state.tool_index],
                                   state->call_state)
                    | center | size(WIDTH, LESS_THAN, 80),
            });
        } else if (state->preview_state.open && state->snap &&
                   state->preview_state.resource_index >= 0 &&
                   state->preview_state.resource_index < (int)state->snap->resources.size()) {
            body = dbox({
                RenderResourceList(state->snap->resources,
                                   state->preview_state.resource_index,
                                   state->sel_resource)
                    | clear_under | yframe,
                RenderResourcePreview(state->snap->resources[state->preview_state.resource_index],
                                      state->preview_state)
                    | center | size(WIDTH, LESS_THAN, 80),
            });
        } else if (state->try_state.open && state->snap &&
                   state->try_state.prompt_index >= 0 &&
                   state->try_state.prompt_index < (int)state->snap->prompts.size()) {
            body = dbox({
                RenderPromptList(state->snap->prompts,
                                 state->try_state.prompt_index,
                                 state->sel_prompt)
                    | clear_under | yframe,
                RenderPromptTry(state->snap->prompts[state->try_state.prompt_index],
                                state->try_state)
                    | center | size(WIDTH, LESS_THAN, 80),
            });
        } else {
            switch (state->tab) {
                case DetailTab::Tools:
                    body = state->snap
                        ? RenderToolList(state->snap->tools, state->sel_tool, state->sel_tool)
                        : text("   No data loaded. Press [U] to refresh.") | dim;
                    break;
                case DetailTab::Resources:
                    body = state->snap
                        ? RenderResourceList(state->snap->resources, state->sel_resource, state->sel_resource)
                        : text("   No data loaded. Press [U] to refresh.") | dim;
                    break;
                case DetailTab::Prompts:
                    body = state->snap
                        ? RenderPromptList(state->snap->prompts, state->sel_prompt, state->sel_prompt)
                        : text("   No data loaded. Press [U] to refresh.") | dim;
                    break;
                case DetailTab::Settings:
                    body = RenderSettingsTab(
                        state->cfg_patch ? &*state->cfg_patch : nullptr,
                        current_status(),
                        state->focus_setting);
                    break;
            }
        }

        auto header = RenderHeader(state->props.server_name,
                                   state->snap ? &*state->snap : nullptr);
        auto tabs = RenderTabsRow(state->tab, current_status());

        return window(
            text(" Server Details ") | bold | color(Color::Blue),
            vbox({
                header,
                separator(),
                tabs,
                separator(),
                body | flex,
                separator(),
                hbox({
                    text(" [←/→]") | color(Color::Cyan), text(" tab  "),
                    text("[j/k]") | color(Color::Cyan), text(" nav  "),
                    text("[U]") | color(Color::Cyan), text("pdate  "),
                    text("[Esc]") | color(Color::GrayLight), text(" back"),
                }) | dim,
            }) | xflex
        ) | color(Color::Blue) | size(WIDTH, LESS_THAN, 90);
    }) | CatchEvent([state, refresh_snapshot, current_status,
                      open_tool_form, run_tool_call,
                      open_resource_preview, fetch_resource_preview,
                      open_prompt_try, render_prompt](Event event) -> bool {

        // ---------- Global exits ----------
        if (event == Event::Escape) {
            // Close any open sub-dialog first.
            if (state->call_state.open) { state->call_state.open = false; return true; }
            if (state->preview_state.open) { state->preview_state.open = false; return true; }
            if (state->try_state.open) { state->try_state.open = false; return true; }
            if (state->props.on_cancel) state->props.on_cancel();
            return true;
        }

        // ---------- Tab switching ----------
        if (event == Event::ArrowRight || event == Event::Tab) {
            state->tab = static_cast<DetailTab>(
                (static_cast<int>(state->tab) + 1) % kTabCount);
            return true;
        }
        if (event == Event::ArrowLeft || event == Event::TabReverse) {
            state->tab = static_cast<DetailTab>(
                (static_cast<int>(state->tab) + kTabCount - 1) % kTabCount);
            return true;
        }
        if (event == Event::Character('1')) { state->tab = DetailTab::Tools; return true; }
        if (event == Event::Character('2')) { state->tab = DetailTab::Resources; return true; }
        if (event == Event::Character('3')) { state->tab = DetailTab::Prompts; return true; }
        if (event == Event::Character('4')) { state->tab = DetailTab::Settings; return true; }

        // ---------- Refresh ----------
        if (event == Event::Character('u') || event == Event::Character('U')) {
            refresh_snapshot();
            return true;
        }

        // ---------- Sub-dialog dispatch ----------
        if (state->call_state.open) {
            int nf = static_cast<int>(state->call_state.fields.size());
            if (event == Event::Tab) {
                state->call_state.focus_field =
                    std::max(0, nf - 1);
                return true;
            }
            if (event == Event::TabReverse) {
                state->call_state.focus_field = 0;
                return true;
            }
            if (event == Event::Character('+')) {
                state->call_state.fields.emplace_back("field_" +
                    std::to_string(nf + 1), "");
                state->call_state.focus_field = nf;
                return true;
            }
            if (event == Event::Character('-') && nf > 0) {
                int rm = std::clamp(state->call_state.focus_field, 0, nf - 1);
                state->call_state.fields.erase(
                    state->call_state.fields.begin() + rm);
                state->call_state.focus_field =
                    std::max(0, state->call_state.focus_field - 1);
                return true;
            }
            if (event == Event::Return || event == Event::Character('r') ||
                event == Event::Character('R')) {
                run_tool_call();
                return true;
            }
            if (event.is_character() && nf > 0) {
                int f = state->call_state.focus_field;
                if (f >= 0 && f < nf)
                    state->call_state.fields[f].second += event.character();
                return true;
            }
            if (event == Event::Backspace && nf > 0) {
                int f = state->call_state.focus_field;
                if (f >= 0 && f < nf && !state->call_state.fields[f].second.empty())
                    state->call_state.fields[f].second.pop_back();
                return true;
            }
            return false;
        }

        if (state->preview_state.open) {
            if (event == Event::Character('p') || event == Event::Character('P')) {
                fetch_resource_preview(); return true;
            }
            if (event == Event::Character('o') || event == Event::Character('O')) {
                if (state->props.on_open_external) state->props.on_open_external();
                return true;
            }
            return false;
        }

        if (state->try_state.open && state->snap) {
            int np = static_cast<int>(
                state->snap->prompts[state->try_state.prompt_index].arguments.size());
            if (event == Event::Tab && np > 0) {
                state->try_state.prompt_index =
                    std::min(np - 1, state->try_state.prompt_index + 1);
                return true;
            }
            if (event == Event::Return) {
                render_prompt();
                return true;
            }
            if (event.is_character()) {
                int arg = state->try_state.prompt_index;
                if (arg >= 0 && arg < (int)state->try_state.arg_values.size())
                    state->try_state.arg_values[arg].second += event.character();
                return true;
            }
            if (event == Event::Backspace) {
                int arg = state->try_state.prompt_index;
                if (arg >= 0 && arg < (int)state->try_state.arg_values.size() &&
                    !state->try_state.arg_values[arg].second.empty())
                    state->try_state.arg_values[arg].second.pop_back();
                return true;
            }
            return false;
        }

        // ---------- Per-tab list navigation ----------
        int up   = (event == Event::ArrowUp   || event == Event::Character('k'));
        int down = (event == Event::ArrowDown || event == Event::Character('j'));

        if (state->tab == DetailTab::Tools && state->snap) {
            int n = (int)state->snap->tools.size();
            if (up)   { state->sel_tool = std::max(0, state->sel_tool - 1); return true; }
            if (down) { state->sel_tool = std::max(0, std::min(n - 1, state->sel_tool + 1)); return true; }
            if (event == Event::Return || event == Event::Character('t') ||
                event == Event::Character('T')) {
                open_tool_form(state->sel_tool);
                return true;
            }
        }

        if (state->tab == DetailTab::Resources && state->snap) {
            int n = (int)state->snap->resources.size();
            if (up)   { state->sel_resource = std::max(0, state->sel_resource - 1); return true; }
            if (down) { state->sel_resource = std::max(0, std::min(n - 1, state->sel_resource + 1)); return true; }
            if (event == Event::Character('p') || event == Event::Character('P')) {
                open_resource_preview(state->sel_resource);
                fetch_resource_preview();
                return true;
            }
            if (event == Event::Character('o') || event == Event::Character('O')) {
                if (state->props.on_open_external) state->props.on_open_external();
                return true;
            }
            if (event == Event::Return) {
                open_resource_preview(state->sel_resource);
                fetch_resource_preview();
                return true;
            }
        }

        if (state->tab == DetailTab::Prompts && state->snap) {
            int n = (int)state->snap->prompts.size();
            if (up)   { state->sel_prompt = std::max(0, state->sel_prompt - 1); return true; }
            if (down) { state->sel_prompt = std::max(0, std::min(n - 1, state->sel_prompt + 1)); return true; }
            if (event == Event::Character('t') || event == Event::Character('T') ||
                event == Event::Return) {
                open_prompt_try(state->sel_prompt);
                return true;
            }
        }

        if (state->tab == DetailTab::Settings && state->cfg_patch) {
            constexpr int kSettingFieldCount = 8;
            if (up)   { state->focus_setting = std::max(0, state->focus_setting - 1); return true; }
            if (down) { state->focus_setting = std::min(kSettingFieldCount - 1,
                                                        state->focus_setting + 1); return true; }
            if (event == Event::Character('a') || event == Event::Character('A')) {
                state->cfg_patch->auto_start = !state->cfg_patch->auto_start;
                if (state->props.on_update_config)
                    state->props.on_update_config(*state->cfg_patch);
                return true;
            }
            if (event == Event::Character('r') || event == Event::Character('R')) {
                if (state->props.on_restart) state->props.on_restart();
                return true;
            }
            if (event == Event::Delete) {
                if (state->props.on_delete) state->props.on_delete();
                return true;
            }
            // Enter: "edit" the focused setting (just toggle bools / no-op strings
            // — real editing done via the character events below).
            if (event == Event::Return && state->props.on_update_config) {
                state->props.on_update_config(*state->cfg_patch);
                return true;
            }
        }

        return false;
    });
}

/// Factory — exposed so DialogRouter can instantiate with a server name.
[[nodiscard]] inline Component MakeMcpServerDetails(std::string server_name,
                                                     ServerDetailsProps props = {}) {
    props.server_name = std::move(server_name);
    return McpServerDetails(std::move(props));
}

} // namespace cc::ui::mcp
