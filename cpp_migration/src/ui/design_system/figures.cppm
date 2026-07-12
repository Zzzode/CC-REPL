/// @file figures.cppm
/// @brief Shared UI glyph and figure constants (TS `utils/figures.ts` +
///        `src/components/PromptInput/inputModes.ts` port).
///
/// Every "glyph" visible to the user — the prompt `❯`, bash-mode `!`, check
/// marks, crosses, spinner frames, connector lines, etc. — is defined ONCE
/// here so that render sites scattered across 10+ .cppm files don't each
/// hard-code slightly different UTF-8 byte sequences or wrong characters
/// (was the source of the CPP Round 1-6 "prefix glyph three fights" bug).
///
/// TS REFERENCE (port verbatim):
///   - `node_modules/figures/index.js` (main default glyph set, platform aware)
///   - `src/utils/figures.ts` (CC-local extensions like BRIDGE_SPINNER_FRAMES)
///   - `src/components/PromptInput/inputModes.ts` (mode detection + prefix)
///
/// LANGUAGE NOTE:
///   C++23 named modules.  All constants are `inline constexpr std::string_view`
///   with explicit UTF-8 byte escapes so every compiler/platform sees the
///   exact same bytes.  Use `std::string(kFigsXxx)` when passing to FTXUI
///   APIs that want a value string instead of a view.
// ────────────────────────────────────────────────────────────────────────
module;

#include <array>
#include <cstdlib>      // for std::getenv (is_unicode_supported)
#include <string>       // for std::string (replace_symbols)
#include <string_view>

// FTXUI Color.hpp: not used here (figures module is glyphs-only, no colors).
#include <ftxui/dom/elements.hpp>  // for ftxui::Color forward refs that
                                   // downstream consumers need, and to keep
                                   // the module graph consistent.
export module cc.ui.design.figures;

export namespace cc::ui::design::figures {

using namespace std::string_view_literals;

// ─── Prompt / command prefix glyphs ────────────────────────────────────
// TS: `figures.pointer` = '❯' U+276F HEAVY RIGHT-POINTING ANGLE QUOTATION
//     MARK ORNAMENT.  Used as default prompt prefix and user transcript
//     message prefix.  Followed by NBSP/U+00A0 in TS PromptInputModeIndicator.
//
// NOTE: We expose TWO symbols:
//   kPointer        — glyph alone (2 UTF-8 bytes: 0xE2 0x9D 0xAF → 3 bytes total
//                     in UTF-8, displays as ONE cell — confirmed by wcwidth)
//   kPointerPrefix  — glyph + ASCII space, 2 display cells total, matching
//                     TS's glyph+NBSP pair.  Use inside hbox layout so the
//                     trailing cell doesn't get squashed by flex.
inline constexpr std::string_view kPointer       = "\xE2\x9D\xAF";           // ❯ U+276F (1 display cell)
inline constexpr std::string_view kPointerPrefix = "\xE2\x9D\xAF ";          // ❯ + space (2 cells)

// TS: PromptInputModeIndicator `bash` case: literal '!' + NBSP.
// `inputModes.ts` also exports `isInputModeCharacter('!') === true`.
inline constexpr std::string_view kBashGlyph       = "!";                     // ASCII '!' U+0021 (1 cell)
inline constexpr std::string_view kBashPrefix      = "! ";                     // ! + space (2 cells)
inline constexpr char           kBashModeChar      = '!';                     // for leading-char detection: `text[0] == kBashModeChar`

// TS: mode = 'orphaned-permission' / 'task-notification' → fall through to
// kPointer glyph — no distinct visual.  The CPP InputMode enum used to have
// extra values SlashCommand/HistorySearch/PlanMode/VimNormal/VimVisual —
// these are ORTHOGONAL to the prompt prefix and handled elsewhere (slash
// routing, Ctrl+R overlay, vim status bar).  From this commit on, the
// prompt glyph only has TWO rendered variants: kPointer vs kBashGlyph.
//
// The CPP-only "extra" indicator glyphs that don't exist in TS are retained
// below as kExtra* so call sites that still reference them (vim status bar,
// plan badge) continue to compile, but they are never used as the prompt
// prefix — only as badge / status-row content.

// Plan badge glyph (CPP extension, not a prompt prefix). U+25A3 = "▣".
inline constexpr std::string_view kExtraPlanGlyph     = "\xE2\x96\xA3";
// Vim reverse pointer (CPP extension). U+276E = "❮".
inline constexpr std::string_view kExtraVimReverse    = "\xE2\x9D\xAE";
// Task-notification star (CPP extension). U+002A = "*".
inline constexpr std::string_view kExtraTaskStar       = "*";
// Orphaned-permission bang (CPP extension — visually overlaps kBashGlyph
// but semantically different for routing). Kept identical so we have a
// grep-able symbol name.
inline constexpr std::string_view kExtraPermissionBang = "!";

// ─── npm::figures defaults (common glyphs, ported verbatim) ────────────
inline constexpr std::string_view kTick        = "\xE2\x9C\x94";  // ✔ U+2714 (figures.tick / TS success)
inline constexpr std::string_view kCross       = "\xE2\x9C\x98";  // ✘ U+2718 (figures.cross / TS error)
inline constexpr std::string_view kPointerSmall= "\xE2\x80\xBA";  // › U+203A (figures.pointerSmall)
inline constexpr std::string_view kWarning     = "\xE2\x9A\xA0";  // ⚠ U+26A0 (figures.warning)
inline constexpr std::string_view kInfo        = "\xE2\x84\xB9";  // ℹ U+2139 (figures.info)
inline constexpr std::string_view kCircle      = "\xE2\x97\xAF";  // ◯ U+25EF (figures.circle)
inline constexpr std::string_view kEllipsis    = "\xE2\x80\xA6";  // … U+2026 (figures.ellipsis — THREE DOTS leader)
inline constexpr std::string_view kCheckboxOn  = "\xE2\x98\x92";  // ☒ U+2612 (figures.checkboxOn)
inline constexpr std::string_view kCheckboxOff = "\xE2\x98\x90";  // ☐ U+2610 (figures.checkboxOff)
inline constexpr std::string_view kHamburger   = "\xE2\x98\xB0";  // ☰ U+2630 (figures.hamburger — three bars)
inline constexpr std::string_view kBullet      = "\xE2\x97\x8F";  // ● U+25CF (figures.bullet)
inline constexpr std::string_view kArrowDown   = "\xE2\x86\x93";  // ↓ U+2193 (figures.arrowDown — new-messages pill caret)
// TS REF: src/utils/figures.ts — additional npm-figures glyphs used
// throughout the UI for status indicators (GAP 5: fig-npm-glyphs-no-cpp-module).
inline constexpr std::string_view kSquare      = "\xE2\x96\xA0";  // ■ U+25A0 (figures.square — solid square)
inline constexpr std::string_view kDiamond     = "\xE2\x97\x86";  // ◆ U+25C6 (figures.diamond — filled diamond, alias kLozenge)
inline constexpr std::string_view kLozenge     = "\xE2\x97\x86";  // ◆ U+25C6 (alias for kDiamond)
inline constexpr std::string_view kArrowRight  = "\xE2\x86\x92";  // → U+2192 (figures.arrowRight — cross-session injected indicator)

// ─── Additional npm::figures glyphs (used in MCP dialogs, settings, task lists) ─
// TS REF: node_modules/figures/index.js — mainSymbols set (isUnicodeSupported).
// These were missing from the CPP port; used by MCP server list (radio buttons),
// settings carousel (triangles), task list (squares), companion UI (heart),
// issue banner (star), and tree-drawing connectors.
inline constexpr std::string_view kArrowUp             = "\xE2\x86\x91";  // ↑ U+2191 (figures.arrowUp — opus 1m merge notice)
inline constexpr std::string_view kArrowLeft           = "\xE2\x86\x90";  // ← U+2190 (figures.arrowLeft — inbound channel indicator)
inline constexpr std::string_view kRadioOn             = "\xE2\x97\x89";  // ◉ U+25C9 (figures.radioOn — selected radio button, alias kCircleFilled)
inline constexpr std::string_view kRadioOff            = "\xE2\x97\xAF";  // ◯ U+25EF (figures.radioOff — unselected radio, alias kCircle)
inline constexpr std::string_view kTriangleUpOutline   = "\xE2\x96\xB3";  // △ U+25B3 (figures.triangleUpOutline — warning triangle)
inline constexpr std::string_view kTriangleRightSmall  = "\xE2\x96\xB8";  // ▸ U+25B8 (figures.triangleRightSmall — expand caret)
inline constexpr std::string_view kTriangleDownSmall   = "\xE2\x96\xBE";  // ▾ U+25BE (figures.triangleDownSmall — collapse caret)
inline constexpr std::string_view kStar                = "\xE2\x98\x85";  // ★ U+2605 (figures.star — favorite / diagnostic hint)
inline constexpr std::string_view kHeart               = "\xE2\x99\xA5";  // ♥ U+2665 (figures.heart — companion pet)
inline constexpr std::string_view kSquareSmall         = "\xE2\x97\xBB";  // ◻ U+25FB (figures.squareSmall — task todo)
inline constexpr std::string_view kSquareSmallFilled   = "\xE2\x97\xBC";  // ◼ U+25FC (figures.squareSmallFilled — task done)
inline constexpr std::string_view kCircleFilled        = "\xE2\x97\x89";  // ◉ U+25C9 (figures.circleFilled — checked circle, alias kRadioOn)
inline constexpr std::string_view kCircleDouble        = "\xE2\x97\x8E";  // ◎ U+25CE (figures.circleDouble — remote indicator)
inline constexpr std::string_view kPlay                = "\xE2\x96\xB6";  // ▶ U+25B6 (figures.play — play icon)
inline constexpr std::string_view kQuestionMarkPrefix  = "(?)";             // (?) (figures.questionMarkPrefix — awaiting approval)

// Tree-drawing chars (TS REF: figures.lineVertical / lineUpRight / lineUpDownRight).
// Used by treeify.cppm and any hierarchical list rendering.  The "── " / "   "
// suffixes are ASCII and appended by callers; only the box-drawing glyph itself
// lives here so the byte sequence is defined once.
inline constexpr std::string_view kLineVertical        = "\xE2\x94\x82";  // │ U+2502 (figures.lineVertical — tree vertical connector)
inline constexpr std::string_view kLineUpRight         = "\xE2\x94\x94";  // └ U+2514 (figures.lineUpRight — tree last-child connector)
inline constexpr std::string_view kLineUpDownRight     = "\xE2\x94\x9C";  // ├ U+251C (figures.lineUpDownRight — tree mid-child connector)

// ─── CC-local figures (src/constants/figures.ts port) ───────────────────
// TS REF: src/constants/figures.ts (46 lines).  Platform-aware + app-specific
// glyphs that don't come from npm::figures.  Every scattered local definition
// of these across message_components.cppm / system_text_message.cppm must be
// replaced with imports from this module (single source of truth).

// BLACK_CIRCLE: platform-aware.  Darwin = ⏺ U+23FA, others = ● U+25CF.
// We expose BOTH so callers can choose; kBlackCircle is the Darwin default
// (cc-repl targets macOS), kBlackCircleFallback is the non-Darwin ●.
// TS REF: constants/figures.ts L4 — `env.platform === 'darwin' ? '⏺' : '●'`
inline constexpr std::string_view kBlackCircle         = "\xE2\x8F\xBA";  // ⏺ U+23FA (Darwin — message row prefix, system event dot)
inline constexpr std::string_view kBlackCircleFallback = "\xE2\x97\x8F";  // ● U+25CF (non-Darwin, same as kBullet)

inline constexpr std::string_view kDiamondOpen         = "\xE2\x97\x87";  // ◇ U+25C7 (DIAMOND_OPEN — ultraplan running)
inline constexpr std::string_view kTeardropAsterisk    = "\xE2\x9C\xBB";  // ✻ U+273B (TEARDROP_ASTERISK — scheduled_task / permission_retry)
inline constexpr std::string_view kLightningBolt       = "\xE2\x86\xAF";  // ↯ U+21AF (LIGHTNING_BOLT — fast mode indicator)
inline constexpr std::string_view kPauseIcon           = "\xE2\x8F\xB8";  // ⏸ U+23F8 (PAUSE_ICON — paused state)
inline constexpr std::string_view kBulletOperator      = "\xE2\x88\x99";  // ∙ U+2219 (BULLET_OPERATOR — tool error prefix)
inline constexpr std::string_view kRefreshArrow        = "\xE2\x86\xBB";  // ↻ U+21BB (REFRESH_ARROW — resource update indicator)
inline constexpr std::string_view kChannelArrow        = "\xE2\x86\x90";  // ← U+2190 (CHANNEL_ARROW — inbound channel, alias kArrowLeft)
inline constexpr std::string_view kInjectedArrow       = "\xE2\x86\x92";  // → U+2192 (INJECTED_ARROW — cross-session, alias kArrowRight)
inline constexpr std::string_view kReferenceMark       = "\xE2\x80\xBB";  // ※ U+203B (REFERENCE_MARK — komejirushi, away-summary recap marker)
inline constexpr std::string_view kBlockquoteBar       = "\xE2\x96\x8E";  // ▎ U+258E (BLOCKQUOTE_BAR — left one-quarter block)
inline constexpr std::string_view kHeavyHorizontal     = "\xE2\x94\x81";  // ━ U+2501 (HEAVY_HORIZONTAL — heavy box-drawing separator)
inline constexpr std::string_view kFlagIcon            = "\xE2\x9A\x91";  // ⚑ U+2691 (FLAG_ICON — issue banner)
inline constexpr std::string_view kForkGlyph           = "\xE2\x91\x82";  // ⑂ U+2442 (FORK_GLYPH — fork directive indicator)

// Effort level indicators (TS REF: constants/figures.ts L10-13).
inline constexpr std::string_view kEffortLow           = "\xE2\x97\x8B";  // ○ U+25CB (EFFORT_LOW)
inline constexpr std::string_view kEffortMedium        = "\xE2\x97\x90";  // ◐ U+25D0 (EFFORT_MEDIUM)
inline constexpr std::string_view kEffortHigh          = "\xE2\x97\x8F";  // ● U+25CF (EFFORT_HIGH, alias kBullet/kBlackCircleFallback)
inline constexpr std::string_view kEffortMax           = "\xE2\x97\x89";  // ◉ U+25C9 (EFFORT_MAX — Opus 4.6 only, alias kRadioOn)

// Bridge status indicators (TS REF: constants/figures.ts L38-45).
// TS BRIDGE_SPINNER_FRAMES = ['·|·', '·/·', '·—·', '·\\·'] (4 frames).
// Distinct from the 10-frame braille kSpinnerFrames used for general loading
// spinners — bridge/MCP connection indicators use this compact middot+line set.
inline constexpr std::array<std::string_view, 4> kBridgeSpinnerFrames = {{
    "\xC2\xB7\x7C\xC2\xB7",          // ·|· frame 0 (middot + pipe + middot)
    "\xC2\xB7\x2F\xC2\xB7",          // ·/· frame 1 (middot + slash + middot)
    "\xC2\xB7\xE2\x80\x94\xC2\xB7",  // ·—· frame 2 (middot + U+2014 em dash + middot)
    "\xC2\xB7\x5C\xC2\xB7",          // ·\· frame 3 (middot + backslash + middot)
}};

/// Bridge spinner frame count (4).  Distinct from kSpinnerFrameCount (10).
inline constexpr std::size_t kBridgeSpinnerFrameCount = kBridgeSpinnerFrames.size();

/// Get the bridge spinner glyph for a given frame index (wraps modulo count).
[[nodiscard]] inline std::string_view bridge_spinner_frame_glyph(int frame) noexcept {
    const auto idx = static_cast<std::size_t>(
        ((frame % static_cast<int>(kBridgeSpinnerFrameCount)) +
         static_cast<int>(kBridgeSpinnerFrameCount)) %
        static_cast<int>(kBridgeSpinnerFrameCount));
    return kBridgeSpinnerFrames[idx];
}

// TS BRIDGE_READY_INDICATOR = '·✔︎·' = '·✔︎·'
//   (middot U+00B7 + heavy check U+2714 + VS15 U+FE0E + middot U+00B7).
// NOTE: previous CPP value was wrong — emoji ✅︎ (U+2705 + VS15).  Corrected
// below to match TS exactly.  kBridgeReadyIndicatorLegacy kept for one release
// so callers that already imported the old symbol don't break.
inline constexpr std::string_view kBridgeReadyIndicator = "\xC2\xB7\xE2\x9C\x94\xEF\xB8\x8E\xC2\xB7";  // ·✔︎· (TS-faithful)
inline constexpr std::string_view kBridgeReadyIndicatorLegacy = "\xE2\x9C\x85\xEF\xB8\x8F";  // ✅︎ (old emoji, deprecated)

// TS BRIDGE_FAILED_INDICATOR = '×' = '×' U+00D7.
inline constexpr std::string_view kBridgeFailedIndicator = "\xC3\x97";  // × U+00D7

// ─── Message connector / separator glyphs ──────────────────────────────
// TS components/messages/Connector.tsx: ⌐ U+2310 "REVERSED NOT SIGN"
//     Rendered vertically aligned between tool-use row and tool-result row.
inline constexpr std::string_view kConnector = "\xE2\x8C\x90";    // ⌐ (1 cell)

// ─── Spinner frames ────────────────────────────────────────────────────
// TS src/utils/figures.ts: BRIDGE_SPINNER_FRAMES (8 braille dots sweeping
// from top-left to bottom-right).  Used for:
//   - Bridge + MCP connection-establishment indicator
//   - cc::ui::components::Spinner (the compact teardrop braille animator)
//
// WARNING: tool_use_loader / thinking_message both had truncated 8-frame
// variants dropping frames [7,8] ('⠇⠏').  Standardize on the 10-frame
// TS SpinnerGlyph sequence below for EVERY in-app spinner.
inline constexpr std::array<std::string_view, 10> kSpinnerFramesBraille = {{
    "\xE2\xA0\x8B",  // ⠋ frame 0 (U+280B)
    "\xE2\xA0\x99",  // ⠙ frame 1 (U+2819)
    "\xE2\xA0\xB9",  // ⠹ frame 2 (U+2839)
    "\xE2\xA0\xB8",  // ⠸ frame 3 (U+2838)
    "\xE2\xA0\xBC",  // ⠼ frame 4 (U+283C)
    "\xE2\xA0\xB4",  // ⠴ frame 5 (U+2834)
    "\xE2\xA0\xA6",  // ⠦ frame 6 (U+2826)
    "\xE2\xA0\xA7",  // ⠧ frame 7 (U+2826 — previously dropped by thinking_message)
    "\xE2\xA0\x87",  // ⠇ frame 8 (U+2807 — previously dropped by tool_use_loader)
    "\xE2\xA0\x8F",  // ⠏ frame 9 (U+280F)
}};

/// Canonical spinner frame set — 10 braille frames matching TS exactly.
/// TS REF: src/components/Spinner/SpinnerGlyph.tsx — DEFAULT_CHARACTERS
///   (['·','✢','✳','✶','✻','✽']) forward + reversed = 12 frames for the
///   asterisk spinner; the braille set below is the CPP unified spinner
///   used by thinking_message, tool_use_loader, progress_bar, and
///   messages_list (GAP 4: fig-spinner-frame-inconsistency).
///
/// All modules MUST import this instead of defining local frame arrays,
/// so that the animation speed and visual style are consistent everywhere.
inline constexpr auto& kSpinnerFrames = kSpinnerFramesBraille;
inline constexpr std::size_t kSpinnerFrameCount = kSpinnerFramesBraille.size();

/// Get the spinner glyph for a given frame index (wraps modulo frame count).
/// TS REF: SpinnerGlyph.tsx L49 — SPINNER_FRAMES[frame % SPINNER_FRAMES.length]
[[nodiscard]] inline std::string_view spinner_frame_glyph(int frame) noexcept {
    const auto idx = static_cast<std::size_t>(
        ((frame % static_cast<int>(kSpinnerFrameCount)) +
         static_cast<int>(kSpinnerFrameCount)) %
        static_cast<int>(kSpinnerFrameCount));
    return kSpinnerFrames[idx];
}

// ─── Prompt-input mode helpers (inputModes.ts port) ────────────────────
// TS equivalent: type PromptInputMode = 'bash' | 'prompt' |
//                                     'orphaned-permission' | 'task-notification'
//
// We keep the enum narrow; PlanMode / Vim state / Slash mode are orthogonal
// and handled as layered badges (not as a prefix-glyph switch).  CPP-only
// extra values from the old ReplScreenState::InputMode are preserved under
// a separate VimMode enum / PlanMode flag so the prefix logic is clean.
enum class PromptMode : int {
    kPrompt              = 0,
    kBash                = 1,
    kOrphanedPermission  = 2,
    kTaskNotification    = 3,
};

/// TS: `getModeFromInput(input)` -> detect bash mode from leading '!'.
[[nodiscard]] inline PromptMode get_mode_from_input(std::string_view input) noexcept {
    if (!input.empty() && input.front() == kBashModeChar) {
        return PromptMode::kBash;
    }
    return PromptMode::kPrompt;
}

/// TS: `getValueFromInput(input)` -> strip 1 leading char when bash mode.
/// IMPORTANT: The '!' is a transient mode trigger.  Per TS PromptInput.tsx
/// lines 869-901, once the mode is detected the character is NEVER stored
/// in the input state.  This function is used (a) when pasting "!cmd" into
/// an empty input (multi-char insertion) and (b) when rendering history
/// entries that were saved WITH the prefix intact.
[[nodiscard]] inline std::string_view strip_mode_prefix(std::string_view input) noexcept {
    const auto mode = get_mode_from_input(input);
    if (mode == PromptMode::kPrompt) return input;
    if (input.size() <= 1) return {};
    return input.substr(1);
}

/// TS: `prependModeCharacterToInput(input, mode)` -> only bash prepends '!'.
/// Used when PUSHING a value back into history storage (history persists
/// the raw mode-aware string so it can be round-tripped).
[[nodiscard]] inline std::string prepend_mode_char(std::string_view input, PromptMode mode) {
    if (mode == PromptMode::kBash) {
        std::string out;
        out.reserve(input.size() + 1);
        out.push_back(kBashModeChar);
        out.append(input);
        return out;
    }
    return std::string(input);
}

/// TS: `isInputModeCharacter(c)` -> true iff c == '!'.  Used to catch the
/// single-char '!' insertion at cursor-offset==0 so it can be swallowed
/// as a mode-transition instead of being inserted into the buffer.
[[nodiscard]] inline bool is_mode_character(std::string_view c) noexcept {
    return c.size() == 1 && c[0] == kBashModeChar;
}

// ─── Platform fallback for BLACK_CIRCLE ────────────────────────────────
// TS node_modules/figures: figures.circleBlack fallback handling.
// On macOS / Linux we use the Unicode glyph; on Windows we fall back to '*'.
// This build targets Darwin (cc-repl); Windows support is gated behind a
// build-time macro; callers that want the fallback behaviour should branch
// on `#ifdef _WIN32` themselves.
inline constexpr std::string_view kCircleBlack = "\xE2\x97\x8F";  // ● U+25CF (same as kBullet — alias for grep clarity)

// ─── Unicode support detection (TS REF: is-unicode-supported/index.js) ──
// TS REF: node_modules/is-unicode-supported/index.js L1-25.
// Determines whether the terminal supports Unicode glyphs. On non-Windows,
// returns true unless TERM == "linux" (kernel console lacks Unicode).
// On Windows, checks various terminal emulator environment variables.
[[nodiscard]] inline bool is_unicode_supported() noexcept {
#ifdef _WIN32
    if (const char* wt = std::getenv("WT_SESSION"); wt && *wt) return true;
    if (const char* terminus = std::getenv("TERMINUS_SUBLIME"); terminus && *terminus) return true;
    if (const char* conemu = std::getenv("ConEmuTask");
        conemu && std::string_view(conemu) == "{cmd::Cmder}") return true;
    if (const char* tp = std::getenv("TERM_PROGRAM"); tp) {
        const std::string_view tpv(tp);
        if (tpv == "Terminus-Sublime" || tpv == "vscode") return true;
    }
    if (const char* term = std::getenv("TERM"); term) {
        const std::string_view tv(term);
        if (tv == "xterm-256color" || tv == "alacritty" ||
            tv == "rxvt-unicode" || tv == "rxvt-unicode-256color") return true;
    }
    if (const char* te = std::getenv("TERMINAL_EMULATOR");
        te && std::string_view(te) == "JetBrains-JediTerm") return true;
    return false;
#else
    // Non-Windows: only the Linux kernel console (TERM=linux) lacks Unicode.
    if (const char* term = std::getenv("TERM");
        term && std::string_view(term) == "linux") return false;
    return true;
#endif
}

// ─── Fallback special symbols (TS REF: figures/index.js L237-272) ───────
// TS REF: node_modules/figures/index.js specialFallbackSymbols.
// ASCII/limited-Unicode alternatives used when is_unicode_supported() is false.
// Only "special" glyphs differ; "common" glyphs (arrows, lines, bullet, etc.)
// are identical in both main and fallback sets.
namespace fallback {
    inline constexpr std::string_view kTick        = "\xE2\x88\x9A";  // √ U+221A SQUARE ROOT
    inline constexpr std::string_view kInfo        = "i";              // lowercase i
    inline constexpr std::string_view kWarning     = "\xE2\x80\xBC";  // ‼ U+203C DOUBLE EXCLAMATION
    inline constexpr std::string_view kCross       = "\xC3\x97";      // × U+00D7 MULTIPLICATION SIGN
    inline constexpr std::string_view kSquareSmall = "\xE2\x96\xA1";  // □ U+25A1 WHITE SQUARE
    inline constexpr std::string_view kSquareSmallFilled = "\xE2\x96\xA0";  // ■ U+25A0 BLACK SQUARE
    inline constexpr std::string_view kCircle      = "( )";            // ASCII ( )
    inline constexpr std::string_view kCircleFilled = "(*)";           // ASCII (*)
    inline constexpr std::string_view kCircleDotted = "( )";           // ASCII ( )
    inline constexpr std::string_view kCircleDouble = "( )";           // ASCII ( )
    inline constexpr std::string_view kCircleCircle = "\xE2\x97\x8B";  // ○ U+25CB (TS: '(○)')
    inline constexpr std::string_view kCircleCross = "\xC3\x97";      // × U+00D7 (TS: '(×)')
    inline constexpr std::string_view kCirclePipe  = "\xE2\x94\x82";  // │ U+2502 (TS: '(│)')
    inline constexpr std::string_view kRadioOn     = "(*)";           // alias kCircleFilled
    inline constexpr std::string_view kRadioOff    = "( )";           // alias kCircle
    inline constexpr std::string_view kCheckboxOn  = "[\xC3\x97]";    // [×] ASCII brackets + ×
    inline constexpr std::string_view kCheckboxOff = "[ ]";            // ASCII [ ]
    inline constexpr std::string_view kCheckboxCircleOn = "\xC3\x97"; // × (TS: '(×)')
    inline constexpr std::string_view kCheckboxCircleOff = "( )";     // ASCII ( )
    inline constexpr std::string_view kPointer     = ">";              // ASCII greater-than
    inline constexpr std::string_view kTriangleUpOutline = "\xE2\x88\x86";  // ∆ U+2206 INCREMENT
    inline constexpr std::string_view kTriangleLeft = "\xE2\x97\x84";  // ◄ U+25C4 BLACK LEFT-POINTING POINTER
    inline constexpr std::string_view kTriangleRight = "\xE2\x96\xBA"; // ► U+25BA BLACK RIGHT-POINTING POINTER
    inline constexpr std::string_view kLozenge     = "\xE2\x99\xA6";  // ♦ U+2666 BLACK DIAMOND SUIT
    inline constexpr std::string_view kLozengeOutline = "\xE2\x97\x8A"; // ◊ U+25CA LOZENGE
    inline constexpr std::string_view kHamburger   = "\xE2\x89\xA1";  // ≡ U+2261 IDENTICAL TO
    inline constexpr std::string_view kSmiley      = "\xE2\x98\xBA";  // ☺ U+263A WHITE SMILING FACE
    inline constexpr std::string_view kMustache    = "\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90";  // ┌─┐
    inline constexpr std::string_view kStar        = "\xE2\x9C\xB6";  // ✶ U+2736 SIX POINTED BLACK STAR
    inline constexpr std::string_view kPlay        = "\xE2\x96\xBA";  // ► U+25BA (alias kTriangleRight)
    inline constexpr std::string_view kNodejs      = "\xE2\x99\xA6";  // ♦ U+2666 (alias kLozenge)
    inline constexpr std::string_view kOneSeventh  = "1/7";            // ASCII fraction
    inline constexpr std::string_view kOneNinth    = "1/9";            // ASCII fraction
    inline constexpr std::string_view kOneTenth    = "1/10";           // ASCII fraction
}  // namespace fallback

// ─── Figures glyph set accessor (TS REF: figures/index.js L274-279) ────
// TS REF: node_modules/figures/index.js — `export default figures`.
// Provides a struct with all commonly-used glyphs, automatically selecting
// between main (Unicode) and fallback (ASCII) sets via is_unicode_supported().
//
// Usage:
//   const auto& fig = cc::ui::design::figures::figures();
//   render(fig.pointer);   // '❯' on Unicode terminals, '>' on fallback
//   render(fig.tick);      // '✔' on Unicode, '√' on fallback
//
// Individual kXxx constants (kPointer, kTick, etc.) always return the Unicode
// main-set values. Use figures() when you want automatic fallback selection.
struct Figures {
    std::string_view tick;
    std::string_view info;
    std::string_view warning;
    std::string_view cross;
    std::string_view pointer;
    std::string_view pointerSmall;
    std::string_view arrowUp;          // common — same in both sets
    std::string_view arrowDown;        // common — same in both sets
    std::string_view arrowLeft;        // common — same in both sets
    std::string_view arrowRight;       // common — same in both sets
    std::string_view bullet;           // common — same in both sets
    std::string_view ellipsis;         // common — same in both sets
    std::string_view square;           // common — same in both sets (U+25A0 solid)
    std::string_view squareSmall;
    std::string_view squareSmallFilled;
    std::string_view circle;
    std::string_view circleFilled;
    std::string_view circleDouble;
    std::string_view radioOn;
    std::string_view radioOff;
    std::string_view checkboxOn;
    std::string_view checkboxOff;
    std::string_view triangleUpOutline;
    std::string_view triangleDownSmall;  // common — same in both sets
    std::string_view triangleRightSmall; // common — same in both sets
    std::string_view star;
    std::string_view heart;            // common — same in both sets
    std::string_view play;
    std::string_view questionMarkPrefix; // common — "(?)"
    std::string_view lineVertical;     // common — tree drawing
    std::string_view lineUpRight;      // common — tree drawing
    std::string_view lineUpDownRight;  // common — tree drawing
};

/// TS: `export const mainSymbols = {...common, ...specialMainSymbols}` (L274).
/// Full Unicode glyph set — used when is_unicode_supported() == true.
inline constexpr Figures kMainFigures = {
    /*.tick           =*/ kTick,                    // ✔ U+2714
    /*.info           =*/ kInfo,                    // ℹ U+2139
    /*.warning        =*/ kWarning,                 // ⚠ U+26A0
    /*.cross          =*/ kCross,                   // ✘ U+2718
    /*.pointer        =*/ kPointer,                 // ❯ U+276F
    /*.pointerSmall   =*/ kPointerSmall,            // › U+203A (common)
    /*.arrowUp        =*/ kArrowUp,                 // ↑ U+2191 (common)
    /*.arrowDown      =*/ kArrowDown,               // ↓ U+2193 (common)
    /*.arrowLeft      =*/ kArrowLeft,               // ← U+2190 (common)
    /*.arrowRight     =*/ kArrowRight,              // → U+2192 (common)
    /*.bullet         =*/ kBullet,                  // ● U+25CF (common)
    /*.ellipsis       =*/ kEllipsis,                // … U+2026 (common)
    /*.square         =*/ kSquare,                  // ■ U+25A0 (common)
    /*.squareSmall    =*/ kSquareSmall,             // ◻ U+25FB
    /*.squareSmallFilled =*/ kSquareSmallFilled,    // ◼ U+25FC
    /*.circle         =*/ kCircle,                  // ◯ U+25EF
    /*.circleFilled   =*/ kCircleFilled,            // ◉ U+25C9
    /*.circleDouble   =*/ kCircleDouble,            // ◎ U+25CE
    /*.radioOn        =*/ kRadioOn,                 // ◉ U+25C9 (alias kCircleFilled)
    /*.radioOff       =*/ kRadioOff,                // ◯ U+25EF (alias kCircle)
    /*.checkboxOn     =*/ kCheckboxOn,              // ☒ U+2612
    /*.checkboxOff    =*/ kCheckboxOff,             // ☐ U+2610
    /*.triangleUpOutline =*/ kTriangleUpOutline,    // △ U+25B3
    /*.triangleDownSmall =*/ kTriangleDownSmall,    // ▾ U+25BE (common)
    /*.triangleRightSmall =*/ kTriangleRightSmall,  // ▸ U+25B8 (common)
    /*.star           =*/ kStar,                    // ★ U+2605
    /*.heart          =*/ kHeart,                   // ♥ U+2665 (common)
    /*.play           =*/ kPlay,                    // ▶ U+25B6
    /*.questionMarkPrefix =*/ kQuestionMarkPrefix,  // "(?)" (common)
    /*.lineVertical   =*/ kLineVertical,            // │ U+2502 (common)
    /*.lineUpRight    =*/ kLineUpRight,             // └ U+2514 (common)
    /*.lineUpDownRight =*/ kLineUpDownRight,        // ├ U+251C (common)
};

/// TS: `export const fallbackSymbols = {...common, ...specialFallbackSymbols}` (L275).
/// Fallback glyph set — used when is_unicode_supported() == false.
/// Common glyphs (arrows, lines, bullet, ellipsis, etc.) are identical to main.
inline constexpr Figures kFallbackFigures = {
    /*.tick           =*/ fallback::kTick,                 // √
    /*.info           =*/ fallback::kInfo,                 // i
    /*.warning        =*/ fallback::kWarning,              // ‼
    /*.cross          =*/ fallback::kCross,                // ×
    /*.pointer        =*/ fallback::kPointer,              // >
    /*.pointerSmall   =*/ kPointerSmall,                   // › (common)
    /*.arrowUp        =*/ kArrowUp,                       // ↑ (common)
    /*.arrowDown      =*/ kArrowDown,                     // ↓ (common)
    /*.arrowLeft      =*/ kArrowLeft,                     // ← (common)
    /*.arrowRight     =*/ kArrowRight,                    // → (common)
    /*.bullet         =*/ kBullet,                        // ● (common)
    /*.ellipsis       =*/ kEllipsis,                      // … (common)
    /*.square         =*/ kSquare,                        // ■ (common)
    /*.squareSmall    =*/ fallback::kSquareSmall,         // □
    /*.squareSmallFilled =*/ fallback::kSquareSmallFilled,// ■
    /*.circle         =*/ fallback::kCircle,              // "( )"
    /*.circleFilled   =*/ fallback::kCircleFilled,        // "(*)"
    /*.circleDouble   =*/ fallback::kCircleDouble,        // "( )"
    /*.radioOn        =*/ fallback::kRadioOn,             // "(*)"
    /*.radioOff       =*/ fallback::kRadioOff,            // "( )"
    /*.checkboxOn     =*/ fallback::kCheckboxOn,          // "[×]"
    /*.checkboxOff    =*/ fallback::kCheckboxOff,         // "[ ]"
    /*.triangleUpOutline =*/ fallback::kTriangleUpOutline,// ∆
    /*.triangleDownSmall =*/ kTriangleDownSmall,          // ▾ (common)
    /*.triangleRightSmall =*/ kTriangleRightSmall,        // ▸ (common)
    /*.star           =*/ fallback::kStar,                // ✶
    /*.heart          =*/ kHeart,                         // ♥ (common)
    /*.play           =*/ fallback::kPlay,                // ►
    /*.questionMarkPrefix =*/ kQuestionMarkPrefix,        // "(?)" (common)
    /*.lineVertical   =*/ kLineVertical,                  // │ (common)
    /*.lineUpRight    =*/ kLineUpRight,                   // └ (common)
    /*.lineUpDownRight =*/ kLineUpDownRight,              // ├ (common)
};

/// TS: `const figures = shouldUseMain ? mainSymbols : fallbackSymbols;`
/// `export default figures;` (L278-279).
///
/// Returns a const reference to the active glyph set, determined once per
/// process by is_unicode_supported(). Individual kXxx constants always return
/// Unicode glyphs; use this when you want automatic fallback to ASCII on
/// terminals that don't support Unicode.
[[nodiscard]] inline const Figures& figures() noexcept {
    static const bool use_main = is_unicode_supported();
    return use_main ? kMainFigures : kFallbackFigures;
}

// ─── replace_symbols (TS REF: figures/index.js L281-292) ───────────────
// TS REF: node_modules/figures/index.js `export const replaceSymbols`.
// Replaces main special symbols with their fallback equivalents in a string.
// By default only replaces when !is_unicode_supported(); pass use_fallback=true
// to force replacement regardless of terminal capability.
//
// TS: for (const [key, mainSymbol] of Object.entries(specialMainSymbols))
//       string = string.replaceAll(mainSymbol, fallbackSymbols[key]);
namespace detail {
    /// Replace all occurrences of `from` with `to` in string `s`.
    inline void replace_all(std::string& s, std::string_view from,
                            std::string_view to) noexcept {
        if (from.empty()) return;
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
    }
}  // namespace detail

/// TS REF: replaceSymbols(string, {useFallback = !shouldUseMain}) (L284-292).
/// Iterates over all specialMainSymbols entries and replaces each with its
/// fallbackSymbols counterpart in the input string.
[[nodiscard]] inline std::string replace_symbols(
    std::string_view input,
    bool use_fallback = !is_unicode_supported()) {
    if (!use_fallback) return std::string(input);
    std::string result(input);
    // Only "special" symbols have fallbacks; common glyphs are identical.
    detail::replace_all(result, kTick,                fallback::kTick);
    detail::replace_all(result, kInfo,                fallback::kInfo);
    detail::replace_all(result, kWarning,             fallback::kWarning);
    detail::replace_all(result, kCross,               fallback::kCross);
    detail::replace_all(result, kSquareSmall,         fallback::kSquareSmall);
    detail::replace_all(result, kSquareSmallFilled,   fallback::kSquareSmallFilled);
    detail::replace_all(result, kCircle,              fallback::kCircle);
    detail::replace_all(result, kCircleFilled,        fallback::kCircleFilled);
    detail::replace_all(result, kCircleDouble,        fallback::kCircleDouble);
    detail::replace_all(result, kRadioOn,             fallback::kRadioOn);
    detail::replace_all(result, kRadioOff,            fallback::kRadioOff);
    detail::replace_all(result, kCheckboxOn,          fallback::kCheckboxOn);
    detail::replace_all(result, kCheckboxOff,         fallback::kCheckboxOff);
    detail::replace_all(result, kPointer,             fallback::kPointer);
    detail::replace_all(result, kTriangleUpOutline,   fallback::kTriangleUpOutline);
    detail::replace_all(result, kStar,                fallback::kStar);
    detail::replace_all(result, kPlay,                fallback::kPlay);
    return result;
}

}  // namespace cc::ui::design::figures
