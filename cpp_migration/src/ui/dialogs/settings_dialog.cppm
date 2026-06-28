/// @file settings_dialog.cppm
/// @brief Settings interface with tabbed navigation (General, Model, API,
/// Permissions, Tools, MCP, LSP, Bridge, Hooks, Privacy, About, Status, Usage).
/// Migrated from Settings.tsx, Config.tsx, Status.tsx. Config read/write is
/// delegated 100% to ConfigManager (cc.config.config) — no direct JSON I/O.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <variant>
#include <algorithm>
#include <expected>
#include <chrono>
#include <array>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.settings_dialog;

import cc.types.types;
import cc.config.config;
import cc.ui.custom_select;

export namespace cc::ui::dialogs::settings_dialog {
using namespace ftxui;

// Check whether `e` is a Ctrl+<letter> combination.
// Control characters in ASCII are 1..26 = letter - 'a' + 1 (case insensitive).
inline bool is_ctrl_key(const Event& e, char letter) {
    char code = (letter >= 'a' && letter <= 'z') ? (letter - 'a' + 1)
                : (letter >= 'A' && letter <= 'Z') ? (letter - 'A' + 1) : 0;
    return code != 0 && e.input().size() == 1 &&
           static_cast<unsigned char>(e.input()[0]) ==
               static_cast<unsigned char>(code);
}
using cc::core::ConfigManager;
using cc::core::Settings;
using cc::core::FeatureFlag;
using cc::core::McpServerConfig;
using custom_select::SelectOption;

// ============================================================
// Types
// ============================================================

/// Top-level tabs visible in the left sidebar
enum class SettingsTabId : std::uint8_t {
    General = 0,
    Model,
    API,
    Permissions,
    Tools,
    MCP,
    LSP,
    Bridge,
    Hooks,
    Privacy,
    About,
    Status,
    Usage,
    _COUNT,
};

/// Convert a tab enum to its sidebar label
[[nodiscard]] inline std::string tab_label(SettingsTabId id) {
    switch (id) {
        case SettingsTabId::General:     return "General";
        case SettingsTabId::Model:       return "Model";
        case SettingsTabId::API:         return "API";
        case SettingsTabId::Permissions: return "Permissions";
        case SettingsTabId::Tools:       return "Tools";
        case SettingsTabId::MCP:         return "MCP";
        case SettingsTabId::LSP:         return "LSP";
        case SettingsTabId::Bridge:      return "Bridge";
        case SettingsTabId::Hooks:       return "Hooks";
        case SettingsTabId::Privacy:     return "Privacy";
        case SettingsTabId::About:       return "About";
        case SettingsTabId::Status:      return "Status";
        case SettingsTabId::Usage:       return "Usage";
        case SettingsTabId::_COUNT:      return "?";
    }
    return "?";
}

/// Default permission mode used by the Permissions tab
enum class DefaultPermissionMode : std::uint8_t {
    Ask,
    Allow,
    Deny,
};

[[nodiscard]] inline std::string perm_mode_str(DefaultPermissionMode m) {
    switch (m) {
        case DefaultPermissionMode::Ask:   return "Ask";
        case DefaultPermissionMode::Allow: return "Allow";
        case DefaultPermissionMode::Deny:  return "Deny";
    }
    return "Ask";
}

/// Subsystem health state shown on the Status tab
enum class HealthState : std::uint8_t {
    OK,
    Degraded,
    Down,
};

/// One row in the Status tab health listing
struct SubsystemHealth {
    std::string name;
    HealthState state;
    std::chrono::system_clock::time_point last_check;
    std::string message;
};

/// Command result display mode (kept for on_close signature parity)
enum class CommandResultDisplay {
    normal,
    system,
    skip,
};

/// Toast — transient short message shown at bottom after save
struct ToastState {
    std::string message;
    Color color = Color::Green;
    std::chrono::steady_clock::time_point until;
    [[nodiscard]] bool active() const {
        return !message.empty() &&
               std::chrono::steady_clock::now() < until;
    }
};

/// Per-model usage row for the Usage tab
struct ModelUsageRow {
    std::string model_name;
    std::uint64_t requests = 0;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    double cost_usd = 0.0;
};

/// Aggregated usage snapshot
struct UsageSnapshot {
    double total_cost_usd = 0.0;
    std::uint64_t total_input_tokens = 0;
    std::uint64_t total_output_tokens = 0;
    std::uint64_t total_requests = 0;
    double daily_limit_pct = 0.0;    // 0..1
    double rate_limit_pct = 0.0;     // 0..1
    std::vector<ModelUsageRow> per_model;
};

/// Options / props for the settings dialog
struct SettingsDialogOptions {
    /// Called when the dialog is dismissed
    std::function<void(std::optional<std::string>, CommandResultDisplay)> on_close;
    /// Initial tab
    SettingsTabId initial_tab = SettingsTabId::General;
    /// Pre-loaded status diagnostics (optional — can be empty)
    std::vector<SubsystemHealth> status_rows;
    /// Pre-loaded usage snapshot (optional — can be empty)
    std::function<void(UsageSnapshot&)> refresh_usage;
};

// ============================================================
// Helpers — health / toast
// ============================================================

[[nodiscard]] inline std::pair<std::string, Color> health_display(HealthState s) {
    switch (s) {
        case HealthState::OK:       return {"●", Color::Green};
        case HealthState::Degraded: return {"●", Color::Yellow};
        case HealthState::Down:     return {"●", Color::Red};
    }
    return {"?", Color::White};
}

[[nodiscard]] inline std::string format_ts(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

// ============================================================
// Individual setting row builder
// ============================================================

/// Build a standard setting row: label | control | description.
/// Each row takes label text, a control Element, an optional description
/// (hint text rendered in dim gray below the row).
[[nodiscard]] inline Element SettingRow(
    const std::string& label,
    Element control,
    const std::optional<std::string>& description = std::nullopt,
    bool disabled = false,
    bool is_modified = false) {

    auto label_el = text(" " + label) | (disabled ? dim : nothing);
    auto mod_mark = is_modified ? text("*") | color(Color::Yellow) : text(" ");
    auto row = hbox({
        mod_mark,
        label_el | size(WIDTH, EQUAL, 28) | flex_shrink,
        std::move(control) | flex,
        text(" "),
    });

    Elements result;
    result.push_back(row);
    if (description && !description->empty()) {
        result.push_back(hbox({
            text("   ") | dim,
            text(*description) | dim | color(Color::GrayLight),
        }));
    }
    return vbox(result);
}

// ============================================================
// Tab: General
// ============================================================

/// In-memory copy of settings being edited. We read from ConfigManager on
/// construction, mutate this working copy while the user edits, and write
/// back only on Save / Reset.
struct WorkingSettings {
    // General / Display
    std::string theme = "dark";
    bool auto_mode = false;
    std::string effort_level = "medium";
    bool fast_mode = false;
    double default_temperature = 0.7;
    bool show_thinking = true;
    bool show_token_usage = false;
    bool compact_mode = false;

    // Model
    std::string default_model = "claude-sonnet-4-20250514";
    std::uint32_t max_output_tokens = 16384;
    bool extended_thinking = false;
    std::optional<std::uint32_t> thinking_budget;
    std::uint32_t context_window_size = 200000;
    std::string teammate_model;   // swarm teammate model override

    // API
    std::optional<std::string> base_url;
    std::optional<std::string> api_key;
    std::uint32_t timeout_seconds = 120;
    bool verify_ssl = true;
    std::optional<std::string> proxy;
    std::uint32_t max_retries = 3;

    // Permissions
    DefaultPermissionMode default_perm = DefaultPermissionMode::Ask;
    bool allow_bash = true;
    bool allow_file_write = true;
    bool allow_network = true;
    std::vector<std::string> allowed_paths;
    std::vector<std::string> denied_paths;
    std::vector<std::string> allowed_commands;

    // Tools
    bool enable_agent_tool = true;
    bool enable_web_fetch = true;
    bool enable_web_search = true;
    bool enable_bash_granular = true;
    bool enable_glob_grep_safety = true;
    bool enable_skill_loading = true;

    // MCP
    std::vector<McpServerConfig> mcp_servers;
    bool auto_start_mcp = true;

    // LSP
    bool enable_lsp = true;
    std::uint32_t lsp_timeout = 60;

    // Bridge
    bool enable_bridge = false;
    bool bridge_outbound_only = false;
    std::uint32_t bridge_port = 37246;
    std::string bridge_jwt_secret;       // JWT secret (display only)
    std::vector<std::string> bridge_allowed_origins;
};

/// Copy effective values from ConfigManager into a working set
[[nodiscard]] inline WorkingSettings snapshot_from(const ConfigManager& cfg) {
    WorkingSettings w;
    const auto& s = cfg.settings();
    w.theme = s.display.theme.empty() ? std::string("auto") : s.display.theme;
    w.show_thinking = s.display.show_thinking;
    w.show_token_usage = s.display.show_token_usage;
    w.compact_mode = s.display.compact_mode;
    w.default_model = s.model.default_model;
    w.max_output_tokens = s.model.max_output_tokens;
    w.extended_thinking = s.model.extended_thinking;
    w.default_temperature = s.model.temperature.value_or(0.7);
    w.thinking_budget = s.model.thinking_budget;
    w.context_window_size = s.model.context_window_size;
    w.base_url = s.network.base_url;
    w.api_key = s.network.api_key;
    w.timeout_seconds = s.network.timeout_seconds;
    w.verify_ssl = s.network.verify_ssl;
    w.proxy = s.network.proxy;
    w.max_retries = s.network.max_retries;
    w.allow_bash = s.permissions.allow_bash;
    w.allow_file_write = s.permissions.allow_file_write;
    w.allow_network = s.permissions.allow_network;
    w.allowed_paths = s.permissions.allowed_paths;
    w.denied_paths = s.permissions.denied_paths;
    w.allowed_commands = s.permissions.allowed_commands;
    w.mcp_servers = s.mcp_servers;
    w.enable_skill_loading = cfg.is_feature_enabled(FeatureFlag::SkillSystem);
    return w;
}

/// Write working copy back to ConfigManager
inline void apply_to(const WorkingSettings& w, ConfigManager& cfg) {
    auto& s = cfg.settings_mut();
    s.display.theme = w.theme;
    s.display.show_thinking = w.show_thinking;
    s.display.show_token_usage = w.show_token_usage;
    s.display.compact_mode = w.compact_mode;
    s.model.default_model = w.default_model;
    s.model.max_output_tokens = w.max_output_tokens;
    s.model.extended_thinking = w.extended_thinking;
    s.model.temperature = w.default_temperature;
    s.model.thinking_budget = w.thinking_budget;
    s.model.context_window_size = w.context_window_size;
    s.network.base_url = w.base_url;
    s.network.api_key = w.api_key;
    s.network.timeout_seconds = w.timeout_seconds;
    s.network.verify_ssl = w.verify_ssl;
    s.network.proxy = w.proxy;
    s.network.max_retries = w.max_retries;
    s.permissions.allow_bash = w.allow_bash;
    s.permissions.allow_file_write = w.allow_file_write;
    s.permissions.allow_network = w.allow_network;
    s.permissions.allowed_paths = w.allowed_paths;
    s.permissions.denied_paths = w.denied_paths;
    s.permissions.allowed_commands = w.allowed_commands;
    s.mcp_servers = w.mcp_servers;
    cfg.set_feature(FeatureFlag::SkillSystem, w.enable_skill_loading);
}

// ============================================================
// Rendering: tab contents
// ============================================================

[[nodiscard]] inline Element RenderTabHeader(const std::string& title,
                                             const std::string& subtitle = {}) {
    Elements els;
    els.push_back(hbox({
        text(" " + title + " ") | bold | color(Color::Cyan),
        filler(),
    }));
    if (!subtitle.empty()) {
        els.push_back(hbox({
            text(" "),
            text(subtitle) | dim | color(Color::GrayLight),
        }));
    }
    els.push_back(separator());
    return vbox(els);
}

// --- General tab ---
[[nodiscard]] inline Element RenderGeneralTab(const WorkingSettings& w,
                                              int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("General", "Theme, behaviour & defaults"));

    // Theme dropdown display
    body.push_back(SettingRow(
        "Theme",
        text(" [" + w.theme + "] ") | color(Color::Yellow) |
             (focus_row == 0 ? inverted : nothing),
        "UI color scheme (dark / light / high-contrast / auto)",
        false, focus_row == -2));

    // Auto mode
    body.push_back(SettingRow(
        "Auto mode",
        text(w.auto_mode ? " [ON]  " : " [OFF] ") |
             color(w.auto_mode ? Color::Green : Color::GrayDark) |
             (focus_row == 1 ? inverted : nothing),
        "Automatically continue tool-use chains without confirmations",
        false, focus_row == -2));

    // Effort level
    body.push_back(SettingRow(
        "Effort level",
        text(" [" + w.effort_level + "] ") | color(Color::Yellow) |
             (focus_row == 2 ? inverted : nothing),
        "Reasoning effort: low / medium / high",
        false, focus_row == -2));

    // Fast mode
    body.push_back(SettingRow(
        "Fast mode",
        text(w.fast_mode ? " [ON]  " : " [OFF] ") |
             color(w.fast_mode ? Color::Green : Color::GrayDark) |
             (focus_row == 3 ? inverted : nothing),
        "Use the faster (cheaper) model when available",
        false, focus_row == -2));

    // Temperature (slider display)
    body.push_back(SettingRow(
        "Default temperature",
        hbox({
            gauge(w.default_temperature / 2.0) | color(Color::Cyan) |
                size(WIDTH, EQUAL, 20),
            text(std::format(" {:.2f} ", w.default_temperature)) |
                color(Color::Cyan) | (focus_row == 4 ? inverted : nothing),
        }),
        "Sampling temperature (0 = deterministic, 2 = maximum randomness)",
        false, focus_row == -2));

    body.push_back(separator());
    body.push_back(text("  Display options:") | dim);
    body.push_back(SettingRow(
        "Show thinking",
        text(w.show_thinking ? " [ON]  " : " [OFF] ") |
             color(w.show_thinking ? Color::Green : Color::GrayDark) |
             (focus_row == 5 ? inverted : nothing),
        "Display thinking blocks in assistant output",
        false, false));
    body.push_back(SettingRow(
        "Token usage display",
        text(w.show_token_usage ? " [ON]  " : " [OFF] ") |
             color(w.show_token_usage ? Color::Green : Color::GrayDark) |
             (focus_row == 6 ? inverted : nothing),
        "Show token counters after each response",
        false, false));
    body.push_back(SettingRow(
        "Compact mode",
        text(w.compact_mode ? " [ON]  " : " [OFF] ") |
             color(w.compact_mode ? Color::Green : Color::GrayDark) |
             (focus_row == 7 ? inverted : nothing),
        "Minimal output formatting for smaller terminals",
        false, false));

    return vbox(body);
}

// --- Model tab ---
[[nodiscard]] inline Element RenderModelTab(const WorkingSettings& w,
                                            int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("Model", "Default model and generation limits"));

    body.push_back(SettingRow(
        "Default model",
        hbox({
            text(" ["),
            text(w.default_model) | color(Color::Cyan),
            text("] ▾ ") | dim,
        }) | (focus_row == 0 ? inverted : nothing),
        "Model used for new sessions",
        false, false));

    body.push_back(SettingRow(
        "Max output tokens",
        text(std::format(" {} ", w.max_output_tokens)) | color(Color::Cyan) |
             (focus_row == 1 ? inverted : nothing),
        "Maximum tokens per assistant turn",
        false, false));

    body.push_back(SettingRow(
        "Extended thinking",
        text(w.extended_thinking ? " [ON]  " : " [OFF] ") |
             color(w.extended_thinking ? Color::Green : Color::GrayDark) |
             (focus_row == 2 ? inverted : nothing),
        "Allocate additional tokens for chain-of-thought reasoning",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "Thinking budget",
        text(w.thinking_budget
            ? std::format(" {} tokens ", *w.thinking_budget)
            : std::string(" (auto) ")) | color(Color::Cyan) |
             (focus_row == 3 ? inverted : nothing),
        "Max tokens allocated for chain-of-thought (blank = automatic)",
        false, false));
    body.push_back(SettingRow(
        "Context window size",
        text(std::format(" {} ", w.context_window_size)) | color(Color::Cyan) |
             (focus_row == 4 ? inverted : nothing),
        "Maximum context window tokens",
        false, false));
    body.push_back(SettingRow(
        "Teammate model",
        text(w.teammate_model.empty()
            ? std::string(" (same as default) ")
            : " [" + w.teammate_model + "] ") | color(Color::Yellow) |
             (focus_row == 5 ? inverted : nothing),
        "Override model for sub-agent / swarm teammates",
        false, false));
    return vbox(body);
}

// --- API tab ---
[[nodiscard]] inline Element RenderAPITab(const WorkingSettings& w,
                                          int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("API", "Endpoints, authentication and timeouts"));

    // Base URL
    std::string url_display = w.base_url.value_or("(default: api.anthropic.com)");
    body.push_back(SettingRow(
        "Base URL",
        text(" " + url_display + " ") | color(Color::Cyan) |
             (focus_row == 0 ? inverted : nothing),
        "Override the API endpoint (for proxies, self-hosted, etc.)",
        false, false));

    // API key — masked as ***
    std::string key_display;
    if (w.api_key && !w.api_key->empty()) {
        auto k = *w.api_key;
        if (k.size() <= 8) key_display = std::string(k.size(), '*');
        else key_display = k.substr(0, 4) + "***" + k.substr(k.size() - 3);
    } else {
        key_display = "(not set — will read $ANTHROPIC_API_KEY)";
    }
    body.push_back(SettingRow(
        "API Key",
        text(" " + key_display + " ") | color(Color::Yellow) |
             (focus_row == 1 ? inverted : nothing),
        "Stored in user config. Leave blank to use the environment variable",
        false, false));

    body.push_back(SettingRow(
        "Timeout (s)",
        text(std::format(" {} ", w.timeout_seconds)) | color(Color::Cyan) |
             (focus_row == 2 ? inverted : nothing),
        "Per-request timeout in seconds",
        false, false));

    body.push_back(SettingRow(
        "Verify SSL",
        text(w.verify_ssl ? " [ON]  " : " [OFF] ") |
             color(w.verify_ssl ? Color::Green : Color::Red) |
             (focus_row == 3 ? inverted : nothing),
        "Verify server TLS certificates (disable only for dev proxies)",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "HTTP proxy",
        text(w.proxy.value_or("(none)")) | color(Color::Cyan) |
             (focus_row == 4 ? inverted : nothing),
        "HTTPS_PROXY / HTTP_PROXY override (env takes precedence)",
        false, false));
    body.push_back(SettingRow(
        "Max retries",
        text(std::format(" {} ", w.max_retries)) | color(Color::Cyan) |
             (focus_row == 5 ? inverted : nothing),
        "Number of retry attempts per failed request",
        false, false));
    return vbox(body);
}

// --- Permissions tab ---
[[nodiscard]] inline Element RenderPermissionsTab(const WorkingSettings& w,
                                                  int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("Permissions",
        "Default behaviour when tools request access"));

    body.push_back(SettingRow(
        "Default mode",
        text(" [" + perm_mode_str(w.default_perm) + "] ") |
             color(Color::Yellow) | (focus_row == 0 ? inverted : nothing),
        "Ask for confirmation, always allow, or always deny",
        false, false));

    body.push_back(SettingRow(
        "Allow Bash",
        text(w.allow_bash ? " [ON]  " : " [OFF] ") |
             color(w.allow_bash ? Color::Green : Color::GrayDark) |
             (focus_row == 1 ? inverted : nothing),
        "Permit shell command execution",
        false, false));

    body.push_back(SettingRow(
        "Allow file write",
        text(w.allow_file_write ? " [ON]  " : " [OFF] ") |
             color(w.allow_file_write ? Color::Green : Color::GrayDark) |
             (focus_row == 2 ? inverted : nothing),
        "Permit file creation and modification",
        false, false));

    body.push_back(SettingRow(
        "Allow network",
        text(w.allow_network ? " [ON]  " : " [OFF] ") |
             color(w.allow_network ? Color::Green : Color::GrayDark) |
             (focus_row == 3 ? inverted : nothing),
        "Permit outbound HTTP requests (web fetch / search)",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "Allowed paths",
        text(w.allowed_paths.empty()
            ? std::string(" (all) ")
            : std::format(" [{} entries] ", w.allowed_paths.size())) |
             color(Color::Cyan) | (focus_row == 4 ? inverted : nothing),
        "Whitelisted file paths for tool access",
        false, false));
    body.push_back(SettingRow(
        "Denied paths",
        text(w.denied_paths.empty()
            ? std::string(" (none) ")
            : std::format(" [{} entries] ", w.denied_paths.size())) |
             color(Color::Cyan) | (focus_row == 5 ? inverted : nothing),
        "Blacklisted file paths for tool access",
        false, false));
    body.push_back(SettingRow(
        "Allowed commands",
        text(w.allowed_commands.empty()
            ? std::string(" (all) ")
            : std::format(" [{} entries] ", w.allowed_commands.size())) |
             color(Color::Cyan) | (focus_row == 6 ? inverted : nothing),
        "Whitelisted shell commands",
        false, false));
    return vbox(body);
}

// --- Tools tab ---
[[nodiscard]] inline Element RenderToolsTab(const WorkingSettings& w,
                                            int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("Tools", "Enable or disable top-level tools"));

    body.push_back(SettingRow(
        "Agent / Sub-agent",
        text(w.enable_agent_tool ? " [ON]  " : " [OFF] ") |
             color(w.enable_agent_tool ? Color::Green : Color::GrayDark) |
             (focus_row == 0 ? inverted : nothing),
        "Allow the assistant to spawn sub-agents for parallel work",
        false, false));
    body.push_back(SettingRow(
        "Web fetch",
        text(w.enable_web_fetch ? " [ON]  " : " [OFF] ") |
             color(w.enable_web_fetch ? Color::Green : Color::GrayDark) |
             (focus_row == 1 ? inverted : nothing),
        "Fetch arbitrary URLs and return their content",
        false, false));
    body.push_back(SettingRow(
        "Web search",
        text(w.enable_web_search ? " [ON]  " : " [OFF] ") |
             color(w.enable_web_search ? Color::Green : Color::GrayDark) |
             (focus_row == 2 ? inverted : nothing),
        "Run web searches via the configured provider",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "Bash granular controls",
        text(w.enable_bash_granular ? " [ON]  " : " [OFF] ") |
             color(w.enable_bash_granular ? Color::Green : Color::GrayDark) |
             (focus_row == 3 ? inverted : nothing),
        "Fine-grained per-command permission toggles",
        false, false));
    body.push_back(SettingRow(
        "Glob / Grep safety",
        text(w.enable_glob_grep_safety ? " [ON]  " : " [OFF] ") |
             color(w.enable_glob_grep_safety ? Color::Green : Color::GrayDark) |
             (focus_row == 4 ? inverted : nothing),
        "Restrict search tools to allowed paths only",
        false, false));
    body.push_back(SettingRow(
        "Skill loading",
        text(w.enable_skill_loading ? " [ON]  " : " [OFF] ") |
             color(w.enable_skill_loading ? Color::Green : Color::GrayDark) |
             (focus_row == 5 ? inverted : nothing),
        "Enable the skill system for extensible agent capabilities",
        false, false));
    return vbox(body);
}

// --- MCP tab ---
[[nodiscard]] inline Element RenderMCPTab(const WorkingSettings& w,
                                          int focus_row,
                                          int selected_server) {
    Elements body;
    body.push_back(RenderTabHeader("MCP",
        "Model Context Protocol servers (managed by services/mcp)"));

    body.push_back(SettingRow(
        "Auto-start servers",
        text(w.auto_start_mcp ? " [ON]  " : " [OFF] ") |
             color(w.auto_start_mcp ? Color::Green : Color::GrayDark) |
             (focus_row == 0 ? inverted : nothing),
        "Start configured servers when the REPL launches",
        false, false));

    // Server list
    body.push_back(text(""));
    body.push_back(text(" Configured servers:") | bold);
    body.push_back(separator());
    if (w.mcp_servers.empty()) {
        body.push_back(text("   (none — use /mcp add to configure)") | dim);
    } else {
        for (std::size_t i = 0; i < w.mcp_servers.size(); ++i) {
            const auto& s = w.mcp_servers[i];
            bool sel = (int)i == selected_server;
            auto name = text(" " + s.name) | (sel ? bold : nothing);
            auto transport = text(" [" + s.transport + "]") | dim;
            auto cmd = s.command.empty()
                           ? (s.url.value_or("(remote)"))
                           : s.command;
            auto row = hbox({
                sel ? text("›") | color(Color::Cyan) : text(" "),
                name | size(WIDTH, EQUAL, 18),
                transport,
                text("  "),
                text(cmd) | color(Color::Cyan) | dim,
            });
            if (sel) row = row | bgcolor(Color::RGB(30, 40, 55));
            body.push_back(row);
        }
    }

    body.push_back(text(""));
    body.push_back(hbox({
        text(" [a]") | color(Color::Cyan), text("dd "),
        text("[r]") | color(Color::Cyan), text("emove "),
        text("[e]") | color(Color::Cyan), text("dit "),
    }) | dim);

    body.push_back(separator());
    body.push_back(SettingRow(
        "Server lifecycle",
        text(w.auto_start_mcp ? " [auto-managed] " : " [manual] ") |
             color(Color::Cyan),
        "MCP servers restart/reconnect automatically when auto-start is ON",
        false, false));
    return vbox(body);
}

// --- LSP tab ---
[[nodiscard]] inline Element RenderLSPTab(const WorkingSettings& w,
                                          int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("LSP", "Language Server Protocol clients"));

    body.push_back(SettingRow(
        "Enable LSP",
        text(w.enable_lsp ? " [ON]  " : " [OFF] ") |
             color(w.enable_lsp ? Color::Green : Color::GrayDark) |
             (focus_row == 0 ? inverted : nothing),
        "Start LSP clients for detected project languages",
        false, false));
    body.push_back(SettingRow(
        "LSP timeout (s)",
        text(std::format(" {} ", w.lsp_timeout)) | color(Color::Cyan) |
             (focus_row == 1 ? inverted : nothing),
        "Per-request timeout for LSP operations",
        false, false));
    body.push_back(SettingRow(
        "LSP instances", text(" (delegates to services/lsp) ") | dim,
        "Running instances shown on the Status tab",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "Per-language LSP config",
        text(" (delegates to services/lsp registry) ") | color(Color::Cyan),
        "Language-specific LSP server configurations managed externally",
        false, false));
    return vbox(body);
}

// --- Bridge tab ---
[[nodiscard]] inline Element RenderBridgeTab(const WorkingSettings& w,
                                             int focus_row) {
    Elements body;
    body.push_back(RenderTabHeader("Bridge",
        "IDE / editor bridge (VS Code, JetBrains, etc.)"));

    body.push_back(SettingRow(
        "Enable bridge",
        text(w.enable_bridge ? " [ON]  " : " [OFF] ") |
             color(w.enable_bridge ? Color::Green : Color::GrayDark) |
             (focus_row == 0 ? inverted : nothing),
        "Listen for incoming editor connections",
        false, false));
    body.push_back(SettingRow(
        "Outbound-only mode",
        text(w.bridge_outbound_only ? " [ON]  " : " [OFF] ") |
             color(w.bridge_outbound_only ? Color::Green : Color::GrayDark) |
             (focus_row == 1 ? inverted : nothing),
        "Only allow REPL→editor calls (security hardening)",
        false, false));
    body.push_back(SettingRow(
        "Listen port",
        text(std::format(" {} ", w.bridge_port)) | color(Color::Cyan) |
             (focus_row == 2 ? inverted : nothing),
        "TCP port for the bridge JWT-authenticated server",
        false, false));

    body.push_back(separator());
    body.push_back(SettingRow(
        "JWT secret",
        text(w.bridge_jwt_secret.empty()
            ? std::string(" (auto-generated) ")
            : std::string(" [configured] ")) | color(Color::Yellow) |
             (focus_row == 3 ? inverted : nothing),
        "JWT secret for bridge authentication (rotate via CLI)",
        false, false));
    body.push_back(SettingRow(
        "Allowed origins",
        text(w.bridge_allowed_origins.empty()
            ? std::string(" (any) ")
            : std::format(" [{} entries] ", w.bridge_allowed_origins.size())) |
             color(Color::Cyan) | (focus_row == 4 ? inverted : nothing),
        "CORS origin allowlist for bridge connections",
        false, false));
    return vbox(body);
}

[[nodiscard]] inline Element RenderHooksTab() {
    return vbox({
        RenderTabHeader("Hooks", "Lifecycle hook configuration"),
        SettingRow(
            "Configuration",
            text(" Managed in settings JSON ") | color(Color::Cyan),
            "Hook matchers and commands are loaded by the native hook registry",
            false,
            false),
        SettingRow(
            "Runtime",
            text(" PreToolUse / PostToolUse / Stop ") | color(Color::Green),
            "Hook execution is reported through the lifecycle hook service",
            false,
            false),
    });
}

[[nodiscard]] inline Element RenderPrivacyTab() {
    return vbox({
        RenderTabHeader("Privacy", "Telemetry and data handling"),
        SettingRow(
            "Telemetry",
            text(" Uses configured analytics sink ") | color(Color::Cyan),
            "Respect environment and managed settings opt-out controls",
            false,
            false),
        SettingRow(
            "Secrets",
            text(" Redacted in UI and logs ") | color(Color::Green),
            "API keys and sensitive tokens are never shown as editable sample values",
            false,
            false),
    });
}

[[nodiscard]] inline Element RenderAboutTab() {
    return vbox({
        RenderTabHeader("About", "CC-REPL native migration"),
        SettingRow(
            "Runtime",
            text(" C++23 modules ") | color(Color::Cyan),
            "Native CLI implementation with CMake/CTest migration gates",
            false,
            false),
        SettingRow(
            "Version",
            text(" 1.0.0 ") | color(Color::Yellow),
            "Project version from cpp_migration/CMakeLists.txt",
            false,
            false),
    });
}

// --- Status tab (health lights) ---
[[nodiscard]] inline Element RenderStatusTab(
    const std::vector<SubsystemHealth>& rows) {
    Elements body;
    body.push_back(RenderTabHeader("Status",
        "Live health of API, MCP, LSP and bridge subsystems"));

    if (rows.empty()) {
        body.push_back(text("  (no diagnostics yet — running checks…)") | dim);
        return vbox(body);
    }

    for (const auto& r : rows) {
        auto [icon, col] = health_display(r.state);
        auto state_label = (r.state == HealthState::OK)       ? "OK"
                         : (r.state == HealthState::Degraded) ? "Degraded"
                                                              : "Down";
        auto line = hbox({
            text(" " + icon + " ") | color(col),
            text(r.name) | bold | size(WIDTH, EQUAL, 18),
            text(state_label) | color(col) | size(WIDTH, EQUAL, 12),
            text(format_ts(r.last_check)) | dim | size(WIDTH, EQUAL, 12),
            text("  "),
            text(r.message) | dim,
        });
        body.push_back(line);
    }
    return vbox(body);
}

// --- Usage tab (cards + gauges + table) ---
[[nodiscard]] inline Element RenderUsageTab(const UsageSnapshot& u) {
    Elements body;
    body.push_back(RenderTabHeader("Usage",
        "Cost, tokens and request counters (current session)"));

    // --- 4 summary cards ---
    auto card = [](const std::string& label, const std::string& value,
                   Color c) {
        return vbox({
            text(" " + label) | dim,
            text(" " + value) | bold | color(c),
        }) | border | size(WIDTH, EQUAL, 22);
    };

    Elements cards = {
        card("Total Cost",
             std::format("${:.4f}", u.total_cost_usd), Color::Green),
        card("Input Tokens",
             std::format("{}", u.total_input_tokens), Color::Cyan),
        card("Output Tokens",
             std::format("{}", u.total_output_tokens), Color::Cyan),
        card("Requests",
             std::format("{}", u.total_requests), Color::Yellow),
    };
    body.push_back(hbox(std::move(cards)));
    body.push_back(text(""));

    // --- Progress bars (gauge) ---
    body.push_back(text(" Limits") | bold);
    body.push_back(separator());

    auto pct_label = [](double p) {
        return std::format("{}%", static_cast<int>(p * 100));
    };

    body.push_back(hbox({
        text(" Daily limit ") | size(WIDTH, EQUAL, 16),
        gauge(std::clamp(u.daily_limit_pct, 0.0, 1.0))
            | color(u.daily_limit_pct > 0.9 ? Color::Red : Color::Green)
            | size(WIDTH, EQUAL, 40),
        text(" " + pct_label(u.daily_limit_pct) + " ") | dim,
    }));
    body.push_back(hbox({
        text(" Rate limit ") | size(WIDTH, EQUAL, 16),
        gauge(std::clamp(u.rate_limit_pct, 0.0, 1.0))
            | color(u.rate_limit_pct > 0.9 ? Color::Red : Color::Yellow)
            | size(WIDTH, EQUAL, 40),
        text(" " + pct_label(u.rate_limit_pct) + " ") | dim,
    }));
    body.push_back(text(""));

    // --- Per-model table ---
    body.push_back(text(" Per-model breakdown") | bold);
    body.push_back(separator());
    if (u.per_model.empty()) {
        body.push_back(text("  (no requests recorded yet)") | dim);
    } else {
        // header
        body.push_back(hbox({
            text(" Model") | bold | size(WIDTH, EQUAL, 28),
            text(" Requests") | bold | size(WIDTH, EQUAL, 12),
            text(" Tokens") | bold | size(WIDTH, EQUAL, 16),
            text(" Cost") | bold | size(WIDTH, EQUAL, 12),
        }));
        body.push_back(separator());
        for (const auto& r : u.per_model) {
            body.push_back(hbox({
                text(" " + r.model_name) | size(WIDTH, EQUAL, 28),
                text(std::format(" {}", r.requests))
                    | color(Color::Yellow) | size(WIDTH, EQUAL, 12),
                text(std::format(" {}↓ {}↑",
                    r.input_tokens, r.output_tokens))
                    | color(Color::Cyan) | size(WIDTH, EQUAL, 16),
                text(std::format(" ${:.4f}", r.cost_usd))
                    | color(Color::Green) | size(WIDTH, EQUAL, 12),
            }));
        }
    }
    body.push_back(text(""));
    body.push_back(hbox({
        text(" [r]") | color(Color::Cyan), text("efresh  "),
    }) | dim);

    return vbox(body);
}

// ============================================================
// Rendering: dialog shell (sidebar + tab content + footer)
// ============================================================

[[nodiscard]] inline Element RenderSidebar(SettingsTabId selected) {
    Elements items;
    items.push_back(text(" Sections") | dim);
    items.push_back(separator());

    // Iterate all tabs in the sidebar order
    constexpr std::array<SettingsTabId, 13> kOrder = {{
        SettingsTabId::General,
        SettingsTabId::Model,
        SettingsTabId::API,
        SettingsTabId::Permissions,
        SettingsTabId::Tools,
        SettingsTabId::MCP,
        SettingsTabId::LSP,
        SettingsTabId::Bridge,
        SettingsTabId::Hooks,
        SettingsTabId::Privacy,
        SettingsTabId::About,
        SettingsTabId::Status,
        SettingsTabId::Usage,
    }};

    for (auto id : kOrder) {
        bool sel = (id == selected);
        auto el = text(" " + tab_label(id)) | (sel ? bold : nothing);
        if (sel) el = el | inverted;
        else el = el | dim;
        items.push_back(el);
    }

    return vbox(items) | size(WIDTH, EQUAL, 18) | border;
}

[[nodiscard]] inline Element RenderFooter(
    const ToastState& toast, bool dirty) {

    // Text-style buttons (FTXUI Button() not used — keeps dialog rendering
    // pure-Elements so keyboard dispatch stays in CatchEvent).
    auto style_btn = [](std::string_view label, Color c, bool enabled = true) {
        auto el = hbox({text(" ["), text(std::string(label))
                            | color(c) | bold, text("] ")});
        if (!enabled) el = el | dim;
        return el;
    };

    auto save_btn  = style_btn("Save  Ctrl+S", Color::Green, dirty);
    auto reset_btn = style_btn("Reset  Ctrl+R", Color::Yellow);
    auto close_btn = style_btn("Close  Esc", Color::Red);

    auto dirty_marker = dirty
        ? text(" * unsaved changes") | color(Color::Yellow) | dim
        : text("");

    Element toast_el = text("");
    if (toast.active()) {
        toast_el = hbox({
            text(" "),
            text(toast.message) | color(toast.color) | bold,
            filler(),
        });
    }

    return vbox({
        separator(),
        hbox({
            save_btn,
            text(" "),
            reset_btn,
            text(" "),
            close_btn,
            filler(),
            dirty_marker,
        }),
        toast_el,
    });
}

// ============================================================
// Interactive component
// ============================================================

/// Create the settings dialog with a live ConfigManager reference.
/// All reads and writes flow through `cfg`.
[[nodiscard]] inline Component MakeSettingsDialog(
    ConfigManager& cfg,
    SettingsDialogOptions opts = {}) {

    struct State {
        SettingsDialogOptions opts;
        ConfigManager* cfg;
        WorkingSettings working;
        WorkingSettings defaults;   // pristine snapshot for Reset
        SettingsTabId selected_tab;
        int focus_row = 0;
        int mcp_selected_server = 0;
        int mcp_tab_row = 1;        // which row inside MCP tab (0 = toggle, 1+ = list)
        ToastState toast;
        bool dirty = false;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(opts);
    state->cfg = &cfg;
    state->working = snapshot_from(cfg);
    state->defaults = state->working;
    state->selected_tab = state->opts.initial_tab;

    // Helper: toggle boolean + mark dirty
    auto toggle = [state](bool& field) {
        field = !field;
        state->dirty = true;
    };
    auto mark = [state]() { state->dirty = true; };

    // Helper: set transient toast
    auto show_toast = [state](std::string msg, Color c = Color::Green) {
        state->toast.message = std::move(msg);
        state->toast.color = c;
        state->toast.until = std::chrono::steady_clock::now()
                             + std::chrono::seconds(3);
    };

    return Renderer([state] {
        Element content;
        switch (state->selected_tab) {
            case SettingsTabId::General:
                content = RenderGeneralTab(state->working, state->focus_row);
                break;
            case SettingsTabId::Model:
                content = RenderModelTab(state->working, state->focus_row);
                break;
            case SettingsTabId::API:
                content = RenderAPITab(state->working, state->focus_row);
                break;
            case SettingsTabId::Permissions:
                content = RenderPermissionsTab(state->working, state->focus_row);
                break;
            case SettingsTabId::Tools:
                content = RenderToolsTab(state->working, state->focus_row);
                break;
            case SettingsTabId::MCP:
                content = RenderMCPTab(state->working, state->focus_row,
                                       state->mcp_selected_server);
                break;
            case SettingsTabId::LSP:
                content = RenderLSPTab(state->working, state->focus_row);
                break;
            case SettingsTabId::Bridge:
                content = RenderBridgeTab(state->working, state->focus_row);
                break;
            case SettingsTabId::Hooks:
                content = RenderHooksTab();
                break;
            case SettingsTabId::Privacy:
                content = RenderPrivacyTab();
                break;
            case SettingsTabId::About:
                content = RenderAboutTab();
                break;
            case SettingsTabId::Status:
                content = RenderStatusTab(state->opts.status_rows);
                break;
            case SettingsTabId::Usage: {
                UsageSnapshot snap;
                if (state->opts.refresh_usage) {
                    state->opts.refresh_usage(snap);
                }
                content = RenderUsageTab(snap);
                break;
            }
            case SettingsTabId::_COUNT:
                content = text("(empty)");
                break;
        }

        auto sidebar = RenderSidebar(state->selected_tab);
        auto body = vbox({
            content | yframe | flex,
            RenderFooter(state->toast, state->dirty),
        });

        return window(
            text(" Settings ") | bold | color(Color::Magenta),
            hbox({sidebar, body | flex})
        ) | color(Color::Magenta);
    }) | CatchEvent([state, toggle, mark, show_toast](Event event) -> bool {

        // --- Global hotkeys ---
        // Save
        if (is_ctrl_key(event, 's')) {
            apply_to(state->working, *state->cfg);
            auto res = state->cfg->save();
            if (res) {
                state->dirty = false;
                state->defaults = state->working;
                show_toast("✓ Settings saved");
            } else {
                show_toast("✗ Save failed: " + res.error().message,
                           Color::Red);
            }
            return true;
        }

        // Close
        if (event == Event::Escape) {
            if (state->opts.on_close) {
                state->opts.on_close(
                    state->dirty ? std::optional("Settings closed without saving")
                                 : std::nullopt,
                    CommandResultDisplay::system);
            }
            return true;
        }

        // --- Tab cycling (1..9/0 hotkeys + Ctrl+Tab) ---
        if (event.is_character()) {
            char ch = event.character()[0];
            if (ch >= '1' && ch <= '9') {
                int idx = ch - '1';
                constexpr std::array<SettingsTabId, 9> kOrder = {{
                    SettingsTabId::General, SettingsTabId::Model,
                    SettingsTabId::API, SettingsTabId::Permissions,
                    SettingsTabId::Tools, SettingsTabId::MCP,
                    SettingsTabId::LSP, SettingsTabId::Bridge,
                    SettingsTabId::Hooks,
                }};
                if (idx < (int)kOrder.size()) {
                    state->selected_tab = kOrder[idx];
                    state->focus_row = 0;
                    return true;
                }
            }
            if (ch == '0') {
                state->selected_tab = SettingsTabId::Privacy;
                state->focus_row = 0;
                return true;
            }
            if (ch == 'a' || ch == 'A') {
                state->selected_tab = SettingsTabId::About;
                state->focus_row = 0;
                return true;
            }
        }

        // Usage tab refresh
        if (state->selected_tab == SettingsTabId::Usage &&
            event == Event::Character('r')) {
            show_toast("↻ Usage data refreshed");
            return true;
        }

        // --- Sidebar navigation: up/down when focus is on sidebar row 0 ---
        // (Tab key cycles focus between sidebar and content rows.)
        if (event == Event::Tab) {
            state->focus_row = (state->focus_row + 1) % 8;
            return true;
        }
        if (event == Event::TabReverse) {
            state->focus_row = (state->focus_row + 7) % 8;
            return true;
        }

        // --- Up/Down changes sidebar tab OR row inside current tab ---
        // Shift+Up / Shift+Down change sidebar. Plain Up/Down changes row.
        // Simplified: if focus_row is 0 (sidebar-style) we cycle tabs,
        // else we cycle a row index within the tab.
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (state->focus_row <= 0) {
                // cycle tabs up
                auto cur = static_cast<int>(state->selected_tab);
                cur = (cur + static_cast<int>(SettingsTabId::_COUNT) - 1)
                      % static_cast<int>(SettingsTabId::_COUNT);
                state->selected_tab = static_cast<SettingsTabId>(cur);
            } else {
                state->focus_row--;
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            if (state->focus_row >= 7) {
                // cycle tabs down
                auto cur = static_cast<int>(state->selected_tab);
                cur = (cur + 1) % static_cast<int>(SettingsTabId::_COUNT);
                state->selected_tab = static_cast<SettingsTabId>(cur);
                state->focus_row = 0;
            } else {
                state->focus_row++;
            }
            return true;
        }

        // Left / Right also cycle tabs
        if (event == Event::ArrowLeft) {
            auto cur = static_cast<int>(state->selected_tab);
            cur = (cur + static_cast<int>(SettingsTabId::_COUNT) - 1)
                  % static_cast<int>(SettingsTabId::_COUNT);
            state->selected_tab = static_cast<SettingsTabId>(cur);
            return true;
        }
        if (event == Event::ArrowRight) {
            auto cur = static_cast<int>(state->selected_tab);
            cur = (cur + 1) % static_cast<int>(SettingsTabId::_COUNT);
            state->selected_tab = static_cast<SettingsTabId>(cur);
            return true;
        }

        // --- Space / Enter: toggle boolean row ---
        if (event == Event::Character(' ') || event == Event::Return) {
            auto& w = state->working;
            auto row = state->focus_row;
            switch (state->selected_tab) {
                // General booleans / cyclic selects
                case SettingsTabId::General:
                    if (row == 0) { // Theme: cycle
                        {
                            static const std::vector<std::string> kThemes =
                                {"dark", "light", "high-contrast", "auto"};
                            auto it = std::find(kThemes.begin(), kThemes.end(), w.theme);
                            int idx = (it == kThemes.end()) ? 0
                                      : static_cast<int>(it - kThemes.begin());
                            w.theme = kThemes[(idx + 1) % kThemes.size()];
                            mark(); return true;
                        }
                    }
                    if (row == 1) { toggle(w.auto_mode); return true; }
                    if (row == 2) { // effort cycle
                        static const std::vector<std::string> kEffort =
                            {"low", "medium", "high"};
                        auto it = std::find(kEffort.begin(), kEffort.end(), w.effort_level);
                        int idx = (it == kEffort.end()) ? 1
                                  : static_cast<int>(it - kEffort.begin());
                        w.effort_level = kEffort[(idx + 1) % kEffort.size()];
                        mark(); return true;
                    }
                    if (row == 3) { toggle(w.fast_mode); return true; }
                    if (row == 4) { // temperature step up by 0.1
                        w.default_temperature = std::min(2.0, w.default_temperature + 0.1);
                        mark(); return true;
                    }
                    if (row == 5) { toggle(w.show_thinking); return true; }
                    if (row == 6) { toggle(w.show_token_usage); return true; }
                    if (row == 7) { toggle(w.compact_mode); return true; }
                    break;
                case SettingsTabId::Model:
                    if (row == 1) { // max tokens cycle (coarse steps)
                        static const std::vector<std::uint32_t> kSteps =
                            {2048, 4096, 8192, 16384, 32768, 65536};
                        auto it = std::find(kSteps.begin(), kSteps.end(), w.max_output_tokens);
                        int idx = (it == kSteps.end()) ? 3
                                  : static_cast<int>(it - kSteps.begin());
                        w.max_output_tokens = kSteps[(idx + 1) % kSteps.size()];
                        mark(); return true;
                    }
                    if (row == 2) { toggle(w.extended_thinking); return true; }
                    if (row == 0) { // cycle model
                        static const std::vector<std::string> kModels = {
                            "claude-sonnet-4-20250514",
                            "claude-opus-4-20250514",
                            "claude-haiku-4-20250514",
                        };
                        auto it = std::find(kModels.begin(), kModels.end(), w.default_model);
                        int idx = (it == kModels.end()) ? 0
                                  : static_cast<int>(it - kModels.begin());
                        w.default_model = kModels[(idx + 1) % kModels.size()];
                        mark(); return true;
                    }
                    if (row == 3) { // thinking budget step
                        static const std::vector<std::optional<std::uint32_t>> kBudgets = {
                            std::nullopt, std::optional(4096u), std::optional(8192u),
                            std::optional(16384u), std::optional(32768u),
                        };
                        auto it = std::find(kBudgets.begin(), kBudgets.end(), w.thinking_budget);
                        int idx = (it == kBudgets.end()) ? 0
                                  : static_cast<int>(it - kBudgets.begin());
                        w.thinking_budget = kBudgets[(idx + 1) % kBudgets.size()];
                        mark(); return true;
                    }
                    if (row == 4) { // context window cycle
                        static const std::vector<std::uint32_t> kCtx =
                            {100000, 128000, 200000};
                        auto it = std::find(kCtx.begin(), kCtx.end(), w.context_window_size);
                        int idx = (it == kCtx.end()) ? 2
                                  : static_cast<int>(it - kCtx.begin());
                        w.context_window_size = kCtx[(idx + 1) % kCtx.size()];
                        mark(); return true;
                    }
                    if (row == 5) { // teammate model cycle
                        static const std::vector<std::string> kTeam = {
                            "", "claude-sonnet-4-20250514",
                            "claude-haiku-4-20250514",
                        };
                        auto it = std::find(kTeam.begin(), kTeam.end(), w.teammate_model);
                        int idx = (it == kTeam.end()) ? 0
                                  : static_cast<int>(it - kTeam.begin());
                        w.teammate_model = kTeam[(idx + 1) % kTeam.size()];
                        mark(); return true;
                    }
                    break;
                case SettingsTabId::API:
                    if (row == 2) { // timeout step
                        w.timeout_seconds += 30;
                        if (w.timeout_seconds > 600) w.timeout_seconds = 30;
                        mark(); return true;
                    }
                    if (row == 3) { toggle(w.verify_ssl); return true; }
                    if (row == 0) { // cycle base URL presets
                        static const std::vector<std::optional<std::string>> kUrls = {
                            std::nullopt,
                            std::optional("https://api.anthropic.com"),
                            std::optional("http://localhost:8080"),
                        };
                        auto it = std::find(kUrls.begin(), kUrls.end(), w.base_url);
                        int idx = (it == kUrls.end()) ? 0
                                  : static_cast<int>(it - kUrls.begin());
                        w.base_url = kUrls[(idx + 1) % kUrls.size()];
                        mark(); return true;
                    }
                    if (row == 1) {
                        if (w.api_key) {
                            w.api_key = std::nullopt;
                            mark();
                            show_toast("API key cleared");
                        } else {
                            show_toast(
                                "Set ANTHROPIC_API_KEY or run the auth flow",
                                Color::Yellow);
                        }
                        return true;
                    }
                    if (row == 4) { // proxy: cycle presets
                        static const std::vector<std::optional<std::string>> kProxy = {
                            std::nullopt,
                            std::optional("http://localhost:8888"),
                            std::optional("http://proxy.corp:3128"),
                        };
                        auto it = std::find(kProxy.begin(), kProxy.end(), w.proxy);
                        int idx = (it == kProxy.end()) ? 0
                                  : static_cast<int>(it - kProxy.begin());
                        w.proxy = kProxy[(idx + 1) % kProxy.size()];
                        mark(); return true;
                    }
                    if (row == 5) { // max retries step
                        w.max_retries = (w.max_retries % 10) + 1;
                        mark(); return true;
                    }
                    break;
                case SettingsTabId::Permissions:
                    if (row == 0) { // cycle default perm mode
                        auto m = static_cast<int>(w.default_perm);
                        w.default_perm = static_cast<DefaultPermissionMode>(
                            (m + 1) % 3);
                        mark(); return true;
                    }
                    if (row == 1) { toggle(w.allow_bash); return true; }
                    if (row == 2) { toggle(w.allow_file_write); return true; }
                    if (row == 3) { toggle(w.allow_network); return true; }
                    break;
                case SettingsTabId::Tools:
                    if (row == 0) { toggle(w.enable_agent_tool); return true; }
                    if (row == 1) { toggle(w.enable_web_fetch); return true; }
                    if (row == 2) { toggle(w.enable_web_search); return true; }
                    if (row == 3) { toggle(w.enable_bash_granular); return true; }
                    if (row == 4) { toggle(w.enable_glob_grep_safety); return true; }
                    if (row == 5) { toggle(w.enable_skill_loading); return true; }
                    break;
                case SettingsTabId::MCP:
                    if (state->mcp_tab_row == 0) {
                        toggle(w.auto_start_mcp); return true;
                    }
                    break;
                case SettingsTabId::LSP:
                    if (row == 0) { toggle(w.enable_lsp); return true; }
                    if (row == 1) {
                        w.lsp_timeout += 15;
                        if (w.lsp_timeout > 300) w.lsp_timeout = 15;
                        mark(); return true;
                    }
                    break;
                case SettingsTabId::Bridge:
                    if (row == 0) { toggle(w.enable_bridge); return true; }
                    if (row == 1) { toggle(w.bridge_outbound_only); return true; }
                    if (row == 2) {
                        w.bridge_port += 1;
                        mark(); return true;
                    }
                    break;
                default:
                    break;
            }
        }

        // MCP-specific keys
        if (state->selected_tab == SettingsTabId::MCP) {
            if (event == Event::Character('a')) {
                show_toast("Use /mcp add to configure a server", Color::Yellow);
                return true;
            }
            if (event == Event::Character('r')) {
                if (!state->working.mcp_servers.empty()) {
                    int idx = std::clamp(state->mcp_selected_server, 0,
                        (int)state->working.mcp_servers.size() - 1);
                    state->working.mcp_servers.erase(
                        state->working.mcp_servers.begin() + idx);
                    state->mcp_selected_server = std::max(0,
                        state->mcp_selected_server - 1);
                    mark();
                    show_toast("− MCP server removed");
                    return true;
                }
            }
        }

        // Reset
        if (is_ctrl_key(event, 'r')) {
            state->working = state->defaults;
            state->dirty = false;
            show_toast("↺ Reverted to last saved values");
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::dialogs::settings_dialog
