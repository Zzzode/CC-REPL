/// @file vim_types.cppm
/// @brief Canonical VimMode enum — single source of truth for all vim mode
///        representations across the codebase.
///
/// Lives in cc_vim (low-level target, depends only on cc_utils) so both
/// cc_hooks and cc_ui can safely import it without circular dependencies.
///
/// Replaces 5 incompatible local VimMode definitions:
///   - src/ui/prompt/vim_input.cppm (6 values)
///   - src/ui/prompt_input.cppm (3 values)
///   - src/vim/vim_mode.cppm (6 values)
///   - src/hooks/vim_input.cppm (5 values)
///   - src/ui/components/text_input.cppm (bool enable_vim — no enum)
///
/// TS REF: src/types/textInputTypes.ts:222 — public VimMode type is
///   'INSERT' | 'NORMAL' (2 values).  Internal vim state machine
///   (src/vim/transitions.ts, src/vim/operators.ts) tracks richer modes.
/// TS REF: src/hooks/useVimInput.ts:36 — mode starts at 'INSERT'.
module;

#include <cstdint>
#include <string_view>

export module cc.vim.vim_types;

export namespace cc::vim {

/// Canonical vim editing mode.
enum class VimMode : std::uint8_t {
    Normal,      ///< Navigation / command mode (TS: 'NORMAL').
    Insert,      ///< Text insertion mode (TS: 'INSERT').  Default on focus.
    Visual,      ///< Character-wise visual selection (v).
    VisualLine,  ///< Line-wise visual selection (V).
    VisualBlock, ///< Block-wise visual selection (Ctrl+V).
    Replace,     ///< Single-character replace mode (R).
    Command,     ///< Ex command-line mode (:).
};

/// Returns true for modes where typed characters are inserted into text
/// (Insert, Replace).  TS REF: useVimInput.ts:209 — only INSERT mode
/// passes characters to the base textInput handler.
[[nodiscard]] constexpr bool is_editing_mode(VimMode m) noexcept {
    return m == VimMode::Insert || m == VimMode::Replace;
}

/// Returns true for navigation / command modes (Normal, Visual, VisualLine,
/// VisualBlock, Command).  In these modes, typed keys are vim commands,
/// not text input.
[[nodiscard]] constexpr bool is_navigation_mode(VimMode m) noexcept {
    return !is_editing_mode(m);
}

/// Display label for the mode indicator (e.g. "-- NORMAL --", "-- INSERT --").
/// TS REF: vim mode indicator shown in statusline / footer.
[[nodiscard]] constexpr std::string_view vim_mode_label(VimMode m) noexcept {
    switch (m) {
        case VimMode::Normal:      return "-- NORMAL --";
        case VimMode::Insert:      return "-- INSERT --";
        case VimMode::Visual:      return "-- VISUAL --";
        case VimMode::VisualLine:  return "-- VISUAL LINE --";
        case VimMode::VisualBlock: return "-- VISUAL BLOCK --";
        case VimMode::Replace:     return "-- REPLACE --";
        case VimMode::Command:     return ":";
    }
    return "-- ??? --";
}

/// Short mode name for badges / compact display (e.g. "NORMAL", "INSERT").
[[nodiscard]] constexpr std::string_view vim_mode_short_label(VimMode m) noexcept {
    switch (m) {
        case VimMode::Normal:      return "NORMAL";
        case VimMode::Insert:      return "INSERT";
        case VimMode::Visual:      return "VISUAL";
        case VimMode::VisualLine:  return "V-LINE";
        case VimMode::VisualBlock: return "V-BLOCK";
        case VimMode::Replace:     return "REPLACE";
        case VimMode::Command:     return "COMMAND";
    }
    return "UNKNOWN";
}

} // namespace cc::vim
