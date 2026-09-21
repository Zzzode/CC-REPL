/// @file idle_return_dialog.cppm
/// @brief Faithful port of src/components/IdleReturnDialog.tsx
///
/// Shows a "Welcome Back" dialog when the user returns to an idle session.
/// Three Select-style options: Continue this conversation / Start new /
/// "Don't ask me again" — matching TS IdleReturnAction =
/// 'continue' | 'clear' | 'dismiss' | 'never'.
///
/// Renderer lives in cc.ui.dialogs.idle_return_dialog.  The dialog is a
/// Bottom-slot, Band4 dialog (no typing/animation suppression).
///
/// Rendering uses FTXUI window() + border() — matching the pattern used by
/// all other repl_screen.cppm dialog lambdas (CostThreshold, etc.) so the
/// visual chrome (rounded border + titled header) is consistent with the rest of
/// the migrated UI.
module;

#include <string>
#include <string_view>
#include <format>
#include <cstdint>
#include <array>
#include <functional>
#include <optional>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.idle_return_dialog;

export namespace cc::ui::dialogs::idle_return {

using namespace ftxui;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// Mirrors TS `type IdleReturnAction = 'continue' | 'clear' | 'dismiss' | 'never'`.
enum class IdleReturnAction : std::uint8_t {
    Continue,  ///< Resume existing conversation (default).
    Clear,     ///< Start a new conversation (clear context).
    Dismiss,   ///< Esc / onCancel — treated as "resume, don't auto-submit".
    Never,     ///< "Don't ask me again" — persist dismissal flag.
};

/// Selection state.  The Select component exposes a 0-based index into the
/// three-option list; we keep the index here so ArrowUp/Down can rotate
/// the selection just like TS CustomSelect.
struct IdleReturnState {
    std::uint32_t idle_minutes{0};
    std::uint64_t total_input_tokens{0};
    /// Currently highlighted option in the 3-item Select.
    int selected_index{0};

    /// Callback invoked once the user commits an action.  NOT fired on
    /// keystrokes that merely rotate the selection (ArrowUp/Down).
    std::function<void(IdleReturnAction)> on_done;
};

// ---------------------------------------------------------------------------
// Format helpers (faithful ports of TS helpers)
// ---------------------------------------------------------------------------

/// TS IdleReturnDialog.formatIdleDuration — rounds DOWN:
///   `< 1m`, `Nm`, `Nh`, `Nh Mm`.
[[nodiscard]] inline std::string format_idle_duration(std::uint32_t minutes) {
    if (minutes < 1) return "< 1m";
    if (minutes < 60) return std::format("{}m", minutes);
    const auto hours = minutes / 60;
    const auto rem   = minutes % 60;
    if (rem == 0) return std::format("{}h", hours);
    return std::format("{}h {}m", hours, rem);
}

/// TS utils.formatTokens → formatNumber(count).replace('.0', '').
/// Uses K/M/B compact notation for >= 1000 (lowercase suffix, strip .0).
[[nodiscard]] inline std::string format_tokens(std::uint64_t count) {
    if (count < 1000) return std::format("{}", count);
    double v = static_cast<double>(count);
    const char* suffix = "k";
    if (v >= 1'000'000'000.0)      { v /= 1'000'000'000.0; suffix = "b"; }
    else if (v >= 1'000'000.0)        { v /= 1'000'000.0;         suffix = "m"; }
    else                                 { v /= 1'000.0; }
    // 1 decimal, strip trailing ".0" — same as TS replace(".0", '').
    std::string s = std::format("{:.1f}{}",
        std::floor(v * 10.0 + 0.5) / 10.0, suffix);
    if (auto pos = s.find(".0"); pos != std::string::npos) s.erase(pos, 2);
    return s;
}

/// Index → action mapping for the three-item Select.
[[nodiscard]] inline IdleReturnAction action_for_index(int idx) noexcept {
    switch (idx) {
        case 0:  return IdleReturnAction::Continue;
        case 1:  return IdleReturnAction::Clear;
        default: return IdleReturnAction::Never;
    }
}

/// Option labels — mirrors TS IdleReturnDialog options[] exactly.
inline constexpr std::array<std::string_view, 3> kOptionLabels = {
    "Continue this conversation",
    "Send message as a new conversation",
    "Don't ask me again",
};

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

/// Faithful port of TS IdleReturnDialog output:
///
///   ┌─ Welcome Back ───────────────────────────┐
///   │  ▸ You've been away 3h 15m and …    │
///   │  this conversation is 12.5k tokens.    │
///   │  ──────────────────────────────────  │
///   │  If this is a new task, clearing │
///   │  context will save usage and be     │
///   │  faster.                             │
///   │  ──────────────────────────────────  │
///   │  ● Continue this conversation     │
///   │  ○ Send message as a new conversation  │
///   │  ○ Don't ask me again           │
///   │  ──────────────────────────────────  │
///   │  [Enter] confirm   [Esc] cancel │
///   └─────────────────────────────────────┘
///
/// Layout order matches TS:
///   1. Titled window (Pane-style, Info/Cyan color)
///   2. Title (bold, Info): "You've been away …"
///   3. separator()
///   4. Body paragraph (dim)
///   5. separator()
///   6. 3-option CustomSelect clone (●/○ Unicode bullets, selected bold+Cyan)
///   7. separator()
///   8. Byline keyboard hint (italic, dim): Enter confirm / Esc cancel)
[[nodiscard]] inline Element RenderIdleReturnDialog(const IdleReturnState& st) {
    using namespace ftxui;

    // ── Title (line 1) ──────────────────────────────────────
    const std::string title_text = std::format(
        "You've been away {} and this conversation is {} tokens.",
        format_idle_duration(st.idle_minutes),
        format_tokens(st.total_input_tokens));

    // ── Options (TS CustomSelect clone) ──────────────────────────
    //    TS CustomSelect renders: (● selected / ○ unselected) with the selected
    // option in accent color, unselected dim.
    Elements options;
    options.reserve(kOptionLabels.size());
    for (std::size_t i = 0; i < kOptionLabels.size(); ++i) {
        const bool selected = (static_cast<int>(i) == st.selected_index);
        Elements row;
        row.push_back(text(selected ? "● " : "○ ") |
                      (selected ? bold | color(Color::CyanLight) : dim));
        row.push_back(text(std::string(kOptionLabels[i])) |
                      (selected ? bold | color(Color::White) : dim));
        options.push_back(hbox(std::move(row)));
    }

    // ── Keyboard byline (TS Dialog defaultInputGuide) ───────
    //    Dim, matches "Enter confirm / Esc cancel".
    auto byline = hbox({
        text("Enter") | bold | dim,
        text(" confirm") | dim,
        text("  ") | dim,
        text("Esc") | bold | dim,
        text(" cancel") | dim,
    }) | dim;

    // ── Assemble inner content ───────────────────────────────
    auto inner = vbox({
        text(title_text) | bold | color(Color::Cyan),
        separator(),
        text("If this is a new task, clearing context will save "
             "usage and be faster.") | color(Color::GrayLight),
        separator(),
        vbox(std::move(options)),
        separator(),
        byline,
    }) | size(WIDTH, GREATER_THAN, 52);

    // ── Wrap in titled Pane (rounded, Info colored) ─────────────
    //    TS wraps in <Dialog> → <Pane color='permission'> with border.
    return window(text(" Welcome Back ") | color(Color::Cyan),
                 std::move(inner)) | color(Color::Cyan);
}

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

/// Keyboard handler, one-to-one with TS IdleReturnDialog semantics:
///
///   Enter         → on_done(action_for_index(selected_index))
///   Esc           → on_done(Dismiss)  (TS onCancel → onDone('dismiss'))
///   'n'/'N'       → quick-shortcut: IdleReturnAction::Clear (start new)
///   ArrowUp/Down   → rotate selected (wrap, no callback)
///   'j'/'k'       → vi-style rotation (same as ArrowDown / ArrowUp)
///   '1'/'2'/'3'   → jump to option N and commit
///   'c'/'C'       → quick Continue
///   'd'/'D'       → quick "Don't ask me again" (Never)
///
/// Returns true if the event was consumed.
inline bool HandleIdleReturnEvent(IdleReturnState& st, const Event& ev) {
    // Esc → dismiss (TS onCancel → onDone('dismiss')).
    if (ev == Event::Escape) {
        if (st.on_done) st.on_done(IdleReturnAction::Dismiss);
        return true;
    }
    // Enter → commit currently selected option.
    if (ev == Event::Return) {
        if (st.on_done) st.on_done(action_for_index(st.selected_index));
        return true;
    }
    // ArrowDown / j → next (wrap)).
    if (ev == Event::ArrowDown ||
        ev == Event::Character('j') || ev == Event::Character('J')) {
        st.selected_index =
            (st.selected_index + 1) % static_cast<int>(kOptionLabels.size());
        return true;
    }
    // ArrowUp / k → prev (wrap)).
    if (ev == Event::ArrowUp ||
        ev == Event::Character('k') || ev == Event::Character('K')) {
        const auto n = static_cast<int>(kOptionLabels.size());
        st.selected_index = (st.selected_index - 1 + n) % n;
        return true;
    }
    // Character shortcuts).
    if (ev.is_character()) {
        const char c = ev.character()[0];
        // 'n' / 'N' — quick "start new conversation".
        if (c == 'n' || c == 'N') {
            if (st.on_done) st.on_done(IdleReturnAction::Clear);
            return true;
        }
        // 'c' / 'C' — quick "continue".
        if (c == 'c' || c == 'C') {
            if (st.on_done) st.on_done(IdleReturnAction::Continue);
            return true;
        }
        // 'd' / 'D' — quick "don't ask me again".
        if (c == 'd' || c == 'D') {
            if (st.on_done) st.on_done(IdleReturnAction::Never);
            return true;
        }
        // 1 / 2 / 3 → pick option directly + commit).
        if (c >= '1' && c <= '3') {
            st.selected_index = c - '1';
            if (st.on_done) st.on_done(action_for_index(st.selected_index));
            return true;
        }
    }
    return false;
}

} // namespace cc::ui::dialogs::idle_return