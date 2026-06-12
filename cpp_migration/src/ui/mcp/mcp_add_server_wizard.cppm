/// @file mcp_add_server_wizard.cppm
/// @brief 3-step wizard for adding an MCP server.
///   Step 1: Choose server type (local stdio / SSE / WebSocket / marketplace).
///   Step 2: Configure fields based on the chosen type (OAuth branch included).
///   Step 3: Live probe — start a temp client, show tools/resources/prompts count,
///           green success list or red error with Retry/Back.
///
/// Engine delegation:
///   - cc.services.mcp.config.ConfigLoader::save_server  (persist step 3 → disk)
///   - cc.services.mcp.auth                              (OAuth flow when chosen)
///   - cc.services.oauth.client                          (browser launch + token)
///   - cc.services.mcp.connection_manager                (temp probe in step 3)
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
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <format>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.mcp.mcp_add_server_wizard;

import cc.services.mcp.types;
import cc.services.mcp.config;
import cc.services.mcp.auth;
import cc.services.oauth.types;

export namespace cc::ui::mcp {
using namespace ftxui;
using namespace cc::services::mcp;

// ============================================================
// Step / type definitions
// ============================================================

enum class WizardStep : std::uint8_t { Step1 = 0, Step2 = 1, Step3 = 2 };

enum class ServerType : std::uint8_t {
    LocalStdio = 0,
    RemoteSSE  = 1,
    RemoteWS   = 2,
    Marketplace= 3,
};

enum class AuthMethod : std::uint8_t {
    None = 0,
    BearerToken = 1,
    ApiKey = 2,
    OAuth = 3,
};

/// Aggregated form state — populated step by step, persisted at Step 3 `Save`.
struct WizardForm {
    // --- step 1 ---
    ServerType type = ServerType::LocalStdio;

    // --- step 2: Local stdio ---
    std::string command;              // e.g. "npx" or "/usr/bin/python3"
    std::vector<std::string> args;    // split args for the command
    std::string cwd;                  // default "" => project root
    std::vector<std::pair<std::string, std::string>> env;  // KEY=VALUE pairs

    // --- step 2: Remote (SSE / WS) ---
    std::string url;
    AuthMethod auth = AuthMethod::None;
    std::string bearer_token;
    std::string api_key_header;       // e.g. "x-api-key"
    std::string api_key_value;
    // OAuth fields:
    std::string oauth_server_url;
    std::string oauth_client_id;
    std::string oauth_scope;
    bool oauth_flow_in_progress = false;
    std::optional<std::string> oauth_flow_error;
    std::optional<std::string> oauth_token_preview;

    // --- step 2: Marketplace ---
    std::string marketplace_package_id;
    std::string marketplace_version;

    // --- shared ---
    std::string display_name;          // final server name (derived or typed)
};

// Step 3 probe result.
enum class ProbeStatus { Pending, Success, Failed, Running };
struct ProbeResult {
    ProbeStatus status = ProbeStatus::Pending;
    int tool_count = 0;
    int resource_count = 0;
    int prompt_count = 0;
    std::string server_version;
    std::string error_message;
    std::chrono::steady_clock::duration elapsed{};
};

struct WizardProps {
    // Engine hooks — wiring.
    std::function<void(const std::string& url)> on_launch_browser;   // OAuth step
    std::function<ProbeResult(const WizardForm&)> on_probe_server;  // Step 3 temp connect
    std::function<std::expected<void, std::string>(const WizardForm&)> on_save_server;
    std::function<void(const std::string& pkg_id)> on_install_marketplace;

    // Completion / navigation.
    std::function<void()> on_cancel;
    std::function<void(const std::string& server_name)> on_done;

    // Optional: pre-fill for the "re-edit server" case.
    std::optional<WizardForm> initial;
};

// ============================================================
// Rendering helpers
// ============================================================

[[nodiscard]] inline Element RenderBreadcrumb(WizardStep step) {
    auto pill = [](int n, std::string_view label, bool active, bool done) {
        Color c = done ? Color::Green : (active ? Color::Cyan : Color::GrayDark);
        auto el = hbox({
            text(std::format(" {}. ", n)) | bold | color(c),
            text(std::string{label}) | color(c) | (active ? bold : nothing),
        });
        if (active) el = el | bgcolor(Color::RGB(20, 30, 50)) | border;
        return el;
    };
    auto step1 = pill(1, "Select type", step == WizardStep::Step1, step > WizardStep::Step1);
    auto step2 = pill(2, "Configure",  step == WizardStep::Step2, step > WizardStep::Step2);
    auto step3 = pill(3, "Test & Save", step == WizardStep::Step3, false);
    return hbox({step1, text(" → "), step2, text(" → "), step3});
}

[[nodiscard]] inline Element RenderFooter(bool can_go_back, bool can_go_next,
                                          WizardStep step) {
    // Show [Back] [Next/Cancel/Save] according to current step + state.
    auto back = hbox({
        text(" [") | dim,
        text(can_go_back ? "B" : "·") | bold | (can_go_back ? color(Color::Cyan) : dim),
        text("] Back") | (can_go_back ? dim : color(Color::GrayDark)),
    });
    std::string next_label;
    Color next_color = Color::Green;
    if (step == WizardStep::Step3) { next_label = "Save"; next_color = Color::Green; }
    else                           { next_label = "Next"; next_color = Color::Cyan; }
    auto next = hbox({
        text(" [") | dim,
        text(can_go_next ? "N" : "·") | bold | color(can_go_next ? next_color : Color::GrayDark),
        text("] " + next_label) | color(can_go_next ? next_color : Color::GrayDark),
    });
    auto cancel = hbox({
        text(" [Esc]") | dim | color(Color::GrayLight),
        text(" Cancel") | dim,
    });
    return hbox({back, text("  "), next, filler(), cancel});
}

// ---------- Step 1 ----------

[[nodiscard]] inline Element RenderStep1(int selected_type) {
    auto radio = [](int idx, int sel, std::string_view icon,
                    std::string_view title, std::string_view desc) {
        bool active = (idx == sel);
        auto row = vbox({
            hbox({
                text(active ? "◉ " : "○ ") | color(active ? Color::Cyan : Color::GrayDark),
                text(std::string{icon}) | bold | color(active ? Color::Cyan : Color::White),
                text(" "),
                text(std::string{title}) | (active ? bold : nothing),
                filler(),
            }),
            hbox({
                text("   "),
                paragraph(std::string{desc}) | dim | color(active ? Color::GrayLight : Color::GrayDark),
            }),
        });
        if (active) row = row | bgcolor(Color::RGB(25, 35, 50));
        return row;
    };

    Elements content;
    content.push_back(text(" Choose server type:") | bold);
    content.push_back(text(""));
    content.push_back(radio(0, selected_type, "⟷",
        "Local stdio server",
        "Runs a command on your machine (Node, Python, binary…). "
        "Configure the executable path, args, and environment variables."));
    content.push_back(text(""));
    content.push_back(radio(1, selected_type, "⬈",
        "Remote SSE server (HTTP)",
        "Connect to a server over HTTPS via Server-Sent Events. "
        "Optionally add bearer token, API key, or OAuth."));
    content.push_back(text(""));
    content.push_back(radio(2, selected_type, "⇆",
        "Remote WebSocket server",
        "Streamable HTTP / WebSocket transport for high-frequency servers. "
        "Supports auth token and OAuth."));
    content.push_back(text(""));
    content.push_back(radio(3, selected_type, "🛒",
        "Install from marketplace",
        "One-click install of a known MCP server (the C1 plugin dialog will "
        "resolve dependencies and write the config)."));

    return vbox(content) | yframe;
}

// ---------- Step 2 rendering per type ----------

// Simple field helper: label + value preview (real editing is via keyboard).
[[nodiscard]] inline Element FieldRow(std::string_view label,
                                      const std::string& value,
                                      const std::string& empty_hint,
                                      bool focused) {
    auto shown = value.empty() ? std::string{empty_hint} : value;
    auto val_el = text(shown) | (value.empty() ? dim : nothing) | (focused ? inverted : nothing);
    return vbox({
        hbox({
            text(" " + std::string{label}) | bold,
            filler(),
            val_el | size(WIDTH, GREATER_THAN, 30),
        }),
        separatorLight(),
    });
}

[[nodiscard]] inline Element RenderStep2_LocalStdio(const WizardForm& f, int focus) {
    Elements e;
    e.push_back(text(" Local stdio server configuration") | bold | color(Color::Cyan));
    e.push_back(text(""));
    e.push_back(FieldRow("Server name", f.display_name, "(auto-filled from command)", focus == 0));
    e.push_back(FieldRow("Command",     f.command,      "e.g. npx -y @modelcontextprotocol/server-filesystem", focus == 1));
    e.push_back(FieldRow("Arguments",   [&] {
        std::string j; for (auto& a : f.args) { if (!j.empty()) j += " "; j += a; } return j;
    }(), "(space-separated)", focus == 2));
    e.push_back(FieldRow("Working dir", f.cwd,          "(project root)", focus == 3));

    // Env var preview
    e.push_back(text(""));
    e.push_back(text(" Environment variables") | bold | dim);
    if (f.env.empty()) {
        e.push_back(text("  (none — add with [E]dit env)") | dim);
    } else {
        for (const auto& [k, v] : f.env) {
            e.push_back(hbox({
                text("   " + k + "=") | dim,
                text(std::string(v.size(), '*')) | color(Color::MagentaLight) | dim,
            }));
        }
    }
    return vbox(e);
}

[[nodiscard]] inline bool is_valid_https_url(const std::string& u) {
    static const std::regex rx(R"(^https?://[^\s/$.?#].[^\s]*$)", std::regex::icase);
    return !u.empty() && std::regex_match(u, rx);
}

[[nodiscard]] inline Element RenderStep2_Remote(const WizardForm& f, bool is_ws, int focus) {
    Elements e;
    e.push_back(hbox({
        text(" Remote "),
        text(is_ws ? "WebSocket" : "SSE") | bold | color(Color::Cyan),
        text(" configuration"),
    }) | bold);
    e.push_back(text(""));
    e.push_back(FieldRow("Server name", f.display_name, "(auto-filled from URL)", focus == 0));

    auto url_valid = is_valid_https_url(f.url);
    auto url_row = FieldRow("URL",         f.url, is_ws ? "wss://… or https://…" : "https://…", focus == 1);
    if (!f.url.empty() && !url_valid) {
        e.push_back(url_row);
        e.push_back(text("   ✗ URL must start with http(s):// (or wss://)") | color(Color::Red) | dim);
    } else {
        e.push_back(url_row);
    }

    e.push_back(text(""));
    e.push_back(text(" Authentication") | bold | dim);

    auto auth_radio = [](int i, AuthMethod cur, AuthMethod val,
                        std::string_view label) {
        bool sel = (cur == val);
        return hbox({
            text(std::format(" {} ", i)) | dim,
            text(sel ? "◉ " : "○ ") | color(sel ? Color::Cyan : Color::GrayDark),
            text(std::string{label}) | (sel ? bold : nothing),
        });
    };
    e.push_back(auth_radio(1, f.auth, AuthMethod::None, "No auth"));
    e.push_back(auth_radio(2, f.auth, AuthMethod::BearerToken, "Bearer token"));
    e.push_back(auth_radio(3, f.auth, AuthMethod::ApiKey, "API key (custom header)"));
    e.push_back(auth_radio(4, f.auth, AuthMethod::OAuth, "OAuth 2.0 (authorization code)"));

    if (f.auth == AuthMethod::BearerToken) {
        e.push_back(text(""));
        e.push_back(FieldRow("Token",
            f.bearer_token.empty() ? std::string{} : std::string(f.bearer_token.size(), '*'),
            "paste token", focus == 4));
    } else if (f.auth == AuthMethod::ApiKey) {
        e.push_back(text(""));
        e.push_back(FieldRow("Header name", f.api_key_header, "x-api-key", focus == 4));
        e.push_back(FieldRow("Header value",
            f.api_key_value.empty() ? std::string{} : std::string(f.api_key_value.size(), '*'),
            "paste key", focus == 5));
    } else if (f.auth == AuthMethod::OAuth) {
        e.push_back(text(""));
        e.push_back(FieldRow("Server URL (authorization)", f.oauth_server_url, "https://…/.well-known/oauth-authorization-server", focus == 4));
        e.push_back(FieldRow("Client ID", f.oauth_client_id, "from provider dashboard", focus == 5));
        e.push_back(FieldRow("Scope",     f.oauth_scope, "space separated", focus == 6));
        e.push_back(text(""));

        // Flow status
        if (f.oauth_flow_in_progress) {
            e.push_back(hbox({
                text(" ◐ ") | color(Color::Yellow) | blink,
                text("OAuth flow in progress — check your browser…") | color(Color::Yellow),
            }));
        } else if (f.oauth_token_preview) {
            e.push_back(hbox({
                text(" ✓ ") | color(Color::Green),
                text("OAuth completed — token saved locally") | color(Color::Green),
            }));
        } else if (f.oauth_flow_error) {
            e.push_back(hbox({
                text(" ✗ ") | color(Color::Red),
                paragraph(*f.oauth_flow_error) | color(Color::RedLight),
            }));
        }

        e.push_back(hbox({
            text("   [O]") | color(Color::Cyan) | bold,
            text(" Launch browser ") | dim,
            text("(PKCE, callback on localhost)") | dim,
        }));
    }

    return vbox(e);
}

[[nodiscard]] inline Element RenderStep2_Marketplace(const WizardForm& f, int focus) {
    Elements e;
    e.push_back(text(" Marketplace install") | bold | color(Color::Magenta));
    e.push_back(text(""));
    e.push_back(paragraph(
        "Pick from the curated list of known MCP servers. The C1 plugin "
        "dialog will handle dependency resolution, installation, and config "
        "file writing.") | dim);
    e.push_back(text(""));
    e.push_back(FieldRow("Package id / slug", f.marketplace_package_id,
        "e.g. filesystem, brave-search, postgres", focus == 0));
    e.push_back(FieldRow("Version (optional)", f.marketplace_version, "latest", focus == 1));
    return vbox(e);
}

// ---------- Step 3 ----------

[[nodiscard]] inline Element RenderStep3(const ProbeResult& probe) {
    Elements e;
    e.push_back(text(" Live probe — tool / resource / prompt discovery") | bold | color(Color::Cyan));
    e.push_back(text(""));

    if (probe.status == ProbeStatus::Pending || probe.status == ProbeStatus::Running) {
        e.push_back(hbox({
            text(" ◐ ") | color(Color::Yellow) | blink,
            text("Connecting to the server…") | color(Color::Yellow),
        }));
        e.push_back(text(" (this can take a few seconds while the process starts)") | dim);
        return vbox(e);
    }

    if (probe.status == ProbeStatus::Failed) {
        e.push_back(hbox({
            text(" ✗ ") | color(Color::Red),
            text("Connection failed") | color(Color::Red) | bold,
        }));
        e.push_back(separatorLight());
        e.push_back(paragraph("  " + probe.error_message) | color(Color::RedLight));
        e.push_back(text(""));
        e.push_back(hbox({
            text(" [T]") | color(Color::Yellow),
            text("ry again    ") | dim,
            text("[B]") | color(Color::Cyan),
            text("ack to configure") | dim,
        }));
        return vbox(e);
    }

    // Success
    e.push_back(hbox({
        text(" ✓ ") | color(Color::Green),
        text("Server is reachable") | color(Color::Green) | bold,
    }));
    if (!probe.server_version.empty()) {
        e.push_back(text("   server version: " + probe.server_version) | dim);
    }
    e.push_back(text(""));
    e.push_back(hbox({
        text("   ✓ ") | color(Color::Green),
        text(std::format("{} tools", probe.tool_count)) | bold,
    }));
    e.push_back(hbox({
        text("   ✓ ") | color(Color::Green),
        text(std::format("{} resources", probe.resource_count)) | bold,
    }));
    e.push_back(hbox({
        text("   ✓ ") | color(Color::Green),
        text(std::format("{} prompts", probe.prompt_count)) | bold,
    }));
    e.push_back(text(""));
    e.push_back(paragraph("Press [S] or [N] to save this server to your config.") | color(Color::Green));

    return vbox(e);
}

// ============================================================
// Validation helpers
// ============================================================

[[nodiscard]] inline bool CanAdvanceStep1(ServerType) { return true; }

[[nodiscard]] inline bool CanAdvanceStep2(const WizardForm& f) {
    switch (f.type) {
        case ServerType::LocalStdio:
            return !f.command.empty();
        case ServerType::RemoteSSE:
        case ServerType::RemoteWS: {
            if (!is_valid_https_url(f.url)) return false;
            if (f.auth == AuthMethod::BearerToken && f.bearer_token.empty()) return false;
            if (f.auth == AuthMethod::ApiKey &&
                (f.api_key_header.empty() || f.api_key_value.empty())) return false;
            if (f.auth == AuthMethod::OAuth && !f.oauth_token_preview)
                return false; // must complete OAuth first
            return true;
        }
        case ServerType::Marketplace:
            return !f.marketplace_package_id.empty();
    }
    return false;
}

[[nodiscard]] inline bool CanSave(const ProbeResult& p) {
    return p.status == ProbeStatus::Success;
}

// ============================================================
// Interactive wizard component
// ============================================================

[[nodiscard]] inline Component McpAddServerWizard(WizardProps props) {
    struct State {
        WizardProps props;
        WizardForm form;
        WizardStep step = WizardStep::Step1;

        // UI-level focus indices (which field is being edited).
        int step1_focus = 0;    // server type radio idx
        int step2_focus = 0;    // field idx within current type
        int step3_focus = 0;

        // Step 3 probe state
        ProbeResult probe;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);
    if (state->props.initial) state->form = *state->props.initial;

    constexpr int kStep1TypeCount = 4;

    auto apply_step1_selection = [state] {
        state->form.type = static_cast<ServerType>(state->step1_focus);
    };

    // Derive a reasonable display name from current form if empty.
    auto ensure_display_name = [state] {
        if (!state->form.display_name.empty()) return;
        switch (state->form.type) {
            case ServerType::LocalStdio: {
                auto& c = state->form.command;
                if (!c.empty()) {
                    auto slash = c.find_last_of("/\\");
                    state->form.display_name =
                        (slash == std::string::npos) ? c : c.substr(slash + 1);
                }
                break;
            }
            case ServerType::RemoteSSE:
            case ServerType::RemoteWS: {
                std::string host = state->form.url;
                auto proto = host.find("://");
                if (proto != std::string::npos) host = host.substr(proto + 3);
                auto slash = host.find('/');
                if (slash != std::string::npos) host = host.substr(0, slash);
                state->form.display_name = host;
                break;
            }
            case ServerType::Marketplace:
                state->form.display_name = state->form.marketplace_package_id;
                break;
        }
    };

    auto run_probe = [state] {
        state->probe.status = ProbeStatus::Running;
        if (state->props.on_probe_server) {
            state->probe = state->props.on_probe_server(state->form);
        } else {
            state->probe.status = ProbeStatus::Success;
            state->probe.tool_count = 0;
        }
    };

    auto step2_field_count = [state]() -> int {
        switch (state->form.type) {
            case ServerType::LocalStdio: return 4; // name,cmd,args,cwd
            case ServerType::RemoteSSE:
            case ServerType::RemoteWS: {
                switch (state->form.auth) {
                    case AuthMethod::None:        return 2;
                    case AuthMethod::BearerToken: return 5;
                    case AuthMethod::ApiKey:      return 6;
                    case AuthMethod::OAuth:       return 7;
                }
                return 2;
            }
            case ServerType::Marketplace: return 2;
        }
        return 2;
    };

    auto current_field_string_ref = [state](std::string*& out, bool read_only = false) -> bool {
        // Returns a mutable string ref pointer if current step2_focus points at
        // a free-text field.  Returns false for non-editable position.
        switch (state->form.type) {
            case ServerType::LocalStdio:
                switch (state->step2_focus) {
                    case 0: out = &state->form.display_name; return true;
                    case 1: out = &state->form.command;      return true;
                    case 2: {
                        // Special: we join args and split on space on exit.
                        static std::string buf;  // NOLINT (local to call path)
                        buf.clear();
                        for (auto& a : state->form.args) {
                            if (!buf.empty()) buf += ' ';
                            buf += a;
                        }
                        if (!read_only) out = &buf;
                        return true;
                    }
                    case 3: out = &state->form.cwd; return true;
                }
                return false;
            case ServerType::RemoteSSE:
            case ServerType::RemoteWS:
                switch (state->step2_focus) {
                    case 0: out = &state->form.display_name;    return true;
                    case 1: out = &state->form.url;             return true;
                    case 4: out = (state->form.auth == AuthMethod::BearerToken)
                                  ? &state->form.bearer_token
                                  : (state->form.auth == AuthMethod::ApiKey)
                                      ? &state->form.api_key_header
                                      : &state->form.oauth_server_url;
                        return true;
                    case 5: out = (state->form.auth == AuthMethod::ApiKey)
                                  ? &state->form.api_key_value
                                  : (state->form.auth == AuthMethod::OAuth)
                                      ? &state->form.oauth_client_id
                                      : nullptr;
                        return out != nullptr;
                    case 6: out = &state->form.oauth_scope;
                        return state->form.auth == AuthMethod::OAuth;
                }
                return false;
            case ServerType::Marketplace:
                switch (state->step2_focus) {
                    case 0: out = &state->form.marketplace_package_id; return true;
                    case 1: out = &state->form.marketplace_version;    return true;
                }
                return false;
        }
        return false;
    };

    // Flush the join buffer back into args after editing field 2 of LocalStdio.
    auto flush_args_buf = [state](const std::string& buf) {
        state->form.args.clear();
        std::string cur;
        std::istringstream iss(buf);
        while (iss >> cur) state->form.args.push_back(std::move(cur));
    };

    return Renderer([state] {
        Element body;
        switch (state->step) {
            case WizardStep::Step1:
                body = RenderStep1(state->step1_focus);
                break;
            case WizardStep::Step2:
                switch (state->form.type) {
                    case ServerType::LocalStdio:
                        body = RenderStep2_LocalStdio(state->form, state->step2_focus);
                        break;
                    case ServerType::RemoteSSE:
                        body = RenderStep2_Remote(state->form, /*is_ws=*/false, state->step2_focus);
                        break;
                    case ServerType::RemoteWS:
                        body = RenderStep2_Remote(state->form, /*is_ws=*/true, state->step2_focus);
                        break;
                    case ServerType::Marketplace:
                        body = RenderStep2_Marketplace(state->form, state->step2_focus);
                        break;
                }
                break;
            case WizardStep::Step3:
                body = RenderStep3(state->probe);
                break;
        }

        auto crumb = RenderBreadcrumb(state->step);
        bool can_back = (state->step != WizardStep::Step1);
        bool can_next = false;
        if (state->step == WizardStep::Step1) can_next = CanAdvanceStep1(state->form.type);
        else if (state->step == WizardStep::Step2) can_next = CanAdvanceStep2(state->form);
        else if (state->step == WizardStep::Step3) can_next = CanSave(state->probe);

        auto footer = RenderFooter(can_back, can_next, state->step);

        return window(
            text(" ➕ Add MCP Server ") | bold | color(Color::Cyan),
            vbox({
                crumb | size(HEIGHT, EQUAL, 3),
                separator(),
                body | flex,
                separator(),
                footer,
            }) | xflex
        ) | color(Color::Cyan) | size(WIDTH, LESS_THAN, 80);
    }) | CatchEvent([state, apply_step1_selection, ensure_display_name,
                      run_probe, step2_field_count, current_field_string_ref,
                      flush_args_buf](Event event) -> bool {

        // ---------- Global: cancel ----------
        if (event == Event::Escape) {
            if (state->props.on_cancel) state->props.on_cancel();
            return true;
        }

        // ---------- Step-specific event handling ----------

        if (state->step == WizardStep::Step1) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->step1_focus = std::max(0, state->step1_focus - 1);
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->step1_focus =
                    std::min(kStep1TypeCount - 1, state->step1_focus + 1);
                return true;
            }
            if (event == Event::Character('1') || event == Event::Character('2') ||
                event == Event::Character('3') || event == Event::Character('4')) {
                state->step1_focus = (event.character()[0] - '1');
                return true;
            }
            if (event == Event::Return || event == Event::Character('n') ||
                event == Event::Character('N')) {
                apply_step1_selection();
                ensure_display_name();
                state->step2_focus = 0;
                state->step = WizardStep::Step2;
                return true;
            }
            if (event == Event::Tab) {
                state->step1_focus = (state->step1_focus + 1) % kStep1TypeCount;
                return true;
            }
            return false;
        }

        if (state->step == WizardStep::Step2) {

            // Field navigation
            int n = step2_field_count();
            if (event == Event::ArrowDown || event == Event::Tab) {
                state->step2_focus = std::min(n - 1, state->step2_focus + 1);
                return true;
            }
            if (event == Event::ArrowUp || event == Event::TabReverse) {
                state->step2_focus = std::max(0, state->step2_focus - 1);
                return true;
            }

            // Auth method hotkeys: 1-4
            if ((state->form.type == ServerType::RemoteSSE ||
                 state->form.type == ServerType::RemoteWS) &&
                (event == Event::Character('1') || event == Event::Character('2') ||
                 event == Event::Character('3') || event == Event::Character('4'))) {
                // Only apply when focus isn't inside a text field (heuristic:
                // press after step2_focus is 0-1 or the user is idle on the
                // auth section). For safety, always treat as selection.
                state->form.auth = static_cast<AuthMethod>(
                    event.character()[0] - '1');
                return true;
            }

            // OAuth: Launch browser (O)
            if (event == Event::Character('o') || event == Event::Character('O')) {
                if (state->form.auth == AuthMethod::OAuth &&
                    state->props.on_launch_browser &&
                    !state->form.oauth_server_url.empty()) {
                    state->form.oauth_flow_in_progress = true;
                    state->form.oauth_flow_error.reset();
                    state->props.on_launch_browser(state->form.oauth_server_url);
                }
                return true;
            }

            // Generic character input for the focused editable field.
            if (event.is_character()) {
                std::string* field = nullptr;
                if (current_field_string_ref(field)) {
                    *field += event.character();
                    if (state->form.type == ServerType::LocalStdio &&
                        state->step2_focus == 2) {
                        // The buf is local static; flush.
                        std::string* b = nullptr;
                        current_field_string_ref(b);
                        if (b) flush_args_buf(*b);
                    }
                    if (state->step2_focus == 1 && (
                            state->form.type == ServerType::LocalStdio ||
                            state->form.type == ServerType::RemoteSSE ||
                            state->form.type == ServerType::RemoteWS)) {
                        ensure_display_name();
                    }
                    return true;
                }
            }
            if (event == Event::Backspace) {
                std::string* field = nullptr;
                if (current_field_string_ref(field) && !field->empty()) {
                    field->pop_back();
                    if (state->form.type == ServerType::LocalStdio &&
                        state->step2_focus == 2) {
                        std::string* b = nullptr;
                        current_field_string_ref(b);
                        if (b) flush_args_buf(*b);
                    }
                    return true;
                }
                // Fall-through: backspace also acts as "go back" when on first
                // empty field? No — let Esc/Back handle it.
                return false;
            }

            // Go back (B / Esc on first field)
            if (event == Event::Character('b') || event == Event::Character('B')) {
                state->step = WizardStep::Step1;
                return true;
            }

            // Advance (Enter / N)
            if (event == Event::Return || event == Event::Character('n') ||
                event == Event::Character('N')) {
                if (CanAdvanceStep2(state->form)) {
                    ensure_display_name();
                    // Marketplace shortcut: install instead of probe
                    if (state->form.type == ServerType::Marketplace &&
                        state->props.on_install_marketplace) {
                        state->props.on_install_marketplace(state->form.marketplace_package_id);
                        if (state->props.on_done)
                            state->props.on_done(state->form.display_name);
                        return true;
                    }
                    state->step = WizardStep::Step3;
                    run_probe();
                    return true;
                }
                // Shake / no-op — can't advance.
                return true;
            }
            return false;
        }

        if (state->step == WizardStep::Step3) {
            // Retry probe
            if (event == Event::Character('t') || event == Event::Character('T')) {
                run_probe();
                return true;
            }
            // Back to config
            if (event == Event::Character('b') || event == Event::Character('B')) {
                state->step = WizardStep::Step2;
                return true;
            }
            // Save / accept
            if (event == Event::Return || event == Event::Character('n') ||
                event == Event::Character('N') || event == Event::Character('s') ||
                event == Event::Character('S')) {
                if (CanSave(state->probe)) {
                    if (state->props.on_save_server) {
                        auto r = state->props.on_save_server(state->form);
                        if (!r) {
                            state->probe.status = ProbeStatus::Failed;
                            state->probe.error_message =
                                std::string("Failed to save config: ") + r.error();
                            return true;
                        }
                    }
                    if (state->props.on_done)
                        state->props.on_done(state->form.display_name);
                    return true;
                }
                return true;
            }
            return false;
        }

        return false;
    });
}

/// Convenience factory for DialogRouter registration.
[[nodiscard]] inline Component MakeMcpAddServerWizard(WizardProps props = {}) {
    return McpAddServerWizard(std::move(props));
}

} // namespace cc::ui::mcp
