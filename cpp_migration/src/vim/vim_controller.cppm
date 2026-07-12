/// @file vim_controller.cppm
/// @brief VimController — unified vim state container shared by all vim-enabled
///        input components (TextInputImpl, VimInput, etc.).
///
/// Single source of truth for vim editing state: current mode, pending
/// operators, yank registers, and linewise flags.  Replaces the scattered
/// vim_mode_current_ / vim_pending_operator_ / vim_yank_register_ fields
/// that previously lived in each consumer independently.
///
/// TS REF: src/hooks/useVimInput.ts:36 — VimInputState tracks mode, registers,
///   pending operators, and undo history for the vim input hook.
/// TS REF: src/vim/transitions.ts — mode transitions and operator pending.
module;

#include <cstdint>
#include <string>

export module cc.vim.vim_controller;

import cc.vim.vim_types;

export namespace cc::vim {

using cc::vim::VimMode;

/// Bundled vim editing state.  One instance per input widget.
struct VimController {
    // ── Mode ────────────────────────────────────────────────────────
    VimMode mode = VimMode::Insert;       ///< Current editing mode.

    // ── Pending operators ───────────────────────────────────────────
    std::string pending_operator;         ///< "d", "y", or "c" waiting for motion/2nd key.
    bool pending_g = false;               ///< True after 'g', waiting for 2nd key (gg/gj/gk).
    bool pending_replace = false;         ///< True after 'r', waiting for replacement char.

    // ── Registers ───────────────────────────────────────────────────
    std::string unnamed_register;         ///< Default yank register (").
    bool yank_is_linewise = false;        ///< Last yank was linewise (yy/dd).

    // ── Counts / repeats ────────────────────────────────────────────
    int count_prefix = 0;                 ///< Numeric prefix for commands (e.g. 3dw).

    // ── Helper accessors ────────────────────────────────────────────
    /// True when in a navigation mode (Normal, Visual*, Command).
    [[nodiscard]] bool is_navigation() const noexcept {
        return cc::vim::is_navigation_mode(mode);
    }

    /// True when in an editing mode (Insert, Replace).
    [[nodiscard]] bool is_editing() const noexcept {
        return cc::vim::is_editing_mode(mode);
    }

    /// True when in any visual mode (Visual, VisualLine, VisualBlock).
    [[nodiscard]] bool is_visual() const noexcept {
        return mode == VimMode::Visual ||
               mode == VimMode::VisualLine ||
               mode == VimMode::VisualBlock;
    }

    /// Reset to Insert mode and clear all pending state.
    void enter_insert() noexcept {
        mode = VimMode::Insert;
        pending_operator.clear();
        pending_g = false;
        pending_replace = false;
    }

    /// Reset to Normal mode and clear pending operator state.
    void enter_normal() noexcept {
        mode = VimMode::Normal;
        pending_operator.clear();
        pending_g = false;
        pending_replace = false;
    }

    /// Store text in the unnamed register, optionally marking linewise.
    void yank(std::string text, bool linewise = false) noexcept {
        unnamed_register = std::move(text);
        yank_is_linewise = linewise;
    }
};

} // namespace cc::vim
