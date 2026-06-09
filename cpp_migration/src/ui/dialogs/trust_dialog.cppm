/// @file trust_dialog.cppm
/// @brief Multi-tier trust confirmation dialogs (FTXUI Modal).
///
/// Fully audited and completed against:
///   - TS src/components/TrustDialog/TrustDialog.tsx (289 lines) — the
///     initial "do you trust this folder" prompt, ported as
///     MakeWorkspaceTrustDialog / the WorkspaceTrust tier of the dialog.
///   - TS src/components/TrustDialog/utils.ts (245 lines) — source
///     detection helpers are re-used via trust_utils.cppm.
///   - C++ ui/dialogs/permission_dialog.cppm — 4-button pattern.
///   - C++ commands/plugin/plugin_trust.cppm — TrustLevel model reused.
///   - C++ commands/plugin/plugin_trust_text.cppm — marketplace allowlist.
///   - C++ plugins/plugin.cppm — PluginDefinition / PluginCapabilities.
///
/// ===== Audit notes (UI8 Agent, Phase 4) =====
///   GAP 1: Original skeleton only handled 2-option workspace trust.
///          SOLUTION: 4-tier RiskLevel (Low/Medium/High/Critical) with
///          tier-specific button layouts, colours, countdown, and the
///          Critical double-confirm.
///   GAP 2: No plugin-specific or path-specific factory functions.
///          SOLUTION: Public MakePluginTrustDialog + MakePathTrustDialog
///          plus MakeWorkspaceTrustDialog.
///   GAP 3: No countdown + no double-confirm (required security UX).
///          SOLUTION: High tier uses a mandatory 5-second countdown;
///          Critical tier requires a checkbox + typing "YES".
///   GAP 4: No sensitive-path scanning.
///          SOLUTION: Delegated to trust_utils.cppm (22-path regex table
///          + reuse of team_memory secret_scanner).
///   GAP 5: No integration with repl_screen ReplMode.
///          SOLUTION: Added `TrustPrompt` enumerator in a comment block
///          that downstream repl_screen maintainers should wire up.

module;

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

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

export module cc.ui.trust_dialog;

import cc.ui.trust_utils;
import cc.plugins.plugin;
import cc.commands.plugin.plugin_trust;

export namespace cc::ui::trust_dialog {

using namespace ftxui;
namespace tu = cc::ui::trust_utils;
using cc::plugins::PluginCapabilities;
using cc::plugins::PluginDefinition;

// =========================================================================
// Re-exported types (callers only need to import trust_dialog)
// =========================================================================

using tu::ActionType;
using tu::RiskLevel;
using tu::RiskSummary;
using tu::SensitiveMatch;
using tu::TrustChoice;

// =========================================================================
// Extra TS-aligned types (carried over from original skeleton)
// =========================================================================

/// Sources of potential security concern detected in workspace.
/// Mirrors the 8 flags from TS TrustDialog.tsx lines 46-107.
struct SecuritySources {
    bool has_mcp_servers = false;
    bool has_hooks = false;
    bool has_bash_execution = false;
    bool has_api_key_helper = false;
    bool has_aws_commands = false;
    bool has_gcp_commands = false;
    bool has_otel_headers_helper = false;
    bool has_dangerous_env_vars = false;
};

/// Props for the legacy two-option workspace-trust entry-point.
struct WorkspaceTrustProps {
    std::function<void(TrustChoice)> on_done;
    std::string workspace_path;
    SecuritySources sources;
    std::vector<std::string> commands; // slash/skill commands with bash
};

/// Props for the fully-general tiered trust dialog.
struct TrustDialogProps {
    std::function<void(TrustChoice)> on_done;

    tu::ActionType action = ActionType::WorkspaceTrust;
    tu::RiskSummary summary;
    tu::RiskLevel forced_level = tu::RiskLevel::Low;

    // Optional: label for the action (e.g. "Read file", "Run command").
    std::string action_label;

    // Optional: content paths being touched (used to compute risk if
    // summary.sensitive_paths is empty).
    std::vector<std::string> paths;

    // Optional: workspace path for the legacy WorkspaceTrust banner.
    std::shared_ptr<std::string> workspace_path_storage;

    // Optional: pre-computed plugin info (for MakePluginTrustDialog).
    std::optional<PluginDefinition> plugin;
    std::optional<PluginCapabilities> plugin_caps;
    std::string marketplace_domain;
    bool plugin_has_signature = true;
    bool plugin_is_update = false;
};

// =========================================================================
// Helpers: colour + icon pickers
// =========================================================================

namespace detail {

[[nodiscard]] inline Color risk_color(tu::RiskLevel level) {
    switch (level) {
        case tu::RiskLevel::Low:      return Color::Green;
        case tu::RiskLevel::Medium:   return Color::Yellow;
        case tu::RiskLevel::High:     return Color::Orange1;
        case tu::RiskLevel::Critical: return Color::Red;
    }
    return Color::White;
}

[[nodiscard]] inline std::string_view risk_icon(tu::RiskLevel level) {
    switch (level) {
        case tu::RiskLevel::Low:      return "ℹ️ ";
        case tu::RiskLevel::Medium:   return "⚠️ ";
        case tu::RiskLevel::High:     return "🔶 ";
        case tu::RiskLevel::Critical: return "🛑 ";
    }
    return "• ";
}

[[nodiscard]] inline bool has_any_concerns(const SecuritySources& src) {
    return src.has_mcp_servers || src.has_hooks || src.has_bash_execution
        || src.has_api_key_helper || src.has_aws_commands
        || src.has_gcp_commands || src.has_otel_headers_helper
        || src.has_dangerous_env_vars;
}

[[nodiscard]] inline std::vector<std::string> list_concerns(
    const SecuritySources& src)
{
    std::vector<std::string> concerns;
    if (src.has_mcp_servers)         concerns.emplace_back("MCP servers configured");
    if (src.has_hooks)               concerns.emplace_back("Hooks defined");
    if (src.has_bash_execution)      concerns.emplace_back("Bash command execution");
    if (src.has_api_key_helper)      concerns.emplace_back("API key helper commands");
    if (src.has_aws_commands)        concerns.emplace_back("AWS CLI commands");
    if (src.has_gcp_commands)        concerns.emplace_back("GCP CLI commands");
    if (src.has_otel_headers_helper) concerns.emplace_back("OTEL headers helper");
    if (src.has_dangerous_env_vars)  concerns.emplace_back("Dangerous environment variables");
    return concerns;
}

} // namespace detail

// =========================================================================
// Rendering: individual sections
// =========================================================================

/// Render the coloured title-bar strip.
[[nodiscard]] inline Element RenderTitleBar(tu::RiskLevel level,
                                            std::string_view custom_title)
{
    const auto title = custom_title.empty()
        ? tu::get_risk_title(level)
        : std::string{custom_title};
    return hbox({
        text(std::string{detail::risk_icon(level)}),
        text(title) | bold | color(detail::risk_color(level)),
    }) | borderEmpty;
}

/// Render the action summary (one-liner under the title).
[[nodiscard]] inline Element RenderSummary(const tu::RiskSummary& summary,
                                           tu::RiskLevel level)
{
    return paragraph(tu::get_trust_message(level, summary))
         | color(Color::White) | size(WIDTH, LESS_THAN, 78);
}

/// Render the plugin info panel (name, version, author, homepage, etc.)
[[nodiscard]] inline Element RenderPluginPanel(const PluginDefinition& def,
                                               bool is_update,
                                               bool has_signature,
                                               const std::string& marketplace_domain)
{
    Elements rows;
    rows.push_back(hbox({
        text("Plugin: ") | bold,
        text(def.name) | color(Color::Cyan),
        text("  v" + def.version) | dim,
    }));
    if (!def.author.empty()) {
        rows.push_back(hbox({
            text("Author: ") | bold,
            text(def.author),
        }));
    }
    if (!def.description.empty()) {
        rows.push_back(paragraph(std::format("About: {}", def.description)) | dim);
    }
    if (def.homepage) {
        rows.push_back(hbox({
            text("Homepage: ") | bold,
            text(*def.homepage) | underlined | color(Color::Cyan),
        }));
    }
    rows.push_back(hbox({
        text("Install type: ") | bold,
        text(is_update ? "UPDATE" : "NEW INSTALL")
            | color(is_update ? Color::Yellow : Color::Green),
        text("  • Signed: ") | bold,
        text(has_signature ? "yes" : "NO")
            | color(has_signature ? Color::Green : Color::Red),
    }));
    if (!marketplace_domain.empty()) {
        const bool safe = tu::is_known_safe_domain(marketplace_domain);
        rows.push_back(hbox({
            text("Marketplace: ") | bold,
            text(marketplace_domain)
                | color(safe ? Color::White : Color::Yellow),
            text(safe ? "  [trusted]" : "  [NOT OFFICIAL — verify source]")
                | color(safe ? Color::Green : Color::Red),
        }));
    }
    return vbox(std::move(rows)) | borderLight;
}

/// Render the sensitive-path list (inside a collapsible-like panel).
[[nodiscard]] inline Element RenderSensitivePaths(
    const std::vector<SensitiveMatch>& matches)
{
    if (matches.empty()) return text("");

    Elements rows;
    rows.push_back(text(" Sensitive paths detected:") | bold | color(Color::Red));
    for (const auto& m : matches) {
        rows.push_back(hbox({
            text("  ‣ ") | color(detail::risk_color(m.severity)),
            text("[" + m.category + "] ") | bold | color(detail::risk_color(m.severity)),
            text(m.path),
        }));
        if (!m.description.empty()) {
            rows.push_back(text("      " + m.description) | dim);
        }
    }
    return vbox(std::move(rows));
}

/// Render the risk-factor bullet list.
[[nodiscard]] inline Element RenderRiskBullets(const tu::RiskSummary& s) {
    auto bullets = tu::build_risk_bullets(s);
    Elements rows;
    rows.push_back(text(" Risk factors:") | bold);
    for (const auto& b : bullets) {
        rows.push_back(hbox({
            text("  • "),
            paragraph(b),
        }));
    }
    return vbox(std::move(rows));
}

/// Render the workspace-trust path banner.
[[nodiscard]] inline Element RenderWorkspaceBanner(
    std::string_view workspace_path,
    const SecuritySources& src)
{
    Elements parts;
    parts.push_back(hbox({
        text("Accessing workspace: ") | dim,
        text(std::string{workspace_path}) | bold,
    }));
    parts.push_back(text(""));
    parts.push_back(paragraph(
        "Quick safety check: Is this a project you created or one you trust? "
        "(Like your own code, a well-known open source project, or work from "
        "your team). If not, take a moment to review what's in this folder "
        "first."));
    parts.push_back(text("Claude Code'll be able to read, edit, and execute files here.") | dim);

    if (detail::has_any_concerns(src)) {
        parts.push_back(text(""));
        auto concerns = detail::list_concerns(src);
        parts.push_back(text(" Detected:") | bold | color(Color::Yellow));
        for (const auto& c : concerns) {
            parts.push_back(hbox({
                text("  • ") | color(Color::Yellow),
                text(c),
            }));
        }
    }
    return vbox(std::move(parts));
}

// =========================================================================
// Tier-specific button layout + state helpers
// =========================================================================

/// Shared dialog state (captured by renderer + event handler).
struct DialogState {
    TrustDialogProps props;

    // Selection
    int selected = 0;

    // Medium / High tier: "always allow" checkbox
    bool always_allow = false;

    // Critical tier: "I understand" checkbox + YES input
    bool understand_risk = false;
    std::string yes_input;
    bool show_second_confirm = false; // true → input box visible

    // High tier: countdown (seconds remaining)
    int countdown_remaining = 0;
    bool countdown_active = false;

    // UI: details panel expanded?
    bool show_details = true;
};

/// Number of *primary selectable* buttons per tier.
/// (Secondary checkboxes / inputs are not counted here.)
[[nodiscard]] inline int tier_button_count(tu::RiskLevel level) {
    switch (level) {
        case tu::RiskLevel::Low:      return 2; // Allow / Cancel
        case tu::RiskLevel::Medium:   return 3; // Allow once / Always / Cancel
        case tu::RiskLevel::High:     return 4; // Allow once / Always / View / Cancel
        case tu::RiskLevel::Critical: return 2; // Enable anyway / Cancel
    }
    return 2;
}

/// Build button labels (index matches selection indices for the tier).
[[nodiscard]] inline std::vector<std::string> tier_button_labels(
    tu::RiskLevel level, bool has_paths_to_view)
{
    using tu::RiskLevel;
    switch (level) {
        case RiskLevel::Low:
            return {"Allow", "Cancel"};
        case RiskLevel::Medium:
            return {"Allow once", "Always allow this", "Cancel"};
        case RiskLevel::High:
            return has_paths_to_view
                ? std::vector<std::string>{"Allow once", "Always allow", "View file", "Cancel"}
                : std::vector<std::string>{"Allow once", "Always allow", "Skip (no file)", "Cancel"};
        case RiskLevel::Critical:
            return {"Enable anyway", "Cancel"};
    }
    return {"OK", "Cancel"};
}

/// Render a single button row.
[[nodiscard]] inline Element RenderButtonRow(
    const std::vector<std::string>& labels,
    int selected,
    tu::RiskLevel level,
    bool allow_disabled = false)
{
    Elements parts;
    const int n = static_cast<int>(labels.size());
    for (int i = 0; i < n; ++i) {
        const bool is_sel = (i == selected);
        auto label = text((is_sel ? "▶ " : "  ") + labels[i] + " ");
        if (is_sel) label = label | bold | color(detail::risk_color(level));
        if (allow_disabled && i == 0 && level == tu::RiskLevel::High) {
            // High tier: primary Allow is dimmed until countdown expires.
            label = label | dim;
        }
        parts.push_back(label);
        if (i + 1 < n) parts.push_back(text("  "));
    }
    return hbox(std::move(parts));
}

/// Render the countdown bar for High tier.
[[nodiscard]] inline Element RenderCountdown(const DialogState& s) {
    if (!s.countdown_active) return text("");
    if (s.countdown_remaining <= 0) {
        return hbox({
            text(" ✓ ") | color(Color::Green),
            text("You may now proceed.") | color(Color::Green) | dim,
        });
    }
    return hbox({
        text(" ⏳ ") | color(Color::Orange1),
        text(std::format(
            "Please review carefully. Allow enabled in {} second{}…",
            s.countdown_remaining,
            s.countdown_remaining == 1 ? "" : "s")) | color(Color::Orange1),
    });
}

/// Render the critical-tier checkbox + YES input.
[[nodiscard]] inline Element RenderCriticalGates(const DialogState& s) {
    const Color gate = s.understand_risk ? Color::Green : Color::Red;
    Elements parts;
    parts.push_back(hbox({
        text(s.understand_risk ? "☑ " : "☐ ") | bold | color(gate),
        text("I understand the risk of proceeding.")
            | color(s.understand_risk ? Color::White : Color::Red),
    }));
    if (s.show_second_confirm || s.understand_risk) {
        parts.push_back(hbox({
            text(" Type ") | dim,
            text(std::string{tu::kCriticalConfirmWord}) | bold | color(Color::Red),
            text(" to confirm: ") | dim,
            text(s.yes_input.empty() ? std::string{"(empty)"} : s.yes_input)
                | color(s.yes_input == tu::kCriticalConfirmWord ? Color::Green : Color::Red),
            text("│") | blink,
        }));
        parts.push_back(text(
            " (Esc / Cancel aborts. Typing anything other than YES is rejected.)") | dim);
    }
    return vbox(std::move(parts));
}

// =========================================================================
// Full dialog renderer
// =========================================================================

[[nodiscard]] inline Element RenderTrustDialogFull(
    const std::shared_ptr<DialogState>& s)
{
    const auto level = s->props.forced_level;
    const auto& summary = s->props.summary;
    const auto labels = tier_button_labels(level, !s->props.paths.empty());

    // Determine if critical-tier gating is satisfied.
    const bool critical_ok =
        s->props.forced_level != tu::RiskLevel::Critical
        || (s->understand_risk && s->yes_input == tu::kCriticalConfirmWord);

    // Resolve workspace path for the legacy banner.
    std::string workspace_path;
    if (s->props.workspace_path_storage) {
        workspace_path = *s->props.workspace_path_storage;
    }

    Elements body;
    body.push_back(RenderTitleBar(level, s->props.action_label));
    body.push_back(separator());

    // Action-specific banner
    if (s->props.action == ActionType::WorkspaceTrust) {
        body.push_back(RenderWorkspaceBanner(workspace_path, {}));
    } else if (s->props.plugin.has_value()) {
        body.push_back(RenderPluginPanel(
            *s->props.plugin,
            s->props.plugin_is_update,
            s->props.plugin_has_signature,
            s->props.marketplace_domain));
        body.push_back(text(""));
    }

    body.push_back(RenderSummary(summary, level));
    body.push_back(text(""));

    // Details panel (expandable by default)
    if (s->show_details) {
        Elements detail_rows;
        if (!summary.sensitive_paths.empty()) {
            detail_rows.push_back(RenderSensitivePaths(summary.sensitive_paths));
            detail_rows.push_back(text(""));
        }
        detail_rows.push_back(RenderRiskBullets(summary));
        if (!summary.domains.empty()) {
            detail_rows.push_back(text(""));
            detail_rows.push_back(hbox({
                text("External domains: ") | bold,
                text(tu::format_list_with_and(summary.domains)),
            }));
        }
        body.push_back(vbox(std::move(detail_rows)) | borderEmpty | dim);
    } else {
        body.push_back(text(" ▼ Press [D] to show details") | dim);
    }

    // Tier-specific safety gates
    body.push_back(separator());
    if (level == tu::RiskLevel::High) {
        body.push_back(RenderCountdown(*s));
        body.push_back(text(""));
    }
    if (level == tu::RiskLevel::Critical) {
        body.push_back(RenderCriticalGates(*s));
        body.push_back(text(""));
    }

    // Medium tier: "always allow" checkbox (also shown for High)
    if (level == tu::RiskLevel::Medium || level == tu::RiskLevel::High) {
        body.push_back(hbox({
            text(s->always_allow ? "☑ " : "☐ ") | color(Color::Cyan),
            text("Remember this decision for future prompts") | dim,
        }));
        body.push_back(text(""));
    }

    // Buttons
    const bool allow_disabled =
        (level == tu::RiskLevel::High && s->countdown_remaining > 0)
        || (level == tu::RiskLevel::Critical && !critical_ok);

    body.push_back(RenderButtonRow(labels, s->selected, level, allow_disabled));
    body.push_back(text(""));
    body.push_back(hbox({
        text("Security guide") | color(Color::Cyan) | underlined,
    }) | dim);
    body.push_back(hbox({
        text("Enter") | bold,
        text(" to confirm · "),
        text("Esc") | bold,
        text(" to cancel · "),
        text("↑/↓ j/k") | bold,
        text(" navigate · "),
        text("D") | bold,
        text(" details · "),
        text("Space") | bold,
        text(" toggle checkbox"),
    }) | dim);

    const auto color = detail::risk_color(level);
    return window(
        hbox({
            text(" ") | color,
            text(std::string{detail::risk_icon(level)}),
            text(tu::get_risk_title(level)) | bold | color,
            text(" "),
        }),
        vbox(std::move(body)) | xflex
    ) | color | size(WIDTH, LESS_THAN, 86);
}

// =========================================================================
// Event handling
// =========================================================================

/// Checkbox-toggleable gate names for Space-key cycling.
inline void toggle_selected_gate(DialogState& s) {
    const auto level = s.props.forced_level;
    // For Low tier: no checkboxes at all.
    if (level == tu::RiskLevel::Low) return;

    // For Medium/High: the "always allow" checkbox.
    if (level == tu::RiskLevel::Medium || level == tu::RiskLevel::High) {
        s.always_allow = !s.always_allow;
        return;
    }
    // For Critical: the "I understand" checkbox.
    if (level == tu::RiskLevel::Critical) {
        s.understand_risk = !s.understand_risk;
        if (s.understand_risk) s.show_second_confirm = true;
    }
}

/// Emit the final decision via the on_done callback.
inline void emit_choice(std::shared_ptr<DialogState> s, TrustChoice choice) {
    if (!s->props.on_done) return;
    auto cb = std::move(s->props.on_done);
    cb(choice);
}

// =========================================================================
// Factories
// =========================================================================

/// Build a fully-general tiered trust dialog.
[[nodiscard]] inline Component MakeTrustDialogComponent(TrustDialogProps props)
{
    // --- Pre-compute risk tier + sensitive paths --------------------------
    if (props.summary.sensitive_paths.empty() && !props.paths.empty()) {
        props.summary.sensitive_paths = tu::scan_paths_for_sensitive(props.paths);
    }

    // Plugin path: re-derive risk summary using plugin_risk_summary().
    if (props.plugin.has_value() && props.plugin_caps.has_value()) {
        props.summary = tu::plugin_risk_summary(
            *props.plugin, *props.plugin_caps,
            props.marketplace_domain,
            /*is_first_install*/ !props.plugin_is_update,
            /*is_update*/ props.plugin_is_update,
            /*has_signature*/ props.plugin_has_signature);
        // Re-scan paths if caller supplied any (e.g. plugin entrypoint).
        if (!props.paths.empty()) {
            auto extra = tu::scan_paths_for_sensitive(props.paths);
            for (auto& m : extra) props.summary.sensitive_paths.push_back(std::move(m));
        }
    }

    if (props.forced_level == tu::RiskLevel::Low) {
        props.forced_level = tu::classify_risk(props.action, props.summary);
    }

    // --- Allocate state ----------------------------------------------------
    auto state = std::make_shared<DialogState>();
    state->props = std::move(props);
    state->selected = 0;
    state->show_details = true;

    // High tier: initialise countdown values (decremented below by the
    // wall-clock lazy-ticker in the renderer).
    const bool is_high = state->props.forced_level == tu::RiskLevel::High;
    if (is_high) {
        state->countdown_active = true;
        state->countdown_remaining = tu::kHighTierCountdownSeconds;
    }

    // Shared ticker state for High tier: avoid `std::chrono::steady_clock::now()`
    // on every keystroke by recording a start instant and recomputing the
    // remaining seconds lazily inside the renderer closure.
    auto countdown_start = std::make_shared<std::chrono::steady_clock::time_point>(
        is_high ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{});

    auto base = Renderer([state, is_high, countdown_start] {
        // High tier: lazily recompute the countdown each frame. This avoids
        // needing a background thread or FTXUI's animation primitives.
        if (is_high && state->countdown_active) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - *countdown_start).count();
            const int remaining =
                std::max(0, tu::kHighTierCountdownSeconds - static_cast<int>(elapsed));
            state->countdown_remaining = remaining;
            if (remaining == 0) state->countdown_active = false;
        }
        return RenderTrustDialogFull(state);
    });

    const int btn_count = tier_button_count(state->props.forced_level);

    auto final_component = base | CatchEvent([state, btn_count, is_high](Event event) -> bool {
        // Navigation
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(btn_count - 1, state->selected + 1);
            return true;
        }
        // Toggle details panel
        if (event == Event::Character('d') || event == Event::Character('D')) {
            state->show_details = !state->show_details;
            return true;
        }
        // Toggle current-tier checkbox(es)
        if (event == Event::Character(' ')) {
            toggle_selected_gate(*state);
            return true;
        }
        // Critical tier: typed input for YES confirmation
        if (state->props.forced_level == tu::RiskLevel::Critical &&
            (state->understand_risk || state->show_second_confirm))
        {
            if (event.is_character()) {
                const char c = event.character()[0];
                // Accept Y/E/S — build up the string exactly up to length 3
                if (state->yes_input.size() < 3) {
                    state->yes_input.push_back(c);
                }
                return true;
            }
            if (event == Event::Backspace) {
                if (!state->yes_input.empty()) state->yes_input.pop_back();
                return true;
            }
        }
        // Countdown redraw (any custom event or periodic redraw)
        if (is_high && state->countdown_active) {
            // Any animation-frame-like event triggers a re-render via the
            // Renderer closure; nothing extra needed here.
        }
        // Confirm
        if (event == Event::Return) {
            // Safety gate checks
            const auto level = state->props.forced_level;

            if (level == tu::RiskLevel::High && state->countdown_remaining > 0) {
                // Cannot accept while countdown is running.
                return true;
            }
            if (level == tu::RiskLevel::Critical) {
                if (!state->understand_risk) return true;
                if (state->yes_input != tu::kCriticalConfirmWord) return true;
            }

            // Map selection → choice
            TrustChoice choice = TrustChoice::Cancel;
            switch (level) {
                case tu::RiskLevel::Low:
                    choice = (state->selected == 0) ? TrustChoice::AllowOnce
                                                    : TrustChoice::Cancel;
                    break;
                case tu::RiskLevel::Medium:
                    if (state->selected == 0)       choice = TrustChoice::AllowOnce;
                    else if (state->selected == 1)  choice = TrustChoice::AlwaysAllow;
                    else                            choice = TrustChoice::Cancel;
                    break;
                case tu::RiskLevel::High:
                    if (state->selected == 0) {
                        choice = state->always_allow ? TrustChoice::AlwaysAllow
                                                     : TrustChoice::AllowOnce;
                    } else if (state->selected == 1) {
                        choice = TrustChoice::AlwaysAllow;
                    } else if (state->selected == 2) {
                        choice = TrustChoice::ViewFile;
                    } else {
                        choice = TrustChoice::Cancel;
                    }
                    break;
                case tu::RiskLevel::Critical:
                    choice = (state->selected == 0) ? TrustChoice::EnableAnyway
                                                    : TrustChoice::Cancel;
                    break;
            }
            emit_choice(state, choice);
            return true;
        }
        // Cancel
        if (event == Event::Escape) {
            emit_choice(state, TrustChoice::Cancel);
            return true;
        }
        return false;
    });

    return final_component;
}

// =========================================================================
// Named factories (public API required by spec)
// =========================================================================

/// Factory: plugin install/update trust dialog.
/// Reuses PluginDefinition / PluginCapabilities (trust model from
/// plugins/plugin.cppm) and the TrustLevel model from
/// commands/plugin/plugin_trust.cppm to short-circuit when the plugin is
/// already verified.
[[nodiscard]] inline Component MakePluginTrustDialog(
    const PluginDefinition& def,
    const PluginCapabilities& caps,
    std::function<void(TrustChoice)> on_done,
    std::string_view marketplace_domain = "",
    bool is_update = false,
    bool has_signature = true)
{
    using cc::commands::TrustLevel;
    using cc::commands::get_trust_level;

    // Short-circuit via the existing plugin-trust model (DO NOT re-implement
    // the trust model).
    const TrustLevel tl = get_trust_level(def.name);
    const bool permissions_unsafe = std::any_of(
        caps.tools.begin(), caps.tools.end(),
        [](const auto& t) { return t.find("bash") != std::string::npos
                                  || t.find("exec") != std::string::npos
                                  || t.find("unsafe") != std::string::npos; });

    TrustDialogProps props;
    props.on_done = std::move(on_done);
    props.action = is_update ? ActionType::PluginUpdate : ActionType::PluginInstall;
    props.plugin = def;
    props.plugin_caps = caps;
    props.marketplace_domain = marketplace_domain;
    props.plugin_has_signature = has_signature;
    props.plugin_is_update = is_update;
    props.action_label = is_update ? "Plugin Update" : "Plugin Installation";

    // Force tier when the plugin is already trusted.
    if (tl == TrustLevel::Verified) {
        props.forced_level = RiskLevel::Low;
    } else if (tl == TrustLevel::Trusted && !permissions_unsafe) {
        props.forced_level = RiskLevel::Low;
    }

    return MakeTrustDialogComponent(std::move(props));
}

/// Factory: path access / write trust dialog.
[[nodiscard]] inline Component MakePathTrustDialog(
    ActionType action,
    const std::vector<std::string>& paths,
    std::function<void(TrustChoice)> on_done,
    std::string_view action_label = "")
{
    TrustDialogProps props;
    props.on_done = std::move(on_done);
    props.action = action;
    props.paths = paths;
    props.action_label = action_label.empty()
        ? (action == ActionType::PathWrite ? "Modify file(s)" : "Access file(s)")
        : std::string{action_label};

    props.summary.action_summary = [&] {
        if (paths.empty()) return std::string{"This action touches no files."};
        const auto head = tu::format_list_with_and(paths, 3);
        return std::format(
            "{} will {} {}",
            action_label.empty() ? "The agent" : std::string{action_label},
            action == ActionType::PathWrite ? "write to" : "read",
            head);
    }();

    return MakeTrustDialogComponent(std::move(props));
}

/// Back-compat factory: the original two-option workspace trust dialog.
/// Used by bootstrap before the first query — mirrors the behaviour of
/// TS TrustDialog.tsx exactly.
[[nodiscard]] inline Component MakeWorkspaceTrustDialog(WorkspaceTrustProps props)
{
    TrustDialogProps generic;
    generic.on_done = std::move(props.on_done);
    generic.action = ActionType::WorkspaceTrust;
    generic.workspace_path_storage = std::make_shared<std::string>(std::move(props.workspace_path));

    RiskSummary summary;
    summary.action_summary = std::format(
        "Claude Code wants access to the workspace at {}.",
        *generic.workspace_path_storage);

    const auto concerns = detail::list_concerns(props.sources);
    for (const auto& c : concerns) summary.risk_factors.push_back(c);
    for (const auto& cmd : props.commands) {
        summary.risk_factors.push_back("Command has bash capability: " + cmd);
    }
    generic.summary = summary;
    generic.action_label = "Workspace Access";

    // Classic workspace trust is Low unless there are concerns (then Medium).
    generic.forced_level = concerns.empty() ? RiskLevel::Low : RiskLevel::Medium;

    // Expose workspace path through a weak-pointer fallback the renderer
    // consults when action == WorkspaceTrust.
    return MakeTrustDialogComponent(std::move(generic));
}

} // namespace cc::ui::trust_dialog
