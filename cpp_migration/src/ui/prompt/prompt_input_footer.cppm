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
#include <filesystem>
#include <algorithm>
#include <format>
#include <cmath>
#include <cstdio>
#include <chrono>

#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt.prompt_input_footer;

// ANSI → FTXUI element converter (used by StatusLine for colored command output).
// Lives in message_tool_result.cppm as a shared inline utility.
import cc.ui.messages.message_tool_result;
// P0-1: palette tokens (bash_border / prompt_border color resolution).
import cc.ui.design.tokens;
// P0-1: active theme provider for bash-border consistency (BUG-3 fix).
import cc.ui.design.theme;
// Unified canonical PromptInputMode enum (replaces local 5-value definition).
import cc.ui.common.types;

export namespace cc::ui::prompt::footer {

using namespace ftxui;

// ============================================================
// Enums (1:1 with TS types)
// ============================================================

/// Prompt input mode — unified canonical enum from cc::ui::common.
/// Previously this file defined a local 5-value PromptInputMode:
///   {Prompt, Bash, SlashCommand, HistorySearch, PlanMode}
/// Old→new mapping:
///   PromptInputMode::Prompt → PromptInputMode::Normal  (TS: 'prompt')
/// All other values (Bash, SlashCommand, HistorySearch, PlanMode)
/// are identical in the unified definition.
/// TS REF: src/types/textInputTypes.ts:265 (PromptInputMode type)
using cc::ui::common::PromptInputMode;

/// Vim mode — canonical enum from cc::ui::common (ui_types.cppm).
/// Previously this file defined a local 4-value enum { Normal, Insert, Visual, None }.
/// "None" (vim disabled) is now expressed as std::optional<VimMode>{nullopt}.
/// TS REF: src/types/textInputTypes.ts:222 (public VimMode = 'INSERT'|'NORMAL')
///          src/hooks/useVimInput.ts (internal state machine with Visual/Replace/etc.)
using cc::ui::common::VimMode;

/// Permission mode.  Mirrors TS ToolPermissionContext.mode (Tool.ts).
/// Canonical definition lives in cc::ui::common (ui_types.cppm).
using cc::ui::common::PermissionMode;

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

/// Cycle to the next permission mode (TS REF: getNextPermissionMode.ts).
/// Order for non-ant users: Default → AcceptEdits → Plan → Default.
/// AcceptAll is reachable only from Plan when bypass is available (not yet
/// wired in CPP — included for forward compatibility).
[[nodiscard]] inline PermissionMode GetNextPermissionMode(PermissionMode current) {
    switch (current) {
        case PermissionMode::Default:     return PermissionMode::AcceptEdits;
        case PermissionMode::AcceptEdits: return PermissionMode::Plan;
        case PermissionMode::Plan:        return PermissionMode::Default;
        case PermissionMode::AcceptAll:   return PermissionMode::Default;
    }
    return PermissionMode::Default;
}

// ============================================================
// ModeIndicator
// ============================================================
// Mirrors TS ModeIndicator inside PromptInputFooterLeftSide.tsx.
// Shows permission mode badge, background tasks pill, teams, PR badge,
// and keyboard hints.

struct ModeIndicatorOptions {
    PromptInputMode mode = PromptInputMode::Normal;
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

    // Transcript / brief view modes (TS REF: Messages.tsx isTranscriptMode + isBriefOnly).
    // Shown as pills in the mode indicator row so the user knows which filter
    // is currently active on the message list.
    bool is_transcript_mode = false;
    bool is_brief_mode = false;
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

    // ── Transcript mode pill (TS REF: Messages.tsx isTranscriptMode L459)
    //    Shown when user pressed Ctrl+O to enter detailed transcript view.
    if (opts.is_transcript_mode) {
        using namespace cc::ui::design;
        const auto& pal = *theme::current_theme().palette;
        Element pill = hbox({
            text("TRANSCRIPT") | color(pal.info) | bold,
        });
        left_parts.push_back(std::move(pill));
    }

    // ── Brief mode pill (TS REF: Messages.tsx isBriefOnly L236)
    //    Shown when user enabled brief-only filter (e.g. via /brief).
    if (opts.is_brief_mode) {
        using namespace cc::ui::design;
        const auto& pal = *theme::current_theme().palette;
        Element pill = hbox({
            text("BRIEF") | color(pal.brief_label) | bold,
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

    // Vim — nullopt = vim disabled (replaces vim_mode=None + vim_enabled=false)
    std::optional<VimMode> vim_mode;

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
    //    TS REF: PromptInputFooterLeftSide.tsx — shows "-- INSERT --" only in
    //    insert mode (normal/visual show nothing extra in the footer).
    bool show_vim = opts.vim_mode == VimMode::Insert;
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
//
// P0-6: When the user's command returns empty (or no statusLine is
// configured), a built-in statusline shows folder/git/model/token info.
// See BuiltinStatusLineData below.

namespace fs = std::filesystem;

// P0-6 builtin statusline data.  Defined early so StatusLineOptions can
// hold a std::optional<BuiltinStatusLineData>.
struct BuiltinStatusLineData {
    std::string cwd;                ///< Current working directory (full path)
    std::string git_branch;         ///< Git branch name (empty if not a repo)
    std::string model_name;         ///< Model display name
    int input_tokens = 0;           ///< Input tokens used this session
    int output_tokens = 0;          ///< Output tokens used this session
    int context_token_count = 0;    ///< Total context tokens (in+out)
    std::optional<double> cost_usd; ///< Session cost in USD
    int context_window_size = 200000; ///< Context window size (default 200k)
};

/// Extract the last N path components for display.
/// e.g. "/a/b/c/d" with n=2 → "c/d"
[[nodiscard]] inline std::string GetLastPathComponents(
    const std::string& cwd, int n = 2);

/// Format token count as "K" string (e.g. 28000 → "28.0K")
[[nodiscard]] inline std::string FormatTokensK(int tokens);

/// Render the built-in statusline (defined after RenderStatusLine).
[[nodiscard]] inline Element RenderBuiltinStatusLine(
    const BuiltinStatusLineData& data);

struct StatusLineOptions {
    std::string content;     // Status line output text (may contain ANSI codes)
    bool should_display = false;  // True when statusLine is configured in settings
    bool is_fullscreen = true;    // In fullscreen, reserve row even while loading
    int padding_x = 0;            // Horizontal padding (mirrors TS paddingX)
    // P0-6 builtin statusline: fallback data when content is empty (e.g. user's
    // command returns nothing, or no statusLine configured).  When set and
    // content is empty, RenderBuiltinStatusLine() is used instead of blank
    // placeholder.  Provides folder/git/model/token info for standalone mode.
    std::optional<BuiltinStatusLineData> builtin;
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

    // Fullscreen + no content yet → try builtin statusline, then blank row.
    // Mirrors TS: isFullscreenEnvEnabled() ? <Text> </Text> : null
    // P0-6: When user's command returns empty (or no command configured),
    // show the built-in statusline with folder/git/model/token info.
    if (opts.content.empty()) {
        if (opts.builtin) {
            return RenderBuiltinStatusLine(*opts.builtin)
                 | size(HEIGHT, EQUAL, 1);
        }
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
// BuiltinStatusLine (P0-6: default statusline for standalone mode)
// ============================================================
// When the user's statusLine.command returns empty output (e.g. Flux Island
// wrapper without Flux running), this built-in statusline provides useful
// info: folder, git branch, model, context usage, and cost.
//
// TS REFERENCE: There is no TS equivalent — this is a CPP-only enhancement
// for standalone usability.  The visual style is inspired by the user's
// Flux Island statusline:
//   📁 CC-REPL/cpp_migration  🌿 master  🤖 GLM-5.2 ▮  14% 28.0K/200.0K  $0.12
//
// The user's configured command output takes priority when available.
//
// The BuiltinStatusLineData struct and helper prototypes are declared above
// (before StatusLineOptions) so the options struct can hold the fallback data.

/// Extract the last N path components for display.
/// e.g. "/a/b/c/d" with n=2 → "c/d"
[[nodiscard]] inline std::string GetLastPathComponents(
    const std::string& cwd, int n)
{
    if (cwd.empty()) return "";
    fs::path p(cwd);
    std::vector<std::string> parts;
    for (const auto& comp : p) {
        if (!comp.empty() && comp != "/") {
            parts.push_back(comp.string());
        }
    }
    if (parts.empty()) return cwd;
    int start = std::max(0, static_cast<int>(parts.size()) - n);
    std::string result;
    for (int i = start; i < static_cast<int>(parts.size()); ++i) {
        if (!result.empty()) result += "/";
        result += parts[i];
    }
    return result;
}

/// Format token count as "K" string (e.g. 28000 → "28.0K", 1500 → "1.5K")
[[nodiscard]] inline std::string FormatTokensK(int tokens) {
    if (tokens <= 0) return "0K";
    if (tokens < 1000) return std::to_string(tokens);
    double k = tokens / 1000.0;
    // Show 1 decimal for thousands
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fK", k);
    return std::string(buf);
}

/// Render the built-in statusline.
/// Produces a single-row element with colored pills.
[[nodiscard]] inline Element RenderBuiltinStatusLine(
    const BuiltinStatusLineData& data)
{
    using ftxui::text;
    using ftxui::Element;
    using ftxui::Elements;
    using ftxui::hbox;
    using ftxui::color;
    using ftxui::bgcolor;
    using ftxui::bold;
    using ftxui::dim;

    // Resolve colors from active theme palette.
    // Uses real Palette fields (see design_system/design_tokens.cppm).
    namespace theme_ns = cc::ui::design::theme;
    auto theme = theme_ns::current_theme();
    const auto& pal = *theme.palette;

    const Color kClaudeGold = pal.primary;          // clawd orange/amber
    const Color kFolderColor = pal.muted;            // dim text
    const Color kBranchColor = pal.success;          // green
    const Color kModelColor = pal.info;              // blue/cyan
    const Color kCostColor = pal.success;            // green
    const Color kBgColor = Color::RGB(20, 20, 22);

    Elements parts;

    // ── 📁 Folder pill ──────────────────────────────────────
    std::string folder_display = GetLastPathComponents(data.cwd, 2);
    if (folder_display.empty()) folder_display = "~";
    parts.push_back(text("📁 ") | dim | color(kFolderColor));
    parts.push_back(text(folder_display) | dim | color(kFolderColor));

    // ── 🌿 Git branch pill ──────────────────────────────────
    if (!data.git_branch.empty()) {
        parts.push_back(text("  🌿 ") | dim | color(kBranchColor));
        parts.push_back(text(data.git_branch) | dim | color(kBranchColor));
    }

    // ── 🤖 Model pill ───────────────────────────────────────
    if (!data.model_name.empty()) {
        parts.push_back(text("  🤖 ") | dim | color(kModelColor));
        parts.push_back(text(data.model_name) | bold | color(kModelColor));
    }

    // ── ▮ Context usage bar ─────────────────────────────────
    // Calculate usage percentage
    double pct = 0.0;
    if (data.context_window_size > 0) {
        pct = 100.0 * data.context_token_count /
              static_cast<double>(data.context_window_size);
    }
    int pct_int = static_cast<int>(std::round(pct));

    // Build a mini progress bar: ▮▮▮▮▯▯▯▯ (10 segments)
    constexpr int kBarSegments = 10;
    int filled = static_cast<int>(std::round(
        pct / 100.0 * kBarSegments));
    filled = std::clamp(filled, 0, kBarSegments);
    std::string bar;
    for (int i = 0; i < filled; ++i) bar += "▮";
    for (int i = filled; i < kBarSegments; ++i) bar += "▯";

    // Color: green < 50%, amber 50-80%, red > 80%
    Color bar_color = Color::RGB(120, 200, 120);  // green
    if (pct >= 80.0) bar_color = Color::RGB(220, 80, 80);   // red
    else if (pct >= 50.0) bar_color = kClaudeGold;           // amber

    parts.push_back(text("  ") | dim);
    parts.push_back(text(bar) | color(bar_color));

    // Percentage + token counts
    std::string ctx_str = FormatTokensK(data.context_token_count);
    std::string win_str = FormatTokensK(data.context_window_size);
    parts.push_back(text(std::format(" {}% {}/{}",
                      pct_int, ctx_str, win_str))
                    | dim);

    // ── $ Cost ──────────────────────────────────────────────
    if (data.cost_usd && *data.cost_usd > 0.0) {
        char cost_buf[32];
        std::snprintf(cost_buf, sizeof(cost_buf), "$%.4f", *data.cost_usd);
        parts.push_back(text("  ") | dim);
        parts.push_back(text(cost_buf) | dim | color(kCostColor));
    }

    return hbox({ text(" "), hbox(std::move(parts)), text(" ") })
         | bgcolor(kBgColor)
         | size(HEIGHT, EQUAL, 1);
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
// Notifications (right column, non-fullscreen only)
// ============================================================
// TS REFERENCE: src/components/PromptInput/Notifications.tsx (331 lines)
//
// The Notifications component renders ONE notification at a time, chosen by
// priority.  In TS the full priority chain is:
//   voice indicator (highest, VOICE_MODE only)
//   > IdeStatusIndicator
//   > notifications.current (dynamic: env-hook, external-editor-hint, etc.)
//   > overage mode
//   > apiKeyHelper slow
//   > apiKeyStatus invalid/missing
//   > debug mode
//   > verbose token count
//   > TokenWarning (context limit approaching)
//   > AutoUpdater
//   > voice error
//   > MemoryUsageIndicator
//   > SandboxPromptFooterHint (lowest)
//
// For the CPP faithful-port we implement the top user-visible items that
// have data available.  Items requiring engine wiring (autoUpdater, voice,
// memory, sandbox) are stubs that render nothing until data is provided.

/// API key verification status.  Mirrors TS VerificationStatus (useApiKeyVerification.ts).
enum class ApiKeyStatus {
    Valid,       // verified and working
    Invalid,     // rejected by API
    Missing,     // no key configured
    Unknown,     // not yet checked
};

/// IDE selection info.  Mirrors TS IDESelection (useIdeSelection.ts).
struct IdeSelectionInfo {
    bool connected = false;
    std::optional<std::string> file_path;     // basename shown in indicator
    std::optional<int> selected_lines;        // when text is selected in IDE
};

// ============================================================
// Typed notification pill variants (P1: footer-notifications-stub)
// ============================================================
// TS REFERENCE:
//   src/components/AutoUpdater.tsx       – auto-updater status pills
//   src/hooks/useApiKeyVerification.ts   – apiKeyStatus pill colors
//   src/components/PromptInput/Notifications.tsx L306-322 – apiKey + verbose
//
// These typed pill variants provide structured data for the most common
// footer notifications.  They are rendered as styled pills (icon + text
// with colored border/background) via RenderNotificationPill().
//
// To add a typed notification to the queue, use the convenience helpers:
//   AddApiKeyNotification(), AddAutoUpdaterNotification(),
//   AddProRenewalNotification(), AddNewReleaseNotification().

/// Auto-updater install status.
/// TS REF: src/utils/autoUpdater.ts (InstallStatus type)
/// TS REF: src/components/AutoUpdater.tsx L176-196 (render logic)
enum class AutoUpdaterStatus {
    Available,      ///< New version found, not yet installed
    Downloading,    ///< Currently downloading/installing
    Installed,      ///< Successfully installed, restart needed
    Error,          ///< Install failed (install_failed or no_permissions)
};

/// Auto-updater notification data.
/// TS REF: src/components/AutoUpdater.tsx (Props.autoUpdaterResult)
struct AutoUpdaterData {
    AutoUpdaterStatus status = AutoUpdaterStatus::Available;
    std::string version;          ///< Target version string (e.g. "2.1.57")
    std::string error_detail;     ///< Optional error message for Error status
};

/// Pro/Team subscription renewal reminder.
/// TS REF: No direct TS equivalent — CPP enhancement for subscription UX.
/// Shows a renewal reminder pill when days_remaining < threshold (default 7).
struct ProRenewalData {
    int days_remaining = 0;       ///< Days until subscription expires
    std::string plan_name;        ///< Plan display name (e.g. "Pro")
};

/// New release announcement pill.
/// TS REF: useUpdateNotification() in src/hooks/useUpdateNotification.ts
/// Shows "New: vX.Y.Z" gift pill when a new version is announced.
struct NewReleaseData {
    std::string version;          ///< New version string (e.g. "2.2.0")
};

/// Tagged union of typed pill variants for NotificationItem.
/// When set, RenderNotificationPill() uses this to produce a styled pill
/// element instead of the plain text+color path.
enum class PillVariant {
    None,             ///< Plain text notification (default)
    ApiKey,           ///< API key status pill (key icon + status)
    AutoUpdater,      ///< Auto-updater status pill (download icon)
    ProRenewal,       ///< Pro renewal reminder pill (clock icon)
    NewRelease,       ///< New release announcement pill (gift icon)
};

/// Typed pill payload stored in NotificationItem.
struct PillPayload {
    PillVariant variant = PillVariant::None;
    // Only one of these is valid depending on variant:
    ApiKeyStatus       api_key_status = ApiKeyStatus::Unknown;
    AutoUpdaterData    auto_updater;
    ProRenewalData     pro_renewal;
    NewReleaseData     new_release;
};

// ============================================================
// Notification Queue — priority-based rotating carousel
// ============================================================
// TS REFERENCE: src/context/notifications.tsx (the useNotifications hook)
// TS REFERENCE: src/components/PromptInput/Notifications.tsx L288-292
//
// Implements a priority queue of up to 12 notification items that rotate
// through the "current" slot on a timeout basis.  Faithful to TS:
//   - Priority ordering: immediate > high > medium > low
//   - Each item has a timeout (default 8000ms, same as TS DEFAULT_TIMEOUT_MS)
//   - When current expires, highest-priority item from the queue becomes current
//   - Queue capped at 12 items; oldest lowest-priority evicted when full
//   - Items can invalidate others (via invalidates key list)
//   - Duplicate keys are prevented (dedup, same as TS queuedKeys Set)
//   - "immediate" priority items replace current right away (TS L80-116)
//
// The queue is advanced by QueueAdvance() which should be called from the
// engine's event-driven update phase (not a constant ticker — ground rule 5).
// RenderNotifications() reads queue.current if set and shows it as the
// highest-priority display item.

/// Notification priority levels.
/// TS REF: src/context/notifications.tsx L5 (Priority type) + L230-235 (PRIORITIES record)
enum class NotificationPriority {
    Immediate = 0,  ///< Shown immediately, replaces current
    High      = 1,  ///< Next-highest after immediate
    Medium    = 2,  ///< Default for most notifications
    Low       = 3,  ///< Lowest priority
};

/// A single queued notification item.
/// TS REF: src/context/notifications.tsx L6-33 (BaseNotification + TextNotification)
struct NotificationItem {
    std::string key;                              ///< Unique key for dedup/invalidation
    std::string text;                             ///< Display text (plain string, no JSX)
    std::string color;                            ///< "error", "warning", "success", "info", or empty=dim
    NotificationPriority priority = NotificationPriority::Low;  ///< Display priority
    int timeout_ms = 8000;                        ///< TS DEFAULT_TIMEOUT_MS = 8000
    std::vector<std::string> invalidates;         ///< Keys this notification invalidates
    PillPayload pill;                             ///< P1: typed pill variant (for styled rendering)
};

/// The notification queue state.
/// Holds the currently-displayed item + up to 12 pending items.
struct NotificationQueue {
    /// Currently displayed item (nullopt if nothing shown).
    std::optional<NotificationItem> current;

    /// Pending items waiting to be shown (capped at kMaxItems).
    std::vector<NotificationItem> queue;

    /// Maximum items in the queue (carousel size = "up to 12").
    static constexpr std::size_t kMaxItems = 12;

    /// Wall-clock time when current was activated (for timeout check).
    /// Stored as seconds since steady_clock epoch for simple comparison.
    double current_activated_at_sec = 0.0;
};

// ============================================================
// Notification data fed into the footer
// ============================================================

/// Notification data fed into the footer.
///
/// Each field corresponds to one notification type from TS Notifications.tsx.
/// The RenderNotifications() function picks the highest-priority active item.
struct NotificationData {
    // Auth
    ApiKeyStatus api_key_status = ApiKeyStatus::Unknown;
    bool is_remote = false;   // CLAUDE_CODE_REMOTE — changes error text

    // Mode indicators
    bool debug_mode = false;
    bool verbose = false;
    int token_usage = 0;      // used when verbose + apiKey valid

    // Overage
    bool is_overage_mode = false;

    // IDE
    IdeSelectionInfo ide;

    // P1: typed notification pills (footer-notifications-stub)
    // When populated, these render as styled pills in the right column.
    std::optional<AutoUpdaterData> auto_updater;
    std::optional<ProRenewalData> pro_renewal;
    std::optional<NewReleaseData> new_release;

    // Dynamic notification (e.g. env-hook feedback, external-editor hint)
    // When set, takes priority over most static notifications.
    // DEPRECATED: prefer using `queue` instead (NotificationQueue carousel).
    std::optional<std::string> dynamic_text;
    std::optional<std::string> dynamic_color;  // "error", "warning", or empty=dim

    // Notification queue — priority-based rotating carousel of up to 12 items.
    // When queue.current is set, it takes highest priority in RenderNotifications.
    NotificationQueue queue;
};

/// IDE status indicator color — matches TS theme.ide rgb(71,130,200).
/// TS REF: src/utils/theme.ts L125
const Color kIdeColor = Color::RGB(71, 130, 200);

/// Render the IdeStatusIndicator.
/// TS REF: src/components/IdeStatusIndicator.tsx
/// Shows "⧉ In <basename>" or "⧉ N lines selected" when IDE is connected
/// and has a selection.  Returns empty element when nothing to show.
[[nodiscard]] inline Element RenderIdeStatusIndicator(const IdeSelectionInfo& ide) {
    using ftxui::text;
    if (!ide.connected) return text("");

    // TS: shouldShowIdeSelection = ideStatus === "connected" &&
    //   (ideSelection?.filePath || (ideSelection?.text && ideSelection.lineCount > 0))
    const bool has_file = ide.file_path.has_value() && !ide.file_path->empty();
    const bool has_text_sel = ide.selected_lines.has_value() && *ide.selected_lines > 0;

    if (!has_file && !has_text_sel) return text("");

    if (has_text_sel) {
        const int n = *ide.selected_lines;
        const std::string unit = (n == 1) ? "line" : "lines";
        // TS: "⧉ {lineCount} {unit} selected" — color="ide"
        return hbox({
            text("\xE2\xA7\x89 ") | color(kIdeColor),   // ⧉
            text(std::to_string(n) + " " + unit + " selected") | color(kIdeColor),
        });
    }

    if (has_file) {
        // TS: basename(ideSelection.filePath)
        const std::string& path = *ide.file_path;
        auto pos = path.find_last_of("/\\");
        std::string basename = (pos != std::string::npos)
            ? path.substr(pos + 1) : path;
        // TS: "⧉ In {basename}" — color="ide"
        return hbox({
            text("\xE2\xA7\x89 In ") | color(kIdeColor),   // ⧉ In
            text(basename) | color(kIdeColor),
        });
    }

    return text("");
}

// ============================================================
// Notification Pill rendering (P1: footer-notifications-stub)
// ============================================================
// TS REFERENCE:
//   src/components/AutoUpdater.tsx L176-196 – pill text + color for each status
//   src/components/PromptInput/Notifications.tsx L306-322 – apiKey + verbose
//   src/hooks/useUpdateNotification.ts – new release announcement
//
// Each pill: small icon + text, with a colored border or background tint.
// Faithful to TS: uses the same semantic color tokens (success/warning/error)
// and emoji icons that render well in modern terminals.

namespace detail {

/// Resolve a semantic color name ("error", "warning", "success", "info")
/// to an FTXUI Color, using the active theme palette where possible.
/// TS REF: src/components/design-system/ThemedText.tsx (resolveColor)
[[nodiscard]] inline Color ResolveSemanticColor(std::string_view color_name) {
    using namespace cc::ui::design;
    const auto& pal = *theme::current_theme().palette;
    if (color_name == "error")   return pal.danger;
    if (color_name == "warning") return pal.warning;
    if (color_name == "success") return pal.success;
    if (color_name == "info")    return pal.info;
    return pal.muted;   // default dim
}

} // namespace detail

/// Render a styled notification pill from a PillPayload.
///
/// Each pill variant produces a compact 1-row element:
///   ApiKey:    [🔑 ✓] green  /  [🔑 ✗] red  /  [🔑 !] yellow
///   AutoUpdater: [⬇ Update available] blue / [⟳ Updating…] dim / [✓ Installed] green / [✗ Failed] red
///   ProRenewal: [⏱ N days left] orange (only when < 7 days)
///   NewRelease: [🎁 New: vX.Y.Z] purple
///
/// Returns empty text("") for PillVariant::None or empty data.
/// TS REF: src/components/AutoUpdater.tsx L176-196 (auto-updater pills)
/// TS REF: src/components/PromptInput/Notifications.tsx L306-310 (apiKey error)
[[nodiscard]] inline Element RenderNotificationPill(const NotificationItem& item) {
    using ftxui::text;
    using ftxui::color;
    using ftxui::bgcolor;
    using ftxui::bold;
    using ftxui::dim;
    using ftxui::hbox;

    using namespace cc::ui::design;
    const auto& pal = *theme::current_theme().palette;

    const auto& pill = item.pill;

    switch (pill.variant) {
        case PillVariant::None:
            // Fallback: plain text+color (backward compatible)
            if (item.text.empty()) return text("");
            {
                Color c = detail::ResolveSemanticColor(item.color);
                return hbox({ text(item.text) | color(c) })
                     | size(HEIGHT, EQUAL, 1);
            }

        case PillVariant::ApiKey: {
            // TS REF: Notifications.tsx L306-310 — "Not logged in · Run /login" (error)
            //          useApiKeyVerification.ts — VerificationStatus type
            // Icon: 🔑 U+1F511
            const char* kKeyIcon = "\xF0\x9F\x94\x91";   // 🔑
            switch (pill.api_key_status) {
                case ApiKeyStatus::Valid: {
                    // ✓ green — key is working
                    const std::string label = std::string(kKeyIcon) + " \xE2\x9C\x93 API key OK";
                    return hbox({ text(label) | color(pal.success) | dim })
                         | size(HEIGHT, EQUAL, 1);
                }
                case ApiKeyStatus::Invalid: {
                    // ✗ red — key rejected
                    const std::string label = std::string(kKeyIcon) + " \xE2\x9C\x97 Invalid key";
                    return hbox({ text(label) | color(pal.danger) })
                         | size(HEIGHT, EQUAL, 1);
                }
                case ApiKeyStatus::Missing: {
                    // ! yellow — no key configured
                    const std::string label = std::string(kKeyIcon) + " ! Not logged in";
                    return hbox({ text(label) | color(pal.warning) })
                         | size(HEIGHT, EQUAL, 1);
                }
                case ApiKeyStatus::Unknown:
                    return text("");
            }
            return text("");
        }

        case PillVariant::AutoUpdater: {
            // TS REF: src/components/AutoUpdater.tsx L176-196
            //   Downloading: "Auto-updating…" (dim text)
            //   Installed:   "✓ Update installed · Restart to apply" (success)
            //   Error:       "✗ Auto-update failed" (error)
            //   Available:   "New version vX.Y.Z available" (info, not shown in TS
            //                but useful for CPP standalone mode)
            const auto& au = pill.auto_updater;
            switch (au.status) {
                case AutoUpdaterStatus::Downloading: {
                    // ⟳ U+27F3 — "updating"
                    const std::string label =
                        std::string("\xE2\x9F\xB3 ") + "Auto-updating…";
                    return hbox({ text(label) | color(pal.muted) | dim })
                         | size(HEIGHT, EQUAL, 1);
                }
                case AutoUpdaterStatus::Installed: {
                    // ✓ U+2713 — "installed"
                    const std::string label =
                        std::string("\xE2\x9C\x93 Update installed \xC2\xB7 Restart to apply");
                    return hbox({ text(label) | color(pal.success) })
                         | size(HEIGHT, EQUAL, 1);
                }
                case AutoUpdaterStatus::Error: {
                    // ✗ U+2717 — "failed"
                    std::string label = "\xE2\x9C\x97 Auto-update failed";
                    if (!au.error_detail.empty()) {
                        label += " \xC2\xB7 " + au.error_detail;
                    }
                    return hbox({ text(label) | color(pal.danger) })
                         | size(HEIGHT, EQUAL, 1);
                }
                case AutoUpdaterStatus::Available: {
                    // ⬇ U+2B07 — "available" (blue download arrow)
                    std::string label = "\xE2\xAC\x87 New: v" + au.version;
                    return hbox({ text(label) | color(pal.info) })
                         | size(HEIGHT, EQUAL, 1);
                }
            }
            return text("");
        }

        case PillVariant::ProRenewal: {
            // TS REF: No direct TS equivalent — CPP enhancement.
            // Shows renewal reminder only when days_remaining < 7.
            // ⏱ U+23F1 — "timer clock"
            const auto& pr = pill.pro_renewal;
            if (pr.days_remaining >= 7) return text("");
            const std::string plan = pr.plan_name.empty() ? "Pro" : pr.plan_name;
            std::string label = "\xE2\x8F\xB1 " + plan + " renews in "
                              + std::to_string(pr.days_remaining)
                              + (pr.days_remaining == 1 ? " day" : " days");
            // Color: orange/amber for urgency (use warning for < 3 days, info otherwise)
            Color c = (pr.days_remaining <= 3) ? pal.warning : pal.info;
            return hbox({ text(label) | color(c) })
                 | size(HEIGHT, EQUAL, 1);
        }

        case PillVariant::NewRelease: {
            // TS REF: src/hooks/useUpdateNotification.ts — updateSemver
            // 🎁 U+1F381 — "gift" for new release announcement
            const auto& nr = pill.new_release;
            if (nr.version.empty()) return text("");
            const std::string label =
                std::string("\xF0\x9F\x8E\x81 New: v") + nr.version;
            // Purple/magenta for gift pill (closest ANSI to purple)
            return hbox({ text(label) | color(Color::Magenta) })
                 | size(HEIGHT, EQUAL, 1);
        }
    }
    return text("");
}

/// Render the highest-priority active notification.
/// TS REF: src/components/PromptInput/Notifications.tsx NotificationContent()
///
/// Returns an element (possibly empty text("") if nothing active).
/// The element is always exactly 1 row high for stable footer height.
[[nodiscard]] inline Element RenderNotifications(const NotificationData& data) {
    using ftxui::text;
    using ftxui::dim;
    using ftxui::color;
    using ftxui::hbox;

    // Priority chain (highest first):

    // 0. Notification queue — current item (rotating carousel)
    //    TS REF: Notifications.tsx L288-292 (notifications.current render)
    //    The queue's current item has the highest display priority because
    //    it represents time-sensitive dynamic feedback (env-hook, etc.).
    //    P1: When item.pill.variant != None, render as a styled pill via
    //    RenderNotificationPill() — icon + text with themed colors.
    if (data.queue.current) {
        const auto& item = *data.queue.current;
        const bool has_pill = item.pill.variant != PillVariant::None;
        const bool has_text = !item.text.empty();
        if (has_pill || has_text) {
            if (has_pill) {
                // Styled pill rendering (P1: footer-notifications-stub)
                Element pill_el = RenderNotificationPill(item);
                if (pill_el) {
                    return hbox({ std::move(pill_el) })
                         | size(HEIGHT, EQUAL, 1);
                }
            }
            // Fallback: plain text+color
            if (has_text) {
                Color c = Color::GrayLight;   // default dim
                if (item.color == "error")        c = Color::Red;
                else if (item.color == "warning") c = Color::Yellow;
                else if (item.color == "success") c = Color::Green;
                else if (item.color == "info")    c = kIdeColor;
                return hbox({ text(item.text) | color(c) })
                     | size(HEIGHT, EQUAL, 1);
            }
        }
    }

    // 1. Dynamic notification (env-hook, external-editor hint, etc.)
    //    TS: notifications.current with text/color
    //    Kept for backward compatibility; prefer using the queue API.
    if (data.dynamic_text && !data.dynamic_text->empty()) {
        Color c = Color::GrayLight;   // default dim
        if (data.dynamic_color == "error")   c = Color::Red;
        else if (data.dynamic_color == "warning") c = Color::Yellow;
        return hbox({ text(*data.dynamic_text) | color(c) })
             | size(HEIGHT, EQUAL, 1);
    }

    // 2. IDE status indicator
    Element ide_el = RenderIdeStatusIndicator(data.ide);
    if (ide_el) {
        return hbox({ std::move(ide_el) }) | size(HEIGHT, EQUAL, 1);
    }

    // 3. Overage mode — "Now using extra usage" (dim)
    //    TS REF: Notifications.tsx L293-297
    if (data.is_overage_mode) {
        return hbox({ text("Now using extra usage") | dim })
             | size(HEIGHT, EQUAL, 1);
    }

    // 4. API key invalid/missing — "Not logged in · Run /login" (error)
    //    TS REF: Notifications.tsx L306-310
    if (data.api_key_status == ApiKeyStatus::Invalid
        || data.api_key_status == ApiKeyStatus::Missing)
    {
        std::string msg = data.is_remote
            ? "Authentication error \xC2\xB7 Try again"   // ·
            : "Not logged in \xC2\xB7 Run /login";       // ·
        return hbox({ text(msg) | color(Color::Red) })
             | size(HEIGHT, EQUAL, 1);
    }

    // 5. Debug mode — "Debug mode" (warning)
    //    TS REF: Notifications.tsx L311-315
    if (data.debug_mode) {
        return hbox({ text("Debug mode") | color(Color::Yellow) })
             | size(HEIGHT, EQUAL, 1);
    }

    // 6. Verbose token count — "{tokenUsage} tokens" (dim, only when apiKey valid)
    //    TS REF: Notifications.tsx L316-320
    if (data.verbose && data.api_key_status == ApiKeyStatus::Valid
        && data.token_usage > 0)
    {
        return hbox({ text(std::to_string(data.token_usage) + " tokens") | dim })
             | size(HEIGHT, EQUAL, 1);
    }

    // 7. Auto-updater — styled pill (P1: footer-notifications-stub)
    //    TS REF: src/components/AutoUpdater.tsx L176-196
    //    Shows download/install status.  Rendered via NotificationItem with
    //    PillVariant::AutoUpdater so it gets the proper icon + color.
    if (data.auto_updater) {
        NotificationItem au_item;
        au_item.pill.variant = PillVariant::AutoUpdater;
        au_item.pill.auto_updater = *data.auto_updater;
        // Also populate text for accessibility / fallback
        switch (data.auto_updater->status) {
            case AutoUpdaterStatus::Available:
                au_item.text = "New: v" + data.auto_updater->version;
                au_item.color = "info"; break;
            case AutoUpdaterStatus::Downloading:
                au_item.text = "Auto-updating…"; break;
            case AutoUpdaterStatus::Installed:
                au_item.text = "✓ Update installed · Restart to apply";
                au_item.color = "success"; break;
            case AutoUpdaterStatus::Error:
                au_item.text = "✗ Auto-update failed";
                au_item.color = "error"; break;
        }
        Element pill_el = RenderNotificationPill(au_item);
        if (pill_el) {
            return hbox({ std::move(pill_el) }) | size(HEIGHT, EQUAL, 1);
        }
    }

    // 8. New release announcement — purple gift pill (P1: footer-notifications-stub)
    //    TS REF: src/hooks/useUpdateNotification.ts (updateSemver)
    if (data.new_release && !data.new_release->version.empty()) {
        NotificationItem nr_item;
        nr_item.pill.variant = PillVariant::NewRelease;
        nr_item.pill.new_release = *data.new_release;
        nr_item.text = "New: v" + data.new_release->version;
        Element pill_el = RenderNotificationPill(nr_item);
        if (pill_el) {
            return hbox({ std::move(pill_el) }) | size(HEIGHT, EQUAL, 1);
        }
    }

    // 9. Pro renewal reminder — orange clock pill (P1: footer-notifications-stub)
    //    CPP enhancement — no direct TS equivalent.  Shows only when
    //    days_remaining < 7.
    if (data.pro_renewal && data.pro_renewal->days_remaining > 0
        && data.pro_renewal->days_remaining < 7)
    {
        NotificationItem pr_item;
        pr_item.pill.variant = PillVariant::ProRenewal;
        pr_item.pill.pro_renewal = *data.pro_renewal;
        pr_item.text = data.pro_renewal->plan_name + " renews in "
                     + std::to_string(data.pro_renewal->days_remaining) + "d";
        Element pill_el = RenderNotificationPill(pr_item);
        if (pill_el) {
            return hbox({ std::move(pill_el) }) | size(HEIGHT, EQUAL, 1);
        }
    }

    // Nothing active — return empty placeholder row for stable height
    return text(" ") | size(HEIGHT, EQUAL, 1);
}

// ============================================================
// Notification Queue operations
// ============================================================
// These free functions implement the carousel rotation logic.
// They should be called from the engine's event-driven update phase,
// not from a constant-rate ticker (ground rule 5).

namespace detail {

/// Get current steady_clock time as seconds (floating point).
[[nodiscard]] inline double NowSeconds() {
    using clock = std::chrono::steady_clock;
    auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

/// Priority value for sorting (lower = higher priority).
/// TS REF: src/context/notifications.tsx L230-235 (PRIORITIES record)
[[nodiscard]] inline int PriorityValue(NotificationPriority p) {
    switch (p) {
        case NotificationPriority::Immediate: return 0;
        case NotificationPriority::High:      return 1;
        case NotificationPriority::Medium:    return 2;
        case NotificationPriority::Low:       return 3;
    }
    return 3;
}

/// Find the highest-priority item in the queue (lowest PriorityValue).
/// TS REF: src/context/notifications.tsx L236-239 (getNext function)
[[nodiscard]] inline std::size_t FindHighestPriorityIndex(
    const std::vector<NotificationItem>& queue)
{
    if (queue.empty()) return 0;
    std::size_t best = 0;
    for (std::size_t i = 1; i < queue.size(); ++i) {
        if (PriorityValue(queue[i].priority) < PriorityValue(queue[best].priority)) {
            best = i;
        }
    }
    return best;
}

} // namespace detail

/// Add a notification to the queue.
///
/// Faithful to TS addNotification() in notifications.tsx:
///   - "immediate" priority → replaces current right away, re-queues previous
///   - Other priorities → added to queue (deduped by key)
///   - Queue is capped at kMaxItems (12); oldest lowest-priority evicted
///   - If item.invalidates is set, matching keys are removed from queue/current
///
/// TS REF: src/context/notifications.tsx L78-192
inline void QueueAddNotification(NotificationQueue& nq,
                                  const NotificationItem& item)
{
    // Prevent duplicates — TS uses queuedKeys Set (L173)
    const bool already_in_queue = [&]() {
        for (const auto& q : nq.queue) {
            if (q.key == item.key) return true;
        }
        return false;
    }();
    const bool already_current = nq.current && nq.current->key == item.key;
    if (already_in_queue || already_current) return;

    // Handle invalidation — remove matching keys from queue and current
    // TS REF: notifications.tsx L176-186 (invalidatesCurrent + queue filter)
    if (!item.invalidates.empty()) {
        // Check if current is invalidated
        if (nq.current) {
            for (const auto& inv_key : item.invalidates) {
                if (nq.current->key == inv_key) {
                    nq.current.reset();
                    break;
                }
            }
        }
        // Remove invalidated items from queue
        nq.queue.erase(
            std::remove_if(nq.queue.begin(), nq.queue.end(),
                [&](const NotificationItem& q) {
                    for (const auto& inv_key : item.invalidates) {
                        if (q.key == inv_key) return true;
                    }
                    return false;
                }),
            nq.queue.end());
    }

    // "immediate" priority → show right now
    // TS REF: notifications.tsx L80-116
    if (item.priority == NotificationPriority::Immediate) {
        // Re-queue the current item if it's not immediate
        if (nq.current && nq.current->priority != NotificationPriority::Immediate) {
            // Cap queue: if full, evict oldest lowest-priority
            if (nq.queue.size() >= NotificationQueue::kMaxItems) {
                // Find lowest-priority item, prefer older ones
                std::size_t worst = 0;
                for (std::size_t i = 1; i < nq.queue.size(); ++i) {
                    if (detail::PriorityValue(nq.queue[i].priority) >
                        detail::PriorityValue(nq.queue[worst].priority)) {
                        worst = i;
                    }
                }
                nq.queue.erase(nq.queue.begin() + static_cast<std::ptrdiff_t>(worst));
            }
            nq.queue.push_back(*nq.current);
        }
        nq.current = item;
        nq.current_activated_at_sec = detail::NowSeconds();
        return;
    }

    // Non-immediate → add to queue (capped at kMaxItems)
    if (nq.queue.size() >= NotificationQueue::kMaxItems) {
        // Evict the oldest lowest-priority item to make room
        std::size_t worst = 0;
        for (std::size_t i = 1; i < nq.queue.size(); ++i) {
            if (detail::PriorityValue(nq.queue[i].priority) >
                detail::PriorityValue(nq.queue[worst].priority)) {
                worst = i;
            }
        }
        nq.queue.erase(nq.queue.begin() + static_cast<std::ptrdiff_t>(worst));
    }
    nq.queue.push_back(item);
}

/// Remove a notification by key (from both current and queue).
/// TS REF: src/context/notifications.tsx L193-213 (removeNotification)
inline void QueueRemoveNotification(NotificationQueue& nq,
                                     const std::string& key)
{
    // Remove from current
    if (nq.current && nq.current->key == key) {
        nq.current.reset();
    }
    // Remove from queue
    nq.queue.erase(
        std::remove_if(nq.queue.begin(), nq.queue.end(),
            [&](const NotificationItem& q) { return q.key == key; }),
        nq.queue.end());
}

/// Advance the queue: if current has expired, clear it and pull the next
/// highest-priority item from the queue.
///
/// Returns true if the display changed (current was advanced or cleared).
///
/// This should be called from the engine's event-driven update loop — NOT
/// from a constant-rate ticker (ground rule 5).  The engine drives repaints
/// on user input, API responses, etc., which is frequent enough that
/// timeout expiry will be detected within reasonable accuracy.
///
/// TS REF: src/context/notifications.tsx L46-77 (processQueue callback)
/// TS REF: src/context/notifications.tsx L52-68 (setTimeout expiry handler)
[[nodiscard]] inline bool QueueAdvance(NotificationQueue& nq, double now_sec)
{
    // If nothing is current, try to pull from queue
    if (!nq.current) {
        if (nq.queue.empty()) return false;
        // Get highest-priority item from queue
        std::size_t idx = detail::FindHighestPriorityIndex(nq.queue);
        nq.current = nq.queue[idx];
        nq.queue.erase(nq.queue.begin() + static_cast<std::ptrdiff_t>(idx));
        nq.current_activated_at_sec = now_sec;
        return true;
    }

    // Check if current has expired
    const double elapsed_sec = now_sec - nq.current_activated_at_sec;
    const double timeout_sec = nq.current->timeout_ms / 1000.0;
    if (elapsed_sec < timeout_sec) {
        return false;  // Not yet expired
    }

    // Current has expired — clear it
    nq.current.reset();

    // Try to pull next from queue
    if (!nq.queue.empty()) {
        std::size_t idx = detail::FindHighestPriorityIndex(nq.queue);
        nq.current = nq.queue[idx];
        nq.queue.erase(nq.queue.begin() + static_cast<std::ptrdiff_t>(idx));
        nq.current_activated_at_sec = now_sec;
    }
    return true;
}

/// Convenience overload that uses the current steady_clock time.
[[nodiscard]] inline bool QueueAdvance(NotificationQueue& nq) {
    return QueueAdvance(nq, detail::NowSeconds());
}

/// Get the text and color of the currently-displayed notification.
/// Returns nullopt if nothing is current (queue is idle).
/// This is a const read — it does NOT advance the queue.
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>>
QueueGetCurrentDisplay(const NotificationQueue& nq)
{
    if (!nq.current) return std::nullopt;
    return std::make_pair(nq.current->text, nq.current->color);
}

/// Check if the queue has any items (either current or pending).
[[nodiscard]] inline bool QueueHasItems(const NotificationQueue& nq) {
    return nq.current.has_value() || !nq.queue.empty();
}

// ============================================================
// Typed notification convenience helpers (P1: footer-notifications-stub)
// ============================================================
// These helpers construct properly-configured NotificationItems for the
// most common notification types and add them to the queue.
//
// TS REFERENCE:
//   src/context/notifications.tsx L78-192 (addNotification function)
//   src/components/AutoUpdater.tsx (auto-updater result → notification)
//   src/hooks/useUpdateNotification.ts (new version → notification)
//
// Each helper:
//   - Sets a unique key for dedup
//   - Configures the pill variant with typed data
//   - Sets appropriate priority and timeout
//   - Calls QueueAddNotification to enqueue

/// Add an API key status notification pill to the queue.
///
/// TS REF: src/components/PromptInput/Notifications.tsx L306-310
///   - Valid:   green check pill (low priority, informational)
///   - Invalid: red X pill (immediate priority — user needs to act)
///   - Missing: yellow ! pill (immediate priority)
///   - Unknown: no-op (nothing to show)
///
/// Invalidates any existing "api-key" notification.
inline void AddApiKeyNotification(NotificationQueue& nq, ApiKeyStatus status) {
    if (status == ApiKeyStatus::Unknown) return;

    NotificationItem item;
    item.key = "api-key-status";
    item.pill.variant = PillVariant::ApiKey;
    item.pill.api_key_status = status;
    item.invalidates = { "api-key-status" };   // replace existing

    switch (status) {
        case ApiKeyStatus::Valid:
            item.text = "\xF0\x9F\x94\x91 \xE2\x9C\x93 API key OK";   // 🔑 ✓
            item.color = "success";
            item.priority = NotificationPriority::Low;
            item.timeout_ms = 5000;   // brief confirmation
            break;
        case ApiKeyStatus::Invalid:
            item.text = "\xF0\x9F\x94\x91 \xE2\x9C\x97 Invalid key";   // 🔑 ✗
            item.color = "error";
            item.priority = NotificationPriority::Immediate;
            item.timeout_ms = 15000;  // stays visible longer
            break;
        case ApiKeyStatus::Missing:
            item.text = "\xF0\x9F\x94\x91 ! Not logged in";   // 🔑 !
            item.color = "warning";
            item.priority = NotificationPriority::Immediate;
            item.timeout_ms = 15000;
            break;
        case ApiKeyStatus::Unknown:
            return;
    }

    QueueAddNotification(nq, item);
}

/// Add an auto-updater status notification pill to the queue.
///
/// TS REF: src/components/AutoUpdater.tsx L176-196
///   - Available:   blue "⬇ New: vX.Y.Z" (medium priority)
///   - Downloading: dim "⟳ Auto-updating…" (immediate — user is waiting)
///   - Installed:   green "✓ Update installed · Restart to apply" (immediate)
///   - Error:       red "✗ Auto-update failed" (immediate)
///
/// Invalidates any existing "auto-updater" notification.
inline void AddAutoUpdaterNotification(NotificationQueue& nq,
                                        const AutoUpdaterData& data)
{
    NotificationItem item;
    item.key = "auto-updater";
    item.pill.variant = PillVariant::AutoUpdater;
    item.pill.auto_updater = data;
    item.invalidates = { "auto-updater" };   // replace existing

    switch (data.status) {
        case AutoUpdaterStatus::Available:
            item.text = "\xE2\xAC\x87 New: v" + data.version;   // ⬇
            item.color = "info";
            item.priority = NotificationPriority::Medium;
            item.timeout_ms = 10000;
            break;
        case AutoUpdaterStatus::Downloading:
            item.text = "\xE2\x9F\xB3 Auto-updating…";   // ⟳
            item.color = "";   // dim
            item.priority = NotificationPriority::Immediate;
            item.timeout_ms = 30000;  // downloading takes a while
            break;
        case AutoUpdaterStatus::Installed:
            item.text = "\xE2\x9C\x93 Update installed \xC2\xB7 Restart to apply";   // ✓ ·
            item.color = "success";
            item.priority = NotificationPriority::Immediate;
            item.timeout_ms = 20000;
            break;
        case AutoUpdaterStatus::Error:
            item.text = "\xE2\x9C\x97 Auto-update failed";   // ✗
            if (!data.error_detail.empty()) {
                item.text += " \xC2\xB7 " + data.error_detail;
            }
            item.color = "error";
            item.priority = NotificationPriority::Immediate;
            item.timeout_ms = 20000;
            break;
    }

    QueueAddNotification(nq, item);
}

/// Add a Pro/Team subscription renewal reminder pill.
///
/// CPP enhancement — no direct TS equivalent.  Shows only when
/// days_remaining < 7 (configurable urgency threshold).
///
///   - days <= 3:  orange warning (high priority)
///   - days 4-6:   blue info (medium priority)
///   - days >= 7:  no-op (not urgent enough)
///
/// Invalidates any existing "pro-renewal" notification.
inline void AddProRenewalNotification(NotificationQueue& nq,
                                       const ProRenewalData& data,
                                       int urgency_threshold_days = 7)
{
    if (data.days_remaining >= urgency_threshold_days) return;
    if (data.days_remaining <= 0) return;   // already expired

    NotificationItem item;
    item.key = "pro-renewal";
    item.pill.variant = PillVariant::ProRenewal;
    item.pill.pro_renewal = data;
    item.invalidates = { "pro-renewal" };

    const std::string plan = data.plan_name.empty() ? "Pro" : data.plan_name;
    item.text = "\xE2\x8F\xB1 " + plan + " renews in "
              + std::to_string(data.days_remaining) + "d";   // ⏱

    if (data.days_remaining <= 3) {
        item.color = "warning";
        item.priority = NotificationPriority::High;
    } else {
        item.color = "info";
        item.priority = NotificationPriority::Medium;
    }
    item.timeout_ms = 12000;

    QueueAddNotification(nq, item);
}

/// Add a new release announcement pill.
///
/// TS REF: src/hooks/useUpdateNotification.ts (updateSemver)
/// Shows "🎁 New: vX.Y.Z" in purple/magenta.
///
/// Invalidates any existing "new-release" notification.
inline void AddNewReleaseNotification(NotificationQueue& nq,
                                       const NewReleaseData& data)
{
    if (data.version.empty()) return;

    NotificationItem item;
    item.key = "new-release";
    item.pill.variant = PillVariant::NewRelease;
    item.pill.new_release = data;
    item.text = "\xF0\x9F\x8E\x81 New: v" + data.version;   // 🎁
    item.color = "info";   // purple pill uses custom color in renderer
    item.priority = NotificationPriority::Medium;
    item.timeout_ms = 12000;
    item.invalidates = { "new-release", "auto-updater" };   // new release supersedes available update

    QueueAddNotification(nq, item);
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
    NotificationData notification;        // P1: footer notifications
    bool show_notifications = false;      // non-fullscreen only
    bool is_undercover = false;           // ant-only

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
    // P0-6: Also show when builtin fallback data is available (standalone mode
    // where user's command returns empty or no statusLine configured).
    const bool has_builtin = opts.status_line.builtin.has_value();
    bool show_status_line = opts.status_line.should_display
        && (opts.is_fullscreen || !opts.status_line.content.empty() || has_builtin);
    if (show_status_line) {
        left_col.push_back(RenderStatusLine(opts.status_line));
    }

    // LeftSide (always present — ModeIndicator reserves 1 row)
    left_col.push_back(RenderLeftSide(opts.left_side));

    Element left_el = vbox(std::move(left_col)) | flex;

    // ── Right column ───────────────────────────────────────────────────
    // TS: <Box flexDirection="column" alignItems={isNarrow ? 'flex-start' : 'flex-end'}>
    // Right side is a vertical column: notifications stack on top,
    // undercover + bridge status form a bottom row.
    Elements right_col;

    // Notifications (shown in both fullscreen and non-fullscreen in CPP
    // since we reserve stable height; TS hides in fullscreen to save scroll
    // rows, but our BuiltinStatusLine already covers the info need).
    // TS REF: Notifications.tsx — renders NotificationContent as a column.
    {
        Element notif_el = RenderNotifications(opts.notification);
        // Check if the notification element has actual content (not just
        // a blank placeholder row).  We detect this by checking if the
        // notification data has any active field.
        const auto& nd = opts.notification;
        const bool has_active =
            QueueHasItems(nd.queue) ||
            (nd.dynamic_text && !nd.dynamic_text->empty()) ||
            nd.ide.connected ||
            nd.is_overage_mode ||
            nd.api_key_status == ApiKeyStatus::Invalid ||
            nd.api_key_status == ApiKeyStatus::Missing ||
            nd.debug_mode ||
            (nd.verbose && nd.api_key_status == ApiKeyStatus::Valid && nd.token_usage > 0) ||
            // P1: typed notification pills (footer-notifications-stub)
            nd.auto_updater.has_value() ||
            nd.new_release.has_value() ||
            (nd.pro_renewal.has_value()
             && nd.pro_renewal->days_remaining > 0
             && nd.pro_renewal->days_remaining < 7);

        if (has_active) {
            right_col.push_back(hbox({
                filler(),   // right-align (TS: alignItems="flex-end")
                std::move(notif_el),
            }));
        }
    }

    // Bottom row: undercover + bridge status (inline items)
    Elements bottom_row;

    // Undercover (ant-only)
    if (opts.is_undercover) {
        if (!bottom_row.empty()) bottom_row.push_back(text(" ") | dim);
        bottom_row.push_back(text("undercover") | dim);
    }

    // Bridge status indicator
    if (opts.bridge.status != BridgeStatus::Disabled
        && (opts.bridge.explicit_remote
            || opts.bridge.status == BridgeStatus::Reconnecting))
    {
        if (!bottom_row.empty()) bottom_row.push_back(text(" ") | dim);
        bottom_row.push_back(RenderBridgeStatus(opts.bridge));
    }

    if (!bottom_row.empty()) {
        right_col.push_back(hbox({
            filler(),
            hbox(std::move(bottom_row)),
        }));
    }

    const bool has_right = !right_col.empty();
    Element right_el = has_right ? vbox(std::move(right_col)) : text("");

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
