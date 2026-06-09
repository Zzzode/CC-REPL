/// @file prompt_widgets.cppm
/// @brief Small overlay widgets rendered around the Prompt text input:
///   - 3-column help menu (prefix hints · global shortcuts · edit shortcuts)
///   - Notifications row (API key status · IDE status · overage notice ·
///     memory indicator · sandbox hint · auto-updater banner)
///   - Voice indicator (microphone + VU-wave animation, Element-based)
///   - Queued / pending commands list
///
/// Each widget is a self-contained Element renderer + optional
/// interactive Component wrapper. Keeping all four in a single file
/// respects the 1000-line cap while grouping related UI surface.
///
/// Migrated from (TS → C++ FTXUI):
///   PromptInputHelpMenu.tsx       (357 lines, 3-column keyboard help)
///   Notifications.tsx             (331 lines, 7 sub-badges)
///   voice_indicator.cppm (upgrade: from ANSI to FTXUI Element)
///   prompt_queued_commands.cppm   (upgrade: Component + badges)
module;

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cmath>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.prompt.prompt_widgets;

import ui.components.text_input;

export namespace cc::ui::prompt::widgets {
using namespace ftxui;
using ui::components::PermissionMode;
using ui::components::PromptContext;

// ============================================================
// 1. Help Menu  (3-column keyboard shortcut overlay)
// ============================================================

/// A single row in the help menu: prefix hint | global | edit
struct HelpMenuRow {
    std::string prefix;     /// Leading trigger chars (e.g. "/commit")
    std::string global;     /// Global shortcut column (e.g. "Ctrl+C")
    std::string edit;       /// Edit shortcut column (e.g. "Ctrl+A")
};

/// Build the default set of help rows (matches TS HelpMenu columns).
/// Kept as a free function so callers can filter / extend per product.
[[nodiscard]] inline std::vector<HelpMenuRow> DefaultHelpMenuRows() {
    return {
        // ---- Prefix Hints ----     ---- Global ----   ---- Edit ----
        { "/help",                    "Ctrl+C  abort",    "Ctrl+A  select all" },
        { "/compact   compact sess",  "Ctrl+D  exit",     "Ctrl+Z  undo"       },
        { "/commit    create commit", "Ctrl+L  clear",    "Ctrl+Y  redo"       },
        { "/review    review PR",     "Ctrl+R  reset",    "Ctrl+V  paste"      },
        { "/mcp       MCP menu",      "Ctrl+T  tasks",    "Ctrl+←  word-left"  },
        { "/config    settings",      "Ctrl+G  teams",    "Ctrl+→  word-right" },
        { "/doctor    healthcheck",   "Ctrl+O  session",  "Shift+↑/↓ extend sel" },
        { "!command   run shell",     "Ctrl+S  stash",    "Tab     history / accept" },
        { "@path      include file",  "Esc     cancel",   "Shift+Tab cycle mode" },
        { "@*agent    mention agent", "Ctrl+P  PR view",  "Enter   send (soft: ctrl+enter)" },
    };
}

/// Render the 3-column help menu. Matches the TS `PromptInputHelpMenu`
/// visual style: Cyan bold title, 3 aligned columns with dim separators,
/// dim header row, rounded border.
[[nodiscard]] inline Element RenderHelpMenu(
    const std::vector<HelpMenuRow>& rows = DefaultHelpMenuRows()) {

    Elements lines;
    lines.push_back(hbox({
        text(" Prompt Help ") | bold | color(Color::Cyan),
        filler(),
        text(" (press Esc or ? to close) ") | dim,
    }));
    lines.push_back(separator() | dim);

    // Header row
    lines.push_back(hbox({
        text("  Prefix") | bold | color(Color::Yellow),
        filler(),
        text("│") | dim,
        text(" Global ") | bold | color(Color::Yellow),
        filler(),
        text("│") | dim,
        text(" Edit ") | bold | color(Color::Yellow),
    }) | size(WIDTH, GREATER_THAN, 72));

    lines.push_back(separatorLight() | dim);

    for (const auto& r : rows) {
        lines.push_back(hbox({
            text("  " + r.prefix) | color(Color::Green) | size(WIDTH, EQUAL, 26),
            text("│") | dim,
            text(" " + r.global) | color(Color::White) | size(WIDTH, EQUAL, 24),
            text("│") | dim,
            text(" " + r.edit) | dim,
        }) | size(WIDTH, GREATER_THAN, 72));
    }

    return vbox(lines) | border | bgcolor(Color::RGB(20, 22, 28));
}

/// Wrap RenderHelpMenu() in a Component that closes on Escape / '?'.
[[nodiscard]] inline Component HelpMenuOverlay() {
    auto open = std::make_shared<bool>(true);
    return Renderer([open] { return *open ? RenderHelpMenu() : text(""); })
         | CatchEvent([open](Event e) {
             if (!*open) return false;
             if (e == Event::Escape ||
                 (e.is_character() && e.character() == "?")) {
                 *open = false;
                 return true;
             }
             return false;
         });
}

// ============================================================
// 2. Notifications Row
// ============================================================

/// Status of the API key used to render the top-right badge.
enum class ApiKeyStatus : std::uint8_t {
    Valid,        // Green dot
    Missing,      // Red "!"
    Invalid,      // Orange "?"
    Overage,      // Yellow overage banner
};

/// IDE bridge status (for IdeStatusIndicator in Notifications.tsx).
struct IdeBridgeStatus {
    bool connected = false;
    std::string ide_name;    // e.g. "vscode", "jetbrains"
    int pending_requests = 0;
};

/// Memory usage (from TS MemoryUsageIndicator).
struct MemoryUsageInfo {
    double percent = 0.0;    /// 0.0 .. 1.0
    std::int64_t rss_mb = 0;
};

/// All inputs the notifications row consumes.
struct NotificationsOptions {
    // -- API / account --
    ApiKeyStatus api_key = ApiKeyStatus::Valid;
    std::optional<double> cost_usd;

    // -- IDE --
    std::optional<IdeBridgeStatus> ide;

    // -- system --
    std::optional<MemoryUsageInfo> memory;

    // -- from PromptContext --
    std::optional<std::reference_wrapper<const PromptContext>> context;

    // -- misc --
    bool show_auto_updater = false;
    std::optional<std::string> auto_updater_text; // e.g. "v2.5.0 available"
    std::optional<std::string> voice_error;

    /// Called when user clicks the upgrade banner.
    std::function<void()> on_upgrade;
    /// Called when user clicks the IDE status pill.
    std::function<void()> on_ide_toggle;
};

// -- sub-badge helpers --------------------------------------------------

[[nodiscard]] inline Element api_key_badge(ApiKeyStatus s) {
    switch (s) {
        case ApiKeyStatus::Valid:
            return hbox({ text("●") | color(Color::Green) | dim,
                          text(" key ok") | dim });
        case ApiKeyStatus::Missing:
            return hbox({ text("!") | bold | color(Color::Red),
                          text(" API key missing") | color(Color::Red) });
        case ApiKeyStatus::Invalid:
            return hbox({ text("?") | bold | color(Color::Yellow),
                          text(" API key invalid") | color(Color::Yellow) });
        case ApiKeyStatus::Overage:
            return hbox({ text("⚠") | bold | color(Color::YellowLight),
                          text(" rate limited") | color(Color::YellowLight) | dim });
    }
    return text("");
}

[[nodiscard]] inline Element ide_status_badge(
    const std::optional<IdeBridgeStatus>& ide) {
    if (!ide) return text("");
    if (!ide->connected) {
        return text(" ⌁ ide offline") | dim | color(Color::GrayDark);
    }
    std::string label = " ⌁ " + ide->ide_name;
    if (ide->pending_requests > 0) {
        label += std::format(" ({} pending)", ide->pending_requests);
    }
    return text(label) | color(Color::BlueLight) | dim;
}

[[nodiscard]] inline Element memory_badge(
    const std::optional<MemoryUsageInfo>& mem) {
    if (!mem) return text("");
    int pct = static_cast<int>(std::round(mem->percent * 100.0));
    Color c = (pct >= 85) ? Color::Red
            : (pct >= 65) ? Color::Yellow
            : Color::GrayLight;
    // A compact horizontal "meter" using block segments; fallback to text
    // if percentage is absurdly out of range.
    std::string meter;
    const char* block = "█";
    int filled = std::clamp(pct / 10, 0, 10);
    meter.append(filled, block[0]);
    meter.append(std::max(0, 10 - filled), '░');
    return hbox({
        text(" "),
        text(meter) | color(c) | dim,
        text(std::format(" {}% ({}MB)", pct, mem->rss_mb)) | dim | color(c),
    });
}

[[nodiscard]] inline Element overage_banner(const PromptContext* ctx) {
    if (!ctx || !ctx->in_overage_mode) return text("");
    return hbox({
        text(" ⚠ ") | bold | color(Color::YellowLight),
        text("overage — responses may be delayed")
            | color(Color::YellowLight) | dim,
    });
}

[[nodiscard]] inline Element cost_badge(const NotificationsOptions& o) {
    if (!o.cost_usd) return text("");
    // Color by cost bucket so high-usage is visible at a glance.
    double c = *o.cost_usd;
    Color clr = (c > 5.0) ? Color::Red
              : (c > 1.0) ? Color::Yellow
              : Color::Green;
    return hbox({
        text(" "),
        text(std::format("${:.3f}", c)) | color(clr) | dim,
    });
}

[[nodiscard]] inline Element sandbox_badge(const PromptContext* ctx) {
    if (!ctx || !ctx->show_sandbox_hint) return text("");
    return text(" [sandboxed]") | color(Color::Yellow) | dim;
}

[[nodiscard]] inline Element upgrade_banner(const NotificationsOptions& o) {
    if (!o.show_auto_updater || !o.auto_updater_text) return text("");
    return hbox({
        text(" ⇪ ") | bold | color(Color::Cyan),
        text(*o.auto_updater_text) | color(Color::Cyan) | dim,
        text(" — press U to upgrade") | dim,
    });
}

[[nodiscard]] inline Element voice_error_badge(
    const std::optional<std::string>& err) {
    if (!err || err->empty()) return text("");
    return hbox({
        text(" 🎤⚠ ") | bold | color(Color::RedLight),
        text(*err) | color(Color::RedLight) | dim,
    });
}

/// Render the full notifications row. Items are packed left-to-right;
/// empty badges are simply omitted so the row only takes as much space
/// as it really needs (matches TS: conditional rendering per badge).
[[nodiscard]] inline Element RenderNotificationsRow(
    const NotificationsOptions& opts) {

    const PromptContext* ctx =
        opts.context.has_value() ? &opts.context->get() : nullptr;

    Elements parts;
    parts.push_back(api_key_badge(opts.api_key));
    parts.push_back(ide_status_badge(opts.ide));
    parts.push_back(overage_banner(ctx));
    parts.push_back(memory_badge(opts.memory));
    parts.push_back(cost_badge(opts));
    parts.push_back(sandbox_badge(ctx));
    parts.push_back(upgrade_banner(opts));
    parts.push_back(voice_error_badge(opts.voice_error));

    // Drop empty elements from the hbox (FTXUI will still render them as
    // width-0 spacing; we want zero footprint for missing badges).
    Elements filtered;
    for (auto& p : parts) {
        if (p) filtered.push_back(std::move(p));
    }
    if (filtered.empty()) return text("");
    return hbox(filtered);
}

// ============================================================
// 3. Voice Indicator (microphone + VU-wave · Element-based upgrade)
// ============================================================

/// Mirrors cc::ui::prompt::VoiceState from voice_indicator.cppm so we can
/// render the same state machine without importing that ANSI module.
enum class VoiceWidgetState : std::uint8_t {
    Idle,
    Listening,
    Processing,
    Speaking,
};

/// Render a voice-indicator Element. `frame` should be a monotonically
/// increasing integer (e.g. FTXUI ScreenInteractive frames, or caller
/// tick counter at ~30fps). `levels` is optional audio level data.
[[nodiscard]] inline Element RenderVoiceIndicator(
    VoiceWidgetState state,
    int frame,
    const std::vector<float>& levels = {}) {

    switch (state) {
        case VoiceWidgetState::Idle:
            return hbox({ text("🎤 ") | dim, text("") });

        case VoiceWidgetState::Listening: {
            // Pulsing mic
            static constexpr std::array<const char*, 4> pulse =
                {"🎤", "🎤 ", "🎤  ", "🎤 "};
            int p = (frame / 4) % 4;
            // Block-char wave (mirrors TS `AudioWaveform` component)
            static constexpr std::array<const char*, 9> bars =
                {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
            std::string wave;
            int bars_count = 8;
            for (int i = 0; i < bars_count; ++i) {
                size_t li = levels.empty() ? 0
                    : static_cast<size_t>((i * levels.size()) / bars_count);
                float lvl = levels.empty()
                    ? 0.25f + 0.5f * std::abs(std::sin(frame * 0.2f + i))
                    : levels[li];
                int b = std::clamp(static_cast<int>(lvl * 8.0f), 0, 8);
                wave += bars[b];
            }
            return hbox({
                text(pulse[p]) | bold | color(Color::Red),
                text(" LISTEN ") | bold | color(Color::Red),
                text(wave) | color(Color::RedLight),
            });
        }

        case VoiceWidgetState::Processing: {
            static constexpr std::array<const char*, 8> spinner =
                {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"};
            int s = frame % 8;
            return hbox({
                text("🎤 ") | dim | color(Color::Yellow),
                text(spinner[s]) | color(Color::Yellow),
                text(" transcribing…") | color(Color::Yellow) | dim,
            });
        }

        case VoiceWidgetState::Speaking: {
            static constexpr std::array<const char*, 3> speak =
                {"🔊", "🔊 )", "🔊 ))"};
            int s = (frame / 3) % 3;
            return hbox({
                text(speak[s]) | color(Color::GreenLight),
                text(" reply") | color(Color::GreenLight) | dim,
            });
        }
    }
    return text("");
}

/// Convenience: render a short, 1-cell voice indicator for the footer.
[[nodiscard]] inline Element RenderVoiceFooterGlyph(VoiceWidgetState s) {
    switch (s) {
        case VoiceWidgetState::Idle:       return text("🎤") | dim;
        case VoiceWidgetState::Listening:  return text("🎤") | color(Color::Red) | bold;
        case VoiceWidgetState::Processing: return text("🎤") | color(Color::Yellow) | dim;
        case VoiceWidgetState::Speaking:   return text("🔊") | color(Color::Green);
    }
    return text("");
}

// ============================================================
// 4. Queued / Pending Commands List
// ============================================================

/// A single queued command entry. Matches TS `queuedCommands`.
struct QueuedCommandEntry {
    std::string text;
    bool is_executing = false;
    std::optional<int> eta_seconds;
    std::optional<std::string> tool_tag;  // e.g. "[bash]", "[file_write]"
};

/// Render the queued-commands list as an Element.
/// Matches prompt_queued_commands.cppm rendering but adds: ETA display,
/// tool tags, and a header row.
[[nodiscard]] inline Element RenderQueuedCommands(
    const std::vector<QueuedCommandEntry>& cmds) {
    if (cmds.empty()) return text("");

    Elements rows;
    rows.push_back(hbox({
        text(std::format(" Queued ({}) ", cmds.size())) | bold | dim
            | color(Color::Cyan),
        filler(),
    }));
    rows.push_back(separatorLight() | dim);

    for (const auto& c : cmds) {
        Elements parts;
        parts.push_back(text(c.is_executing ? " ● " : "   ")
                        | color(c.is_executing ? Color::Green : Color::GrayDark));
        if (c.tool_tag) {
            parts.push_back(text(*c.tool_tag + " ")
                            | color(Color::Yellow) | dim);
        }
        parts.push_back(text(c.text) | color(c.is_executing ? Color::White : Color::GrayLight));
        if (c.eta_seconds) {
            parts.push_back(filler());
            parts.push_back(
                text(std::format(" ~{}s", *c.eta_seconds)) | dim);
        }
        rows.push_back(hbox(parts));
    }

    return vbox(rows) | borderLight | bgcolor(Color::RGB(18, 20, 26));
}

// ============================================================
// Convenience: compose all widgets into a single Component
// ============================================================

/// Options for the all-in-one PromptWidgets component.
struct PromptWidgetsOptions {
    NotificationsOptions notifications;
    std::vector<HelpMenuRow> help_rows = DefaultHelpMenuRows();
    VoiceWidgetState voice_state = VoiceWidgetState::Idle;
    std::vector<float> audio_levels;
    std::vector<QueuedCommandEntry> queued_commands;
    bool show_help_menu = false;

    // Callbacks
    std::function<void()> on_help_close;
    std::function<void(const QueuedCommandEntry&)> on_queued_click;
};

/// Compose notifications row + queued commands + optional help menu
/// + voice indicator into a single vertical layout. This is the widget
/// that the top-level PromptInput composer plugs in around the text
/// input area.
[[nodiscard]] inline Component PromptWidgets(PromptWidgetsOptions opts_in) {
    auto state = std::make_shared<PromptWidgetsOptions>(std::move(opts_in));
    auto frame = std::make_shared<int>(0);

    return Renderer([state, frame] {
        ++(*frame);
        Elements stack;

        // Help menu overlay (top)
        if (state->show_help_menu) {
            stack.push_back(RenderHelpMenu(state->help_rows));
            stack.push_back(text(" "));
        }

        // Notifications row (above voice + queued)
        stack.push_back(RenderNotificationsRow(state->notifications));

        // Voice indicator (left) + queued commands (below), side-by-side if small
        Element voice = RenderVoiceIndicator(
            state->voice_state, *frame, state->audio_levels);
        Element queued = RenderQueuedCommands(state->queued_commands);

        Elements bottom;
        bottom.push_back(voice);
        bottom.push_back(filler());
        // queued commands live below when present
        stack.push_back(hbox(bottom));
        if (!state->queued_commands.empty()) {
            stack.push_back(queued);
        }

        return vbox(stack);
    }) | CatchEvent([state](Event e) -> bool {
        // Esc closes help menu
        if (state->show_help_menu && e == Event::Escape) {
            state->show_help_menu = false;
            if (state->on_help_close) state->on_help_close();
            return true;
        }
        // '?' toggles help menu
        if (e.is_character() && e.character() == "?") {
            state->show_help_menu = !state->show_help_menu;
            if (!state->show_help_menu && state->on_help_close)
                state->on_help_close();
            return true;
        }
        // 'U' triggers upgrade (auto-updater banner)
        if (state->notifications.show_auto_updater &&
            e.is_character() && (e.character() == "U" || e.character() == "u")) {
            if (state->notifications.on_upgrade) {
                state->notifications.on_upgrade();
                return true;
            }
        }
        return false;
    });
}

} // namespace cc::ui::prompt::widgets
