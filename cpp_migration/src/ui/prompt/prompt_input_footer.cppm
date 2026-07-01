/// =========================================================================
/// @file prompt_input_footer.cppm
/// @brief Faithful C++/FTXUI port of TS `PromptInputFooter.tsx` and
///        `PromptInputFooterLeftSide.tsx` — the area below the prompt input
///        with left/right columns containing mode indicators, tasks, teams,
///        hints, and status info.
///
/// MODULE:   cc.ui.prompt.prompt_input_footer
/// LICENCE:  Exported.  Imported by repl_screen.cppm (bottom slot assembly).
///
/// TS REFERENCE STRUCTURE (PromptInputFooter.tsx):
///   <Box flexDirection="row" justifyContent="space-between">
///     <Box flexDirection="column">              // Left column
///       {StatusLine}                             // optional, user-configurable
///       <PromptInputFooterLeftSide />
///     </Box>
///     <Box>                                      // Right column
///       {Notifications}                          // non-fullscreen only
///       {undercover}                             // ant-only
///       <BridgeStatusIndicator />
///     </Box>
///   </Box>
///   {CoordinatorTaskPanel}                      // ant-only
///
/// TS REFERENCE (PromptInputFooterLeftSide.tsx):
///   - Exit message (dim: "Press <key> again to exit")
///   - Pasting indicator (dim: "Pasting text…")
///   - HistorySearchInput (when isSearching)
///   - Vim INSERT badge (dim: "-- INSERT --")
///   - ModeIndicator (permission mode, tasks, teams, PR, hints)
///
/// STABLE HEIGHT: In fullscreen the bottom slot is flexShrink:0, so every
///   row here is a row stolen from the ScrollBox.  The footer must have a
///   STABLE height so it never grows/shrinks and shifts scroll content.
///   See TS PromptInputFooterLeftSide.tsx comment about stable height.
/// =========================================================================

module;

#include <string>
#include <vector>
#include <optional>
#include <format>

#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt.prompt_input_footer;

// ANSI → FTXUI element converter (used by StatusLine for colored command output).
// Lives in message_tool_result.cppm as a shared inline utility.
import cc.ui.messages.message_tool_result;
// P0-1: palette tokens (bash_border / prompt_border color resolution).
import cc.ui.design.tokens;
// P0-1: active theme provider for bash-border consistency (BUG-3 fix).
import cc.ui.design.theme;

export namespace cc::ui::prompt::footer {

using namespace ftxui;

// ============================================================
// Enums (1:1 with TS types)
// ============================================================

/// Prompt input mode.  Mirrors TS PromptInputMode (textInputTypes.ts).
enum class PromptInputMode {
    Prompt,
    Bash,
    SlashCommand,
    HistorySearch,
    PlanMode,
};

/// Vim mode.  Mirrors TS VimMode (textInputTypes.ts).
enum class VimMode {
    Normal,
    Insert,
    Visual,
    None,   // vim disabled
};

/// Permission mode.  Mirrors TS ToolPermissionContext.mode (Tool.ts).
enum class PermissionMode {
    Default,        // default — confirm each tool use
    AcceptEdits,    // 🔓 auto-accept file edits
    AcceptAll,      // 🔓🔓 auto-accept all tools
    Plan,           // 📋 plan-only mode
};

// ============================================================
// Permission mode helpers (from TS PermissionMode.ts)
// ============================================================

/// Whether the mode is the default (no special badge shown).
[[nodiscard]] inline bool IsDefaultMode(PermissionMode m) {
    return m == PermissionMode::Default;
}

/// Symbol emoji for the permission mode.
[[nodiscard]] inline std::string_view PermissionModeSymbol(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:    return "";
        case PermissionMode::AcceptEdits: return "\xF0\x9F\x94\x93";   // 🔓
        case PermissionMode::AcceptAll:   return "\xF0\x94\x93\x93";   // 🔓🔓 (2 chars)
        case PermissionMode::Plan:        return "\xF0\x9F\x93\x8B";   // 📋
    }
    return "";
}

/// Human-readable title for the permission mode.
[[nodiscard]] inline std::string_view PermissionModeTitle(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:     return "default";
        case PermissionMode::AcceptEdits: return "Edit";
        case PermissionMode::AcceptAll:   return "All";
        case PermissionMode::Plan:        return "Plan";
    }
    return "default";
}

/// Color for the permission mode badge text.
[[nodiscard]] inline Color GetModeColor(PermissionMode m) {
    switch (m) {
        case PermissionMode::Default:     return Color::GrayLight;
        case PermissionMode::AcceptEdits: return Color::Yellow;
        case PermissionMode::AcceptAll:   return Color::Red;
        case PermissionMode::Plan:        return Color::Magenta;
    }
    return Color::GrayLight;
}

// ============================================================
// ModeIndicator
// ============================================================
// Mirrors TS ModeIndicator inside PromptInputFooterLeftSide.tsx.
// Shows permission mode badge, background tasks pill, teams, PR badge,
// and keyboard hints.

struct ModeIndicatorOptions {
    PromptInputMode mode = PromptInputMode::Prompt;
    PermissionMode permission_mode = PermissionMode::Default;
    bool show_hint = true;
    bool is_loading = false;
    bool tasks_selected = false;
    bool teams_selected = false;
    bool tmux_selected = false;
    std::optional<int> teammate_footer_index;

    // Task / team counts
    int background_task_count = 0;
    int teammate_count = 0;

    // Hints
    bool show_esc_hint = false;   // "esc to interrupt" when loading
    std::string esc_shortcut = "esc";
    std::string todos_shortcut = "ctrl+t";

    // Remote / session
    bool is_remote_mode = false;
};

/// Render the ModeIndicator — permission mode + tasks pill + teams + hints.
///
/// Faithful to TS structure:
///   modePart (flexShrink:0) · tasksPart (flexShrink:0) · parts (truncate)
///
/// STABLE HEIGHT: always renders exactly 1 row (may show a space when empty
///   in fullscreen mode — see TS stable-height comment).
[[nodiscard]] inline Element RenderModeIndicator(const ModeIndicatorOptions& opts) {
    // Early return for bash mode (TS: "! for bash mode", colored with
    // bashBorder token for consistency with prompt prefix + transcript
    // user-bash-input bubble — fixes BUG-3: 3-sites bash-border divergence).
    if (opts.mode == PromptInputMode::Bash) {
        using namespace cc::ui::design;
        const auto& pal = *theme::current_theme().palette;
        return hbox({ text("! for bash mode") | color(pal.bash_border) })
             | size(HEIGHT, EQUAL, 1);
    }

    Elements left_parts;   // flexShrink=0 items (mode + tasks pill + teams)
    Elements hint_parts;   // truncatable hints

    bool has_active_mode = !IsDefaultMode(opts.permission_mode) && !opts.is_remote_mode;
    bool has_background_tasks = opts.background_task_count > 0;
    bool has_teams = opts.teammate_count > 0;

    // Count primary items for hint visibility logic (TS: primaryItemCount)
    int primary_item_count = (has_active_mode ? 1 : 0)
                           + (has_background_tasks ? 1 : 0)
                           + (has_teams ? 1 : 0);

    // ── Mode part (permission mode badge) ───────────────────────────────
    if (has_active_mode) {
        Elements mode_el = {
            text(std::string{PermissionModeSymbol(opts.permission_mode)})
                | color(GetModeColor(opts.permission_mode)),
            text(" "),
            text(std::string{PermissionModeTitle(opts.permission_mode)})
                | color(GetModeColor(opts.permission_mode)),
        };
        // Show cycle hint only when few primary items (TS: shouldShowModeHint)
        if (primary_item_count < 2 && opts.show_hint) {
            mode_el.push_back(text(" on") | dim);
            mode_el.push_back(text(" (") | dim);
            mode_el.push_back(text("shift+tab") | dim | bold);
            mode_el.push_back(text(" to cycle)") | dim);
        }
        left_parts.push_back(hbox(std::move(mode_el)));
    }

    // ── Tasks pill ──────────────────────────────────────────────────────
    if (has_background_tasks) {
        std::string label = std::format("{} task{}",
            opts.background_task_count,
            opts.background_task_count == 1 ? "" : "s");
        Color c = opts.tasks_selected ? Color::Cyan : Color::Cyan;
        Element pill = hbox({
            text(label) | color(c) | (opts.tasks_selected ? inverted : nothing),
        });
        if (opts.show_hint && !has_teams) {
            // "↓ to manage" hint when tasks present and no teams
            pill = hbox({
                std::move(pill),
                text(" ↓") | dim,
                text(" manage") | dim,
            });
        }
        left_parts.push_back(std::move(pill));
    }

    // ── Teams pill ──────────────────────────────────────────────────────
    if (has_teams) {
        std::string label = std::format("{} team{}",
            opts.teammate_count,
            opts.teammate_count == 1 ? "" : "s");
        Color c = opts.teams_selected ? Color::Magenta : Color::Magenta;
        Element pill = hbox({
            text(label) | color(c) | (opts.teams_selected ? inverted : nothing),
        });
        left_parts.push_back(std::move(pill));
    }

    // ── Hint parts ──────────────────────────────────────────────────────
    if (opts.show_hint) {
        if (opts.is_loading) {
            hint_parts.push_back(
                hbox({ text(opts.esc_shortcut) | dim | bold,
                       text(" to interrupt") | dim }));
        } else if (has_background_tasks || has_teams) {
            hint_parts.push_back(
                hbox({ text(opts.todos_shortcut) | dim | bold,
                       text(" toggle tasks") | dim }));
        } else if (left_parts.empty()) {
            // Default hint when nothing else is shown
            hint_parts.push_back(text("? for shortcuts") | dim);
        }
    }

    // ── Compose ─────────────────────────────────────────────────────────
    // Join left_parts with " · " separators (TS Byline style)
    Elements row_parts;
    for (std::size_t i = 0; i < left_parts.size(); ++i) {
        if (i > 0) row_parts.push_back(text(" · ") | dim);
        row_parts.push_back(std::move(left_parts[i]));
    }

    // Append hint parts (truncated at end)
    if (!hint_parts.empty()) {
        if (!row_parts.empty()) row_parts.push_back(text(" · ") | dim);
        for (auto& hp : hint_parts) {
            row_parts.push_back(std::move(hp));
        }
    }

    // Stable 1-row height.  When empty, render a space so FTXUI reserves
    // the row (mirrors TS `isFullscreen ? <Text> </Text> : null`).
    if (row_parts.empty()) {
        return text(" ") | size(HEIGHT, EQUAL, 1);
    }

    return hbox(std::move(row_parts)) | size(HEIGHT, EQUAL, 1);
}

// ============================================================
// PromptInputFooterLeftSide
// ============================================================

struct LeftSideOptions {
    // Exit message
    bool exit_message_show = false;
    std::string exit_message_key;

    // Pasting
    bool is_pasting = false;

    // Vim
    VimMode vim_mode = VimMode::None;
    bool vim_enabled = false;

    // History search
    bool is_searching = false;
    std::string history_query;
    bool history_failed_match = false;

    // Mode indicator
    ModeIndicatorOptions mode_indicator;
};

/// Render the left side of the prompt input footer.
///
/// Priority order (only ONE of the top items shows at a time):
///   1. Exit message (highest priority, replaces everything)
///   2. Pasting indicator
///   3. History search input (when searching)
///   4. Vim INSERT badge (when insert mode and not searching)
///   5. ModeIndicator (always shown as baseline)
///
/// STABLE HEIGHT: returns exactly 1 row.  The ModeIndicator always
///   reserves a row even when empty (see stable-height note in TS).
[[nodiscard]] inline Element RenderLeftSide(const LeftSideOptions& opts) {
    // 1. Exit message — highest priority, replaces everything
    if (opts.exit_message_show) {
        std::string msg = "Press " + opts.exit_message_key + " again to exit";
        return hbox({ text(msg) | dim }) | size(HEIGHT, EQUAL, 1);
    }

    // 2. Pasting indicator
    if (opts.is_pasting) {
        return hbox({ text("Pasting text…") | dim }) | size(HEIGHT, EQUAL, 1);
    }

    // 3. History search input
    if (opts.is_searching) {
        // TS HistorySearchInput: "(reverse-i-search)`query':"
        // failed match → red color
        std::string label = "(reverse-i-search)`" + opts.history_query + "':";
        Color c = opts.history_failed_match ? Color::Red : Color::Magenta;
        return hbox({ text(label) | color(c) }) | size(HEIGHT, EQUAL, 1);
    }

    // 4. Vim INSERT badge + mode indicator (both inline in TS)
    bool show_vim = opts.vim_enabled && opts.vim_mode == VimMode::Insert;
    if (show_vim) {
        return hbox({
            text("-- INSERT --") | dim,
            filler(),
            RenderModeIndicator(opts.mode_indicator),
        }) | size(HEIGHT, EQUAL, 1);
    }

    // 5. ModeIndicator only (baseline)
    return RenderModeIndicator(opts.mode_indicator);
}

// ============================================================
// StatusLine (simplified — user-configurable command output)
// ============================================================
// TS StatusLine executes a user-defined shell command and renders ANSI output
// in the footer's left column.  For the C++ port we provide a minimal
// placeholder that can be populated by the engine with pre-formatted text.

struct StatusLineOptions {
    std::string content;     // Status line output text (may contain ANSI codes)
    bool should_display = false;  // True when statusLine is configured in settings
    bool is_fullscreen = true;    // In fullscreen, reserve row even while loading
    int padding_x = 0;            // Horizontal padding (mirrors TS paddingX)
};

/// Render the user-configurable StatusLine.
///
/// Faithful to TS StatusLine.tsx:
///   - Text may contain ANSI escape codes (SGR) for colored output
///   - ANSI output keeps its own brightness; TS passes dimColor to the local
///     Text component, but that component only consumes the `dim` prop
///   - Single line, truncated if too long
///   - In fullscreen mode, reserves a row even while loading (stable height)
///   - Horizontal padding from settings.statusLine.padding (paddingX in TS)
[[nodiscard]] inline Element RenderStatusLine(const StatusLineOptions& opts) {
    if (!opts.should_display) {
        // Not configured — nothing to render (zero height)
        return text("");
    }

    // Fullscreen + no content yet → reserve a blank row for stable height.
    // Mirrors TS: isFullscreenEnvEnabled() ? <Text> </Text> : null
    if (opts.content.empty()) {
        if (opts.is_fullscreen) {
            return text(" ") | size(HEIGHT, EQUAL, 1);
        }
        return text("");
    }

    // Parse ANSI codes into colored FTXUI elements.
    //
    // Two faithful-to-TS alignment:
    //   (1) TS StatusLine.tsx wraps the rendered node in dimColor (SGR 2) so
    //       user shell colors are always slightly muted.  We apply `dim` here.
    //   (2) External scripts frequently set their own SGR 48;2 / bgcolor
    //       (e.g. Flux statusline's 📁 folder pill with a deep-blue pill bg)
    //       which bleeds through and clashes with our terminal chrome.  We
    //       force a neutral userMessageBackground RGB(20,20,22) as the row bg
    //       so the content stays subdued and in-theme.
    namespace msgs = cc::ui::messages;
    Element content = msgs::ansi_to_ftxui_elements(opts.content)
                   | dim
                   | bgcolor(Color::RGB(20, 20, 22));

    // Apply horizontal padding (mirrors TS paddingX on the wrapping Box).
    // Padding is applied equally on left and right sides.
    if (opts.padding_x > 0) {
        std::string pad_str(static_cast<std::size_t>(opts.padding_x), ' ');
        content = hbox({
            text(pad_str),
            std::move(content),
            text(pad_str),
        });
    }

    return hbox({ std::move(content) }) | size(HEIGHT, EQUAL, 1);
}

// ============================================================
// BridgeStatusIndicator
// ============================================================

enum class BridgeStatus {
    Disabled,
    Connected,
    Disconnected,
    Reconnecting,
};

struct BridgeOptions {
    BridgeStatus status = BridgeStatus::Disabled;
    bool selected = false;
    bool explicit_remote = false;   // explicit (--remote) vs implicit config
};

/// Render the bridge / remote control status indicator.
/// Mirrors TS BridgeStatusIndicator in PromptInputFooter.tsx.
/// Returns empty element when bridge is disabled or status isn't shown.
[[nodiscard]] inline Element RenderBridgeStatus(const BridgeOptions& opts) {
    if (opts.status == BridgeStatus::Disabled) return text("");

    // For implicit (config-driven) remote, only show reconnecting state
    // (TS: !explicit && status.label !== 'Remote Control reconnecting')
    if (!opts.explicit_remote && opts.status != BridgeStatus::Reconnecting) {
        return text("");
    }

    std::string label;
    Color c = Color::GrayLight;
    switch (opts.status) {
        case BridgeStatus::Connected:
            label = "Remote Control"; c = Color::Green; break;
        case BridgeStatus::Disconnected:
            label = "Remote disconnected"; c = Color::Red; break;
        case BridgeStatus::Reconnecting:
            label = "Remote Control reconnecting"; c = Color::Yellow; break;
        default:
            return text("");
    }

    Element el = text(label) | color(c);
    if (opts.selected) {
        el = el | inverted;
        // TS: "· Enter to view" when selected
        el = hbox({ std::move(el), text(" · Enter to view") | dim });
    }
    return el;
}

// ============================================================
// Full PromptInputFooter (left + right columns)
// ============================================================

struct FooterOptions {
    // Left column
    StatusLineOptions status_line;
    LeftSideOptions left_side;

    // Right column
    BridgeOptions bridge;
    bool show_notifications = false;   // non-fullscreen only
    bool is_undercover = false;        // ant-only

    // Layout
    bool is_fullscreen = true;
    bool is_narrow = false;            // columns < 80
    bool help_open = false;

    // Suggestions (when present, footer shows only suggestions)
    bool has_suggestions = false;
    Element suggestions_content;       // pre-rendered suggestions list
};

/// Render the full PromptInputFooter with left/right column layout.
///
/// Faithful to TS PromptInputFooter.tsx structure:
///   Row (space-between):
///     Left column (vbox): StatusLine? + LeftSide
///     Right column (vbox): Notifications? + undercover? + BridgeStatus
///
/// STABLE HEIGHT: The footer reserves space for the StatusLine + LeftSide
///   on the left.  In fullscreen mode this is a fixed height so scroll
///   content never shifts.
[[nodiscard]] inline Element RenderPromptInputFooter(const FooterOptions& opts) {
    // ── Special: suggestions overlay replaces entire footer ──────────
    if (opts.has_suggestions && opts.suggestions_content) {
        return hbox({ text("  "), std::move(opts.suggestions_content) });
    }

    // ── Special: help menu replaces entire footer ────────────────────
    if (opts.help_open) {
        // Help menu content would be injected here; for now show placeholder
        return hbox({ text("  Help menu") | dim });
    }

    // ── Left column ────────────────────────────────────────────────────
    Elements left_col;
    left_col.reserve(2);

    // StatusLine (shown only when the caller's TS-equivalent visibility gate
    // allows it).
    // In fullscreen mode, reserves a blank row even while loading so the
    // footer height never shifts (stable height — same trick as LeftSide).
    // Exit message / pasting / history search affect only LeftSide (below),
    // not StatusLine — they are separate rows in the left column.
    bool show_status_line = opts.status_line.should_display
        && (opts.is_fullscreen || !opts.status_line.content.empty());
    if (show_status_line) {
        left_col.push_back(RenderStatusLine(opts.status_line));
    }

    // LeftSide (always present — ModeIndicator reserves 1 row)
    left_col.push_back(RenderLeftSide(opts.left_side));

    Element left_el = vbox(std::move(left_col)) | flex;

    // ── Right column ───────────────────────────────────────────────────
    // TS: <Box flexShrink={1} gap={1}> — items in a row, right-aligned
    Elements right_row;

    // Notifications (non-fullscreen only, per TS)
    if (opts.show_notifications && !opts.is_fullscreen) {
        // Notifications rendered by caller and passed in?  For now stub.
        // TS: <Notifications apiKeyStatus autoUpdaterResult ... />
    }

    // Undercover (ant-only)
    if (opts.is_undercover) {
        if (!right_row.empty()) right_row.push_back(text(" ") | dim);
        right_row.push_back(text("undercover") | dim);
    }

    // Bridge status indicator
    if (opts.bridge.status != BridgeStatus::Disabled
        && (opts.bridge.explicit_remote
            || opts.bridge.status == BridgeStatus::Reconnecting))
    {
        if (!right_row.empty()) right_row.push_back(text(" ") | dim);
        right_row.push_back(RenderBridgeStatus(opts.bridge));
    }

    const bool has_right = !right_row.empty();
    Element right_el = has_right ? hbox(std::move(right_row)) : text("");

    // TS: outer Box switches row -> column at narrow widths.  The StatusLine
    // remains inside the left column, so right-column content top-aligns with
    // the StatusLine row when it is present.
    if (opts.is_narrow) {
        Elements rows;
        rows.push_back(hbox({
            text("  "),
            std::move(left_el),
            text("  "),
        }));
        if (has_right) {
            rows.push_back(hbox({
                text("  "),
                std::move(right_el),
                text("  "),
            }));
        }
        return vbox(std::move(rows));
    }

    return hbox({
        text("  "),   // paddingX={2}
        std::move(left_el),
        filler(),
        std::move(right_el),
        text("  "),   // paddingX={2}
    });
}

} // namespace cc::ui::prompt::footer
