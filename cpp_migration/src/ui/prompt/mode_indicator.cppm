module;
#include <string>

export module cc.ui.prompt.mode_indicator;

import cc.ui.design.figures;
import cc.ui.common.types;  // unified PromptInputMode canonical enum

// TS REF: src/components/PromptInput/PromptInputModeIndicator.tsx
//
// The prompt "mode indicator" in the TS original renders EXACTLY ONE of two
// glyphs at the start of the input line:
//
//   * bash mode (leading '!')  → figures pointer replaced by '!'  (bashBorder)
//   * every other mode         → figures.pointer  '❯'             (theme text)
//
// It never renders per-mode badge pills.  This module previously emitted a
// CPP-only badge system ("● NORMAL / ◆ PLAN / ▶ VIM / ◎ SEARCH") which was
// one of the "three competing prefix systems" flagged as a P0 faithful-port
// blocker (audit round7: prefix-glyph-no-unified-impl).  Slash / plan / vim /
// search are ORTHOGONAL routing/state concepts and are surfaced elsewhere
// (slash routing, plan footer badge, vim status row) — NOT as a prompt-prefix
// glyph.  See cc::ui::design::figures for the single source of truth and the
// live render site in repl_screen.cppm::RenderPromptInput.

export namespace cc::ui::prompt {

namespace figs = cc::ui::design::figures;

// Prompt input mode — unified canonical enum from cc::ui::common.
// Previously this file defined a local 2-value InputMode {Normal, Bash}.
// Old→new mapping: both values retain their names in the unified enum.
// TS REF: src/types/textInputTypes.ts:265 (PromptInputMode type)
using InputMode = cc::ui::common::PromptInputMode;

// Map the CPP prompt-prefix InputMode to the shared figures PromptMode enum.
// TS REF: inputModes.ts getModeFromInput — bash is the only glyph-changing mode.
[[nodiscard]] inline auto to_prompt_mode(InputMode mode) noexcept
    -> figs::PromptMode {
    return mode == InputMode::Bash ? figs::PromptMode::kBash
                                   : figs::PromptMode::kPrompt;
}

// Return the raw prompt-prefix glyph (no trailing space) for the given mode.
// TS REF: PromptInputModeIndicator.tsx:82 (bash → '!', else figures.pointer).
[[nodiscard]] inline auto prompt_glyph(InputMode mode) -> std::string {
    return mode == InputMode::Bash ? std::string(figs::kBashGlyph)
                                   : std::string(figs::kPointer);
}

// Render the prompt-prefix indicator: glyph + trailing space, matching TS's
// `{figures.pointer}&nbsp;` / `!&nbsp;` (2 display cells total).
//
// Colour is intentionally NOT baked in here: TS applies it via Ink's
// <Text color=…> at the render site (bashBorder for bash, teammate/theme.text
// otherwise).  Callers that need colour should wrap this in their own
// FTXUI `color(...)` decorator — see repl_screen.cppm::RenderPromptInput.
[[nodiscard]] inline auto render_mode_indicator(InputMode mode) -> std::string {
    return mode == InputMode::Bash ? std::string(figs::kBashPrefix)
                                   : std::string(figs::kPointerPrefix);
}

} // namespace cc::ui::prompt
