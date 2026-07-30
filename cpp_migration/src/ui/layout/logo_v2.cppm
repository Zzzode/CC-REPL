// ==========================================================================
// Faithful C++23 Named Module port of:
//   TS src/components/LogoV2/LogoV2.tsx           (543 lines)
//   TS src/components/LogoV2/CondensedLogo.tsx    (161 lines)
//   TS src/components/LogoV2/WelcomeV2.tsx        (433 lines)
//   TS src/utils/statusNoticeDefinitions.tsx      (198 lines)
//   TS src/components/LogoV2/Opus1mMergeNotice.tsx
//   TS src/components/LogoV2/VoiceModeNotice.tsx
//   TS src/components/LogoV2/ChannelsNotice.tsx   (feature-gated KAIROS/KAIROS_CHANNELS)
//   TS src/components/LogoV2/EmergencyTip.tsx
//   TS src/components/LogoV2/GuestPassesUpsell.tsx
//   TS src/components/LogoV2/OverageCreditUpsell.tsx
//
// Scope shrink (TS audit confirmed — items below DO NOT EXIST in upstream TS):
//   ✘ starfield / animated starfield background
//   ✘ 12-item rotating carousel of notice messages
//   ✘ per-frame colour cycling of static WelcomeV2 background chars
//   ✘ "CC-REPL" branding (TS uses "Claude Code" exclusively)
//
// See memory [[ui-port-ts-is-the-standard]] — TS is the canonical reference.
// ==========================================================================

module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.logo_v2;

import cc.ui.logo;  // LogoDisplayData + RenderCondensedLogoElement +
                     // RenderOpus1MNotice + RenderBrandChip helpers

export namespace cc::ui::logo_v2 {

using ftxui::bold;
using ftxui::bgcolor;
using ftxui::color;
using ftxui::dim;
using ftxui::Element;
using ftxui::Elements;
using ftxui::hbox;
using ftxui::size;
using ftxui::text;
using ftxui::vbox;
using ftxui::HEIGHT;
using ftxui::WIDTH;
using ftxui::EQUAL;
using ftxui::GREATER_THAN;
using ftxui::Color;

// --------------------------------------------------------------------
// §1  Layout-mode enum + column thresholds
// --------------------------------------------------------------------
// TS getLayoutMode(columns):
//   columns < 70 → "compact"   (single-column rounded border-card, no feed)
//   columns >= 70 → "horizontal"  (rounded border-card, LEFT=meta+clawd,
//                                   RIGHT=FeedColumn, vertical divider)
// "condensed" mode is NOT a layoutMode in TS; it is an early return
// path gated by LogoV2's isCondensedMode (no changelog/onboarding/force).
// We still expose it in the enum for callers that want to short-circuit.
enum class LogoLayoutMode {
  Condensed,    // early return: CondensedLogo + notices, no outer card
  Compact,      // columns < 70, rounded card, centered, no FeedColumn
  Horizontal,   // columns >= 70, rounded card, left panel + divider + feed
};

// TS thresholds (from logoV2Utils.getLayoutMode).
inline constexpr int kHorizontalMinCols = 70;
inline constexpr int kWelcomeV2FixedWidth = 58;  // TS WELCOME_V2_WIDTH

// Decide layout mode purely from terminal width.
[[nodiscard]] constexpr auto layout_mode_from_cols(int cols) noexcept
    -> LogoLayoutMode {
  return cols >= kHorizontalMinCols ? LogoLayoutMode::Horizontal
                                    : LogoLayoutMode::Compact;
}

// --------------------------------------------------------------------
// §2  Extended LogoDisplayData (superset of cc::ui::logo::LogoDisplayData)
// --------------------------------------------------------------------
// TS LogoV2.tsx L159-173 reads: version / cwd / billingType / agentName
//   / effortSuffix / modelDisplayName / username / orgName / companyAnnouncement
//   / hasReleaseNotes / showOnboarding / showSandboxStatus
//   / debugMode / tmux session / sandboxing / showGuestPasses / showOverageCredit
//   / numStartups / globalConfig fields
//
// We do NOT reach out to globalConfig from this module (separation of
// concerns — no IO inside UI rendering). The ReplScreen / caller is
// expected to populate these fields from their config source; default
// values produce the safe "minimal condensed" look with no side-effects.
struct LogoV2Options {
  // --- Identity / chrome (same as cc::ui::logo::LogoDisplayData) ---
  std::string version;
  std::string cwd;
  std::string billing_type;
  std::optional<std::string> agent_name;
  std::string model_display_name;

  // --- Extra fields used by Compact/Horizontal & notice stack ---
  std::optional<std::string> username;    // Welcome greeting: "Welcome back X!"
  std::optional<std::string> org_name;    // shown in Horizontal model-line

  // --- Feature gating ---
  bool is_condensed_mode = true;          // TS: no onboarding & no release notes
  bool show_onboarding = false;           // TS REF: LogoV2.tsx L56 shouldShowProjectOnboarding()
  bool show_sandbox_status = false;       // SandboxManager.isSandboxingEnabled
  bool show_guest_passes = false;         // GuestPassesUpsell visible
  bool show_overage_credit = false;       // OverageCreditUpsell visible
  bool is_debug_mode = false;             // debug mode enabled warning
  bool debug_log_to_stderr = false;       // true → stderr, false → path
  std::string debug_log_path;             // path only meaningful if !stderr

  // --- Environment notices ---
  std::optional<std::string> tmux_session;   // CLAUDE_CODE_TMUX_SESSION
  std::optional<std::string> tmux_prefix;    // CLAUDE_CODE_TMUX_PREFIX
  bool tmux_prefix_conflicts = false;        // show "press twice" variant
  std::optional<std::string> company_announcement;  // org-wide banner
  std::optional<std::string> emergency_tip;         // EmergencyTip override

  // --- StatusNotices (exactly 6 TS definitions) ---
  // Each field is a vector of pre-rendered notice rows; empty means
  // "not active".  Caller is responsible for computing active notices
  // (we deliberately keep the render side IO-free).
  struct NoticeRow {
    std::string glyph;          // U+26A0 ⚠ for warning, U+2191 ↑ for info
    std::string body;           // main text (may include "· sub-clause")
    bool is_warning = true;     // warning→warning color; info→ide/default
  };
  std::vector<NoticeRow> status_notices;
};

// --------------------------------------------------------------------
// §3  Individual notice renderers
// --------------------------------------------------------------------
// All renderers return text("") when not applicable (so a flat vbox over
// all of them produces the correct sparse layout with no extra
// padding for inactive notices).  TS uses <Box paddingLeft={2}> with
// a flexDirection column; we approximate with a 2-column leading space.

namespace detail {

// Exact hex matches from TS src/utils/theme.ts darkTheme tokens
inline const Color kWarningColor(255, 193,   7);   // theme.warning #FFC107
inline const Color kIdeColor    ( 71, 130, 200);   // theme.ide     #4782C8
inline const Color kClaude      (215, 119,  87);   // theme.claude / clawd_body #D77757
inline const Color kClawdBody   (215, 119,  87);   // theme.clawd_body (== claude in dark)
inline const Color kClawdBackground(0, 0, 0);      // theme.clawd_background #000000 (TS L162)
inline const Color kMuted       (153, 153, 153);   // theme.inactive / dimColor #999999

// Padding-left 2 columns (TS <Box paddingLeft={2}>).
[[nodiscard]] inline auto pad2(Element e) -> Element {
  return hbox({ text("  "), std::move(e) });
}

} // namespace detail

// --- 3a. VoiceModeNotice — always shown in current TS (static text).
//     TS VoiceModeNotice.tsx renders as:
//       <Box paddingLeft={2} flexDirection="column">
//         <Text dimColor>
//           ✻ voiceMode.active ? "Voice mode active" : "Voice mode enabled"
//             · Press <Text bold>⌘⇧.</Text> to toggle
//         </Text>
//       </Box>
//     We render the "enabled" variant (default); caller may override.
[[nodiscard]] inline auto RenderVoiceModeNotice(bool active = false) -> Element {
  using namespace detail;
  return pad2(hbox({
    text("\xE2\x9C\xBB ") | color(kClaude) | bold,    // ✻ U+273B
    text(active ? "Voice mode active" : "Voice mode enabled"),
    text(" \xC2\xB7 "),
    text("Press "),
    text("\xE2\x8C\x98\xE2\x87\xA7.") | bold,        // ⌘⇧.
    text(" to toggle"),
  }) | dim | color(kMuted));
}

// --- 3b. Opus 1M notice — delegated to cc::ui::logo::RenderOpus1MNotice
//     (already a faithful TS port).  Re-exported here for the flat stack.
[[nodiscard]] inline auto RenderOpus1MNotice() -> Element {
  return cc::ui::logo::RenderOpus1MNotice();
}

// --- 3c. ChannelsNotice — TS feature() gating (KAIROS || KAIROS_CHANNELS).
//     C++ counterpart is a constexpr bool; when false, returns empty Element.
template <bool KAIROS = false, bool KAIROS_CHANNELS = false>
[[nodiscard]] inline auto RenderChannelsNotice() -> Element {
  if constexpr (!(KAIROS || KAIROS_CHANNELS)) {
    return Element();  // 0-height: FTXUI default-constructed Element has no renderer
  } else {
    using namespace detail;
    return pad2(hbox({
      text("\xE2\x9A\xA1 ") | color(kClaude) | bold,   // ⚡ U+26A1
      text("Channels beta: join live conversations") | color(kMuted) | dim,
    }));
  }
}

// --- 3d. Debug mode notice (L192 TS LogoV2.tsx).
//     <Box paddingLeft={2} flexDirection="column">
//       <Text color="warning">Debug mode enabled</Text>
//       <Text dimColor>Logging to: {stderr | path}</Text>
//     </Box>
[[nodiscard]] inline auto RenderDebugNotice(const LogoV2Options& o) -> Element {
  if (!o.is_debug_mode) return Element();
  using namespace detail;
  Elements lines;
  lines.push_back(hbox({
    text("Debug mode enabled") | color(kWarningColor),
  }));
  lines.push_back(hbox({
    text("Logging to: "),
    text(o.debug_log_to_stderr ? "stderr" : o.debug_log_path),
  }) | dim | color(kMuted));
  return pad2(vbox(std::move(lines)));
}

// --- 3e. EmergencyTip (L193 TS).
//     TS EmergencyTip.tsx: usually a single line about "Use /settings to
//     change your model provider" or similar. Faithful structure:
//       <Box paddingLeft={2} flexDirection="row">
//         <Text color="warning">⚠</Text>
//         <Text dimColor>{emergencyTipText}</Text>
//       </Box>
[[nodiscard]] inline auto RenderEmergencyTip(const LogoV2Options& o) -> Element {
  if (!o.emergency_tip.has_value()) return Element();
  using namespace detail;
  return pad2(hbox({
    text("\xE2\x9A\xA0 ") | color(kWarningColor),                 // ⚠
    text(*o.emergency_tip) | dim | color(kMuted),
  }));
}

// --- 3f. Tmux session notice (L194-197 TS LogoV2.tsx).
[[nodiscard]] inline auto RenderTmuxNotice(const LogoV2Options& o) -> Element {
  if (!o.tmux_session.has_value()) return Element();
  using namespace detail;
  Elements lines;
  lines.push_back(hbox({
    text("tmux session: "),
    text(*o.tmux_session),
  }) | dim | color(kMuted));
  if (o.tmux_prefix.has_value()) {
    const std::string prefix = *o.tmux_prefix;
    std::string detach_msg =
        o.tmux_prefix_conflicts
            ? "Detach: " + prefix + " " + prefix
                  + " d (press prefix twice - Claude uses " + prefix + ")"
            : "Detach: " + prefix + " d";
    lines.push_back(text(std::move(detach_msg)) | dim | color(kMuted));
  }
  return pad2(vbox(std::move(lines)));
}

// --- 3g. Organisation announcement (L213-219 TS).
//     <Box paddingLeft={2} flexDirection="column">
//       [optional <Text dimColor>Message from {orgName}:</Text>]
//       <Text>{announcement}</Text>
//     </Box>
[[nodiscard]] inline auto RenderOrgAnnouncement(const LogoV2Options& o) -> Element {
  if (!o.company_announcement.has_value()) return Element();
  using namespace detail;
  Elements lines;
  if (o.org_name.has_value() && !o.org_name->empty()) {
    lines.push_back(hbox({
      text("Message from "),
      text(*o.org_name),
      text(":"),
    }) | dim | color(kMuted));
  }
  lines.push_back(text(*o.company_announcement));
  return pad2(vbox(std::move(lines)));
}

// --- 3h. Sandbox warning (compact/horizontal only, L312 & L490 TS).
//     <Text color="warning">Your bash commands will be sandboxed.
//                            Disable with /sandbox.</Text>
[[nodiscard]] inline auto RenderSandboxNotice(const LogoV2Options& o) -> Element {
  if (!o.show_sandbox_status) return Element();
  using namespace detail;
  return pad2(hbox({
    text("\xE2\x9A\xA0 ") | color(kWarningColor),
    text("Your bash commands will be sandboxed. Disable with /sandbox.")
      | color(kWarningColor),
  }));
}

// --- 3i. GuestPassesUpsell (L125 TS CondensedLogo.tsx; also shown in feed).
//     Structure (TS): three [✻] glyphs + "N guest passes at /passes" dimText.
[[nodiscard]] inline auto RenderGuestPassesUpsell(const LogoV2Options& o,
                                                 int count = 3) -> Element {
  if (!o.show_guest_passes) return Element();
  using namespace detail;
  Elements brackets;
  brackets.reserve(static_cast<std::size_t>(count * 3));
  for (int i = 0; i < count; ++i) {
    if (i > 0) brackets.push_back(text(" "));
    brackets.push_back(text("[") | color(kMuted) | dim);
    brackets.push_back(text("\xE2\x9C\xBB") | color(kClaude) | bold); // ✻
    brackets.push_back(text("]") | color(kMuted) | dim);
  }
  return pad2(hbox({
    hbox(std::move(brackets)),
    text("  "),
    text(std::to_string(count) + " guest passes at /passes")
      | dim | color(kMuted),
  }));
}

// --- 3j. OverageCreditUpsell (L133 TS CondensedLogo.tsx).
//     TS: <OverageCreditUpsell maxWidth twoLine> — structure varies,
//     faithful fallback: orange ⚠ + dimText "You have used 90% of your
//     credit this period · see /billing for details".
[[nodiscard]] inline auto RenderOverageCreditUpsell(const LogoV2Options& o) -> Element {
  if (!o.show_overage_credit) return Element();
  using namespace detail;
  return pad2(vbox({
    hbox({
      text("\xE2\x9A\xA0 ") | color(kWarningColor),
      text("Nearing monthly credit limit") | color(kWarningColor),
    }),
    hbox({
      text(" "),
      text("Visit /billing to see your usage · upgrades available")
        | dim | color(kMuted),
    }),
  }));
}

// --- 3k. StatusNotices (exactly 6 TS definitions from
//     statusNoticeDefinitions.tsx). We accept pre-rendered rows so callers
//     decide which are active (keeping this file IO-free).
[[nodiscard]] inline auto RenderStatusNotices(const LogoV2Options& o) -> Elements {
  using namespace detail;
  Elements out;
  out.reserve(o.status_notices.size());
  for (const auto& n : o.status_notices) {
    const auto& col = n.is_warning ? kWarningColor : kIdeColor;
    out.push_back(pad2(hbox({
      text(n.glyph.empty() ? std::string("\xE2\x9A\xA0 ") : (n.glyph + " "))
          | color(col),
      text(n.body) | color(col),
    })));
  }
  return out;
}

// --------------------------------------------------------------------
// §4  Notice aggregator — flat vbox over all active notices.
// --------------------------------------------------------------------
// Mirrors TS LogoV2.tsx L187-237 (condensed stack) and L459-525
// (compact/horizontal post-card stack). Order matters — it matches TS
// exactly: Voice → Opus → Channels → Debug → Emergency → Tmux →
// OrgAnnounce → Sandbox → StatusNotices → Guest/Overage (upsells).
//
// (Note: GuestPasses/Overage appear inside CondensedLogo's right column
// in TS — but the same components are ALSO rendered in FeedColumn. We
// render them last in the aggregated stack so both paths can consume
// this helper; CondensedLogo callers may hide them by setting bools off
// and rendering them via a dedicated call if stricter fidelity is needed.)
[[nodiscard]] inline auto RenderNoticeStackAggregated(const LogoV2Options& o)
    -> Elements {
  Elements out;
  out.reserve(6 + o.status_notices.size() + 3);
  auto push = [&](Element&& e) {
    // Only push if the element is non-null (has a renderer installed).
    // Inactive notice renderers return Element() (default-constructed,
    // null shared_ptr<Node>) — these MUST be filtered out because vbox
    // dereferences child pointers and null → UB / black screen.
    if (e) out.push_back(std::move(e));
  };
  push(RenderVoiceModeNotice(false));
  push(RenderOpus1MNotice());
  push(RenderChannelsNotice<>());
  push(RenderDebugNotice(o));
  push(RenderEmergencyTip(o));
  push(RenderTmuxNotice(o));
  push(RenderOrgAnnouncement(o));
  push(RenderSandboxNotice(o));
  for (auto& n : RenderStatusNotices(o)) push(std::move(n));
  push(RenderGuestPassesUpsell(o));
  push(RenderOverageCreditUpsell(o));
  return out;
}

// --------------------------------------------------------------------
// §4b  WelcomeV2 theme selector
// --------------------------------------------------------------------
// TS WelcomeV2.tsx dispatch (L9-19):
//   1. env.terminal === "Apple_Terminal" → AppleTerminalWelcomeV2
//      which internally branches on isLightTheme (L209).
//   2. ["light", "light-daltonized", "light-ansi"].includes(theme) → light variant
//   3. else → dark variant
//
// We expose all 4 combinations as a single enum so callers can dispatch
// in one step.  AppleTerminal variants use ▗/▖ + backgroundColor for the
// clawd (negative-style) instead of solid █ foreground blocks.
// TS REF: src/components/LogoV2/WelcomeV2.tsx::WelcomeV2 + AppleTerminalWelcomeV2
enum class WelcomeV2Theme {
  Dark,               // Normal terminal + dark/non-light theme
  Light,              // Normal terminal + light theme
  AppleTerminalDark,  // Apple Terminal + dark theme
  AppleTerminalLight, // Apple Terminal + light theme
};

// --------------------------------------------------------------------
// §5  WelcomeV2 58-col fixed-width static ASCII clawd card
// --------------------------------------------------------------------
// TS WelcomeV2.tsx builds the dark-themed variant as a completely
// STATIC 14-row × 58-col string of block chars. There is NO per-frame
// animation; the only variables are theme (dark/light/apple-terminal)
// and version-string substitution on row 0. The "scattered *" glyphs
// throughout are plain string literal '*' characters — NOT a starfield
// engine, NOT particle system, NOT per-frame position updates.
//
// We reproduce the dark-theme 14 rows exactly (char-for-char). Rows are
// indexed as t0 (header) → t1 (ellipsis separator) → t2..t15 (art).
// Clawd sits at rows 12-14 (body █████████ / ██▄█████▄██ / █████████)
// with a moon/planet to the right built from ░░/▒▒/██ gradient block chars.
namespace detail {
inline constexpr std::array<std::string_view, 15> kWelcomeV2DarkRows = {{
  // t0: header line (we append v{version} in place of the TS dynamic part)
  "",
  // t1: 58 × U+2026 HORIZONTAL ELLIPSIS separator
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
  "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6",
  // t2: 58 spaces
  "                                                          ",
  // t3: "*" + 39 spaces + 5 ██ + 2 ▓▓ + 1 ░ + 5 spaces
  "     *                                       \xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x93\xE2\x96\x93\xE2\x96\x91"
  "     ",
  // t4: 41 spaces + 3 ██ + 1 ▓ + 2 ░ + 5 spaces + 2 ░░ + 3 spaces
  "                                 *         \xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x93\xE2\x96\x91     \xE2\x96\x91\xE2\x96\x91   ",
  // t5: 12 spaces + 6 ░░░░░░ + 24 spaces + 3 ██ + 1 ▓ + 1 ░ + 11 spaces
  "            \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91                        \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x93\xE2\x96\x91           ",
  // t6: 4 spaces + 3 ░░░ + 3 spaces + 10 ░░░░░░░░░░ + 22 spaces + 3 ██ +
  //     1 ▓ + 1 ░ + 11 spaces
  "    \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91   \xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91                      \xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x93\xE2\x96\x91           ",
  // t7: 19 × ░ + 1 × "*" + 16 spaces + 2 ██ + 2 ▓░ + 2 ░░ + 6 spaces +
  //     1 ▓ + 3 spaces
  "   \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91    *                \xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x93\xE2\x96\x91\xE2\x96\x91      \xE2\x96\x93   ",
  // t8: 45 spaces + 2 ░░ + 2 ▓▓ + 3 ██ + 2 ▓▓ + 1 ░ + 4 spaces
  "                                             \xE2\x96\x91\xE2\x96\x93"
  "\xE2\x96\x93\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x93\xE2\x96\x93"
  "\xE2\x96\x91    ",
  // t9: leading dim "*" + 33 spaces + 4 ░░ + 19 spaces
  " *                                 \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91                   ",
  // t10: 33 spaces + 8 ░░░░░░░░ + 17 spaces
  "                                 \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91                 ",
  // t11: 31 spaces + 16 ░░░░░░░░░░░░░░░░ + 9 spaces
  "                               \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91           ",
  // t12: (clawd row 1) 6 spaces + clawd_body █████████ + 39 spaces +
  //      dim "*" + 1 space
  "       \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
  "                                        * ",
  // t13: (clawd row 2) 6 spaces + ██▄█████▄██ clawd_body + 24 spaces +
  //      bold "*" + 16 spaces
  "       \xE2\x96\x88\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88\xE2\x96\x88"
  "                        *                ",
  // t14: (clawd row 3) 6 spaces + clawd_body █████████ + 5 spaces + dim
  //      "*" + 35 spaces
  "       \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
  "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
  "     *                                   ",
}};

// Light-theme WelcomeV2 rows.  TS REF: WelcomeV2.tsx L20-106 (light branch).
// The light variant has a cloud-shape in the upper-left (6 ░ + 10 ░ + 19 ░)
// and a smaller moon/planet in the upper-right (██, ██▒▒██, ▒▒ patterns).
// Clawd sits at rows 12-14 in the lower-left, same as dark but with a
// different ground/horizon line (▒▒░░▒▒ etc.).
//
// Row layout (15 rows, indexed 0..14):
//   [0]  header (built dynamically — "Welcome to Claude Code vX.X.X")
//   [1]  58 × … ellipsis separator (same as dark, reused from dark array)
//   [2]  58 spaces  (blank)
//   [3]  58 spaces  (blank)
//   [4]  58 spaces  (blank)
//   [5]  12 spaces + 6 ░ + 40 spaces
//   [6]  4 spaces + 3 ░ + 3 spaces + 10 ░ + 38 spaces
//   [7]  3 spaces + 19 ░ + 36 spaces
//   [8]  58 spaces  (blank)
//   [9]  dim-segment: 27 spaces + 4 ░  +  normal-segment: 21 spaces + 2 ██ + 4 spaces
//   [10] dim-segment: 25 spaces + 10 ░ +  normal-segment: 15 spaces + ██▒▒██ + 2 spaces
//   [11] 44 spaces + 2 ▒▒ + 6 spaces + 2 ██ + 3 spaces + 1 ▒
//   [12] clawd row 1: 6 spaces + " █████████ " + 25 spaces + ▒▒░░▒▒ + 6 spaces + ▒ + space + ▒▒
//   [13] clawd row 2: 6 spaces + "██▄█████▄██" (bg) + 27 spaces + 2 ▒▒ + 9 spaces + 2 ▒▒ + space
//   [14] clawd row 3: 6 spaces + " █████████ " + 26 spaces + 1 ░ + 10 spaces + 1 ▒ + 3 spaces
//
// For rows [9] and [10], the leading ░ segment is dimColor in TS; we
// store the full-row strings here and colourise them at render time.
// For rows [12]-[14], the clawd body segment is colourised with
// clawd_body (and clawd_background bg for row 13) at render time.
inline constexpr std::array<std::string_view, 15> kWelcomeV2LightRows = {{
  // [0] header placeholder
  "",
  // [1] ellipsis separator (reused from dark array at render time)
  "",
  // [2] 58 spaces
  "                                                          ",
  // [3] 58 spaces
  "                                                          ",
  // [4] 58 spaces
  "                                                          ",
  // [5] 12 spaces + 6 ░ (U+2591) + 40 spaces
  "            \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91                                        ",
  // [6] 4 spaces + 3 ░ + 3 spaces + 10 ░ + 38 spaces
  "    \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91   \xE2\x96\x91\xE2\x96"
  "\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91                                      ",
  // [7] 3 spaces + 19 ░ + 36 spaces
  "   \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2"
  "\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96"
  "\x91\xE2\x96\x91\xE2\x96\x91                                    ",
  // [8] 58 spaces
  "                                                          ",
  // [9] 27 spaces + 4 ░ + 21 spaces + 2 █ (U+2588) + 4 spaces
  //     (first 31 chars = dim segment, rest = normal)
  "                           \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91                     \xE2\x96\x88\xE2\x96\x88    ",
  // [10] 25 spaces + 10 ░ + 15 spaces + ██▒▒ (U+2588 U+2588 U+2592 U+2592) ██ + 2 spaces
  //      (first 35 chars = dim segment, rest = normal)
  "                         \xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2"
  "\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91"
  "\xE2\x96\x91               \xE2\x96\x88\xE2\x96\x88\xE2\x96\x92\xE2"
  "\x96\x92\xE2\x96\x88\xE2\x96\x88  ",
  // [11] 44 spaces + 2 ▒ (U+2592) + 6 spaces + 2 █ + 3 spaces + 1 ▒
  "                                            \xE2\x96\x92\xE2\x96"
  "\x92      \xE2\x96\x88\xE2\x96\x88   \xE2\x96\x92",
  // [12] clawd row 1 placeholder (built from segments at render)
  "",
  // [13] clawd row 2 placeholder (built from segments at render)
  "",
  // [14] clawd row 3 placeholder (built from segments at render)
  "",
}};

// ------------------------------------------------------------------
// Apple Terminal clawd helpers.
// TS AppleTerminalWelcomeV2 renders the clawd using ▗/▖ (U+2597/U+2596)
// quadrant characters with backgroundColor="clawd_body" — a "negative"
// or "reverse-video" style that works around Apple Terminal's block-
// character rendering quirks.  Faithful structure:
//
//   Top row:  ▗(clawd_body) + [space+▗+5 spaces+▖+space] (fg=clawd_bg, bg=clawd_body) + ▖(clawd_body)
//   Mid row:  9 spaces with bg=clawd_body (solid orange bar)
//   Footer:   … + [bg-space] + [space] + [bg-space] + "   " + [bg-space] + [space] + [bg-space] + …
//
// TS REF: src/components/LogoV2/WelcomeV2.tsx::AppleTerminalWelcomeV2 L293-307 (light) + L404-418 (dark)
// ------------------------------------------------------------------

// Apple Terminal clawd — top row (▗/▖ style).
//   leading_spaces: number of space chars before the ▗ glyph
//   suffix:         trailing art string appended after the ▖ glyph
//   suffix_bold_star_col: column position (0-indexed) of a bold '*' within
//                         the suffix (used by dark variant; std::nullopt for light)
[[nodiscard]] inline auto BuildAppleClawdTopRow(
    int leading_spaces,
    std::string_view suffix,
    std::optional<int> suffix_bold_star_col = std::nullopt) -> Element {
  using ftxui::bgcolor;

  Elements parts;
  // Leading spaces.
  parts.push_back(text(std::string(static_cast<std::size_t>(leading_spaces), ' ')));
  // Left shoulder: ▗ in clawd_body foreground, no bg.
  parts.push_back(text("\xE2\x96\x97") | color(kClawdBody));  // ▗ U+2597
  // Body cavity: fg = clawd_background, bg = clawd_body.
  //   Content: " " + "▗" + "     " + "▖" + " "
  //   TS: <Text color="clawd_background" backgroundColor="clawd_body">{" "}▗{"     "}▖{" "}</Text>
  parts.push_back(text(" \xE2\x96\x97     \xE2\x96\x96 ")
                  | color(kClawdBackground) | bgcolor(kClawdBody));
  // Right shoulder: ▖ in clawd_body foreground, no bg.
  parts.push_back(text("\xE2\x96\x96") | color(kClawdBody));  // ▖ U+2596

  // Suffix: optionally with a bold '*' at a given column.
  if (suffix_bold_star_col.has_value() && *suffix_bold_star_col >= 0
      && static_cast<std::size_t>(*suffix_bold_star_col) < suffix.size()) {
    const int col = *suffix_bold_star_col;
    // Build suffix parts: before_star + bold_star + after_star.
    // We work in byte offsets since suffix is UTF-8.
    // Approximate: each '*' is 1 byte, each space is 1 byte.
    // For the dark variant suffix "                       *                "
    // the star is at byte position 23 (0-indexed from suffix start).
    if (col < static_cast<int>(suffix.size())) {
      parts.push_back(text(std::string(suffix.substr(0, static_cast<std::size_t>(col)))));
      parts.push_back(text(std::string(suffix.substr(static_cast<std::size_t>(col), 1))) | bold);
      parts.push_back(text(std::string(suffix.substr(static_cast<std::size_t>(col + 1)))));
    } else {
      parts.push_back(text(std::string(suffix)));
    }
  } else {
    parts.push_back(text(std::string(suffix)));
  }

  return hbox(std::move(parts));
}

// Apple Terminal clawd — middle bar row (9 bg-colored spaces).
//   leading_spaces: spaces before the bar
//   suffix:         trailing art after the bar
//   suffix_dim_star_col: column of a dim '*' in suffix (std::nullopt if none)
[[nodiscard]] inline auto BuildAppleClawdBarRow(
    int leading_spaces,
    std::string_view suffix,
    std::optional<int> suffix_dim_star_col = std::nullopt) -> Element {
  using ftxui::bgcolor;

  Elements parts;
  parts.push_back(text(std::string(static_cast<std::size_t>(leading_spaces), ' ')));
  // 9 spaces with backgroundColor=clawd_body — solid orange bar.
  // TS: <Text backgroundColor="clawd_body">{" ".repeat(9)}</Text>
  parts.push_back(text("         ") | bgcolor(kClawdBody));

  if (suffix_dim_star_col.has_value() && *suffix_dim_star_col >= 0) {
    const int col = *suffix_dim_star_col;
    if (col < static_cast<int>(suffix.size())) {
      parts.push_back(text(std::string(suffix.substr(0, static_cast<std::size_t>(col)))));
      parts.push_back(text(std::string(suffix.substr(static_cast<std::size_t>(col), 1)))
                      | dim | color(kMuted));
      parts.push_back(text(std::string(suffix.substr(static_cast<std::size_t>(col + 1)))));
    } else {
      parts.push_back(text(std::string(suffix)));
    }
  } else {
    parts.push_back(text(std::string(suffix)));
  }

  return hbox(std::move(parts));
}

// Apple Terminal footer — paws rendered as bg-colored spaces.
// TS AppleTerminalWelcomeV2 footer (L307 light, L418 dark):
//   "………" + <bg=clawd_body> </> + <> </> + <bg=clawd_body> </> + <>"   "</> +
//   <bg=clawd_body> </> + <> </> + <bg=clawd_body> </> + "………(░…▒…)"
// The trailing part is "………" for dark, "………░…▒…" for light.
[[nodiscard]] inline auto BuildAppleFooter(bool is_light) -> Element {
  using ftxui::bgcolor;

  // Build 7 × … (21 bytes).
  std::string e7;
  e7.reserve(21);
  for (int i = 0; i < 7; ++i) e7 += "\xE2\x80\xA6";

  // Build trailing … string.
  // Dark: 42 × … = 126 bytes
  // Light: 27 × … + ░ + 4 × … + ▒ + 4 × … = 27*3 + 3 + 4*3 + 3 + 4*3 = 81+3+12+3+12 = 111 bytes
  std::string trail;
  if (is_light) {
    for (int i = 0; i < 27; ++i) trail += "\xE2\x80\xA6";
    trail += "\xE2\x96\x91";  // ░
    for (int i = 0; i < 4; ++i) trail += "\xE2\x80\xA6";
    trail += "\xE2\x96\x92";  // ▒
    for (int i = 0; i < 4; ++i) trail += "\xE2\x80\xA6";
  } else {
    for (int i = 0; i < 42; ++i) trail += "\xE2\x80\xA6";
  }

  return hbox({
    text(e7),                                           // 7 × …
    text(" ") | bgcolor(kClawdBody),                    // paw 1 (bg)
    text(" "),                                          // gap
    text(" ") | bgcolor(kClawdBody),                    // paw 2 (bg)
    text("   "),                                        // 3-space inter-paw gap
    text(" ") | bgcolor(kClawdBody),                    // paw 3 (bg)
    text(" "),                                          // gap
    text(" ") | bgcolor(kClawdBody),                    // paw 4 (bg)
    text(std::move(trail)),                             // trailing …
  });
}

// Regular (non-Apple) footer — paws rendered as clawd_body foreground █ chars.
// TS: "………" + <color=clawd_body>"█ █   █ █"</> + "………(░…▒…)"
[[nodiscard]] inline auto BuildRegularFooter(bool is_light) -> Element {
  std::string e7;
  e7.reserve(21);
  for (int i = 0; i < 7; ++i) e7 += "\xE2\x80\xA6";

  std::string trail;
  if (is_light) {
    // 27 × … + ░ + 4 × … + ▒ + 4 × …
    for (int i = 0; i < 27; ++i) trail += "\xE2\x80\xA6";
    trail += "\xE2\x96\x91";
    for (int i = 0; i < 4; ++i) trail += "\xE2\x80\xA6";
    trail += "\xE2\x96\x92";
    for (int i = 0; i < 4; ++i) trail += "\xE2\x80\xA6";
  } else {
    // 42 × …
    for (int i = 0; i < 42; ++i) trail += "\xE2\x80\xA6";
  }

  // "█ █   █ █" in clawd_body color.
  const std::string paws = "\xE2\x96\x88 \xE2\x96\x88   \xE2\x96\x88 \xE2\x96\x88";

  return hbox({
    text(e7),
    text(paws) | color(kClawdBody),
    text(std::move(trail)),
  });
}

} // namespace detail

// Produce the 58-col fixed-width WelcomeV2 card. Version fills the
// header line as "Welcome to Claude Code vX.X.X " (TS t0). Returns a
// single Element; caller is responsible for wrapping in `flex` or
// `center` if the terminal is wider than 58 cols.
//
// Theme dispatch (faithful to TS WelcomeV2.tsx L9-19 + AppleTerminalWelcomeV2):
//   Dark              — starfield + solid █ clawd + moon/planet gradient
//   Light             — cloud shape (░) + small planet (██▒▒██) + solid clawd + ground (▒▒░░)
//   AppleTerminalDark — starfield + ▗/▖ negative clawd (bg-color workaround)
//   AppleTerminalLight— cloud + ▗/▖ negative clawd
//
// TS REF: src/components/LogoV2/WelcomeV2.tsx::WelcomeV2
// TS REF: src/components/LogoV2/WelcomeV2.tsx::AppleTerminalWelcomeV2
[[nodiscard]] inline auto RenderWelcomeV2(
    std::string_view version,
    WelcomeV2Theme theme = WelcomeV2Theme::Dark) -> Element {
  using namespace detail;
  using ftxui::bgcolor;

  const std::string v = version.empty() ? "0.0.0" : std::string(version);
  const bool is_light = (theme == WelcomeV2Theme::Light ||
                         theme == WelcomeV2Theme::AppleTerminalLight);
  const bool is_apple = (theme == WelcomeV2Theme::AppleTerminalDark ||
                         theme == WelcomeV2Theme::AppleTerminalLight);

  // --- Common header (all themes): TS t0 ---
  Element header = hbox({
    text("Welcome to Claude Code ") | color(kClaude),
    text("v" + v + " ") | dim | color(kMuted),
  });

  Elements art;
  art.reserve(18);
  art.push_back(std::move(header));

  // --- Ellipsis separator (TS t1, same for all themes) ---
  art.push_back(text(std::string(kWelcomeV2DarkRows[1])));

  if (is_light) {
    // ================================================================
    // LIGHT THEME  (TS WelcomeV2.tsx L20-106)
    // ================================================================
    // t2-t8: simple rows (blank + cloud shape + blank)
    for (std::size_t i = 2; i <= 8; ++i) {
      art.push_back(text(std::string(kWelcomeV2LightRows[i])));
    }

    // t9: dim ░░░░ segment + normal ██ segment
    // TS: <Text dimColor>{"                           ░░░░"}</Text>
    //     <Text>{"                     ██    "}</Text>
    // Dim part: 27 spaces + 4 ░ = 31 terminal chars = 27 + 12 = 39 bytes
    {
      std::string_view row = kWelcomeV2LightRows[9];
      constexpr std::size_t kDimBytes = 39;
      art.push_back(hbox({
        text(std::string(row.substr(0, kDimBytes))) | dim | color(kMuted),
        text(std::string(row.substr(kDimBytes))),
      }));
    }

    // t10: dim ░░░░░░░░░░ segment + normal ██▒▒██ segment
    // TS: <Text dimColor>{"                         ░░░░░░░░░░"}</Text>
    //     <Text>{"               ██▒▒██  "}</Text>
    // Dim part: 25 spaces + 10 ░ = 35 terminal chars = 25 + 30 = 55 bytes
    {
      std::string_view row = kWelcomeV2LightRows[10];
      constexpr std::size_t kDimBytes = 55;
      art.push_back(hbox({
        text(std::string(row.substr(0, kDimBytes))) | dim | color(kMuted),
        text(std::string(row.substr(kDimBytes))),
      }));
    }

    // t11: simple row — ▒▒ + ██ art (no color segmentation)
    art.push_back(text(std::string(kWelcomeV2LightRows[11])));

    // --- Clawd rows (t12-t14) ---
    if (is_apple) {
      // Apple Terminal Light: 2 clawd rows (top ▗/▖ + middle bar)
      // TS AppleTerminalWelcomeV2 L293-300

      // t12: 6 spaces + ▗(clawd) + [body cavity] + ▖(clawd) + ▒▒ art
      // TS: {"      "} + ▗(color=clawd_body) +
      //     {" "}▗{"     "}▖{" "}(color=clawd_background, bg=clawd_body) +
      //     ▖(color=clawd_body) + {"                           ▒▒         ▒▒ "}
      const std::string suffix12 =
          "                           \xE2\x96\x92\xE2\x96\x92"
          "         \xE2\x96\x92\xE2\x96\x92 ";
      art.push_back(BuildAppleClawdTopRow(/*leading=*/6, suffix12));

      // t13: 7 spaces + 9 bg-spaces bar + ░/▒ art
      // TS: {"       "} + {" ".repeat(9)}(bg=clawd_body) +
      //     {"                           ░          ▒   "}
      const std::string suffix13 =
          "                           \xE2\x96\x91"
          "          \xE2\x96\x92   ";
      art.push_back(BuildAppleClawdBarRow(/*leading=*/7, suffix13));

      // Note: Apple Terminal light has only 2 clawd rows (not 3).
      // TS AppleTerminalWelcomeV2 L293-307 shows t16 (top) + t17 (bar),
      // no separate t18 clawd row before the footer.

    } else {
      // Regular Light: 3 solid clawd rows
      // TS WelcomeV2.tsx L80-94

      // t12: 6 spaces + " █████████ " (clawd_body) + ▒▒░░▒▒ + ▒ ▒▒
      // TS: {"      "}<Text color="clawd_body"> █████████ </Text>
      //     {"                         ▒▒░░▒▒      ▒ ▒▒"}
      const std::string clawd_top =
          " \xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
          "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88 ";
      const std::string light_suffix12 =
          "                         \xE2\x96\x92\xE2\x96\x92"
          "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x92\xE2\x96\x92"
          "      \xE2\x96\x92 \xE2\x96\x92\xE2\x96\x92";
      art.push_back(hbox({
        text("      "),
        text(clawd_top) | color(kClawdBody),
        text(light_suffix12),
      }));

      // t13: 6 spaces + "██▄█████▄██" (clawd_body + clawd_background bg) + ▒▒
      // TS: {"      "}
      //     <Text color="clawd_body" backgroundColor="clawd_background">
      //       ██▄█████▄██
      //     </Text>
      //     {"                           ▒▒         ▒▒ "}
      const std::string clawd_mid =
          "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88\xE2\x96\x88"
          "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x84\xE2\x96\x88\xE2\x96\x88";
      const std::string light_suffix13 =
          "                           \xE2\x96\x92\xE2\x96\x92"
          "         \xE2\x96\x92\xE2\x96\x92 ";
      art.push_back(hbox({
        text("      "),
        text(clawd_mid) | color(kClawdBody) | bgcolor(kClawdBackground),
        text(light_suffix13),
      }));

      // t14: 6 spaces + " █████████ " (clawd_body) + ░ + ▒
      // TS: {"      "}<Text color="clawd_body"> █████████ </Text>
      //     {"                          ░          ▒   "}
      const std::string light_suffix14 =
          "                          \xE2\x96\x91"
          "          \xE2\x96\x92   ";
      art.push_back(hbox({
        text("      "),
        text(clawd_top) | color(kClawdBody),
        text(light_suffix14),
      }));
    }

  } else {
    // ================================================================
    // DARK THEME  (TS WelcomeV2.tsx L108-197)
    // ================================================================
    // t2-t6, t8: plain rows (no * styling needed)
    for (std::size_t i : {2u, 3u, 4u, 5u, 6u, 8u}) {
      art.push_back(text(std::string(kWelcomeV2DarkRows[i])));
    }

    // t7: scattered ░ + bold '*' + moon/planet gradient
    // TS L145: <Text>{"   ░░…░░    "}</Text><Text bold={true}>*</Text>
    //          <Text>{"                ██▓░░      ▓   "}</Text>
    // The '*' is the only '*' in this row. Find it and render bold.
    {
      std::string_view row7 = kWelcomeV2DarkRows[7];
      auto star_pos = row7.find('*');
      if (star_pos != std::string_view::npos) {
        art.push_back(hbox({
          text(std::string(row7.substr(0, star_pos))),
          text("*") | ftxui::bold,
          text(std::string(row7.substr(star_pos + 1))),
        }));
      } else {
        art.push_back(text(std::string(row7)));
      }
    }

    // t9, t10, t11: entire rows are dimColor (TS L147-149)
    //   t9:  <Text dimColor={true}>{" *        ░░░░                   "}</Text>
    //   t10: <Text dimColor={true}>{"          ░░░░░░░░              "}</Text>
    //   t11: <Text dimColor={true}>{"        ░░░░░░░░░░░░░░░░        "}</Text>
    for (std::size_t i : {9u, 10u, 11u}) {
      art.push_back(text(std::string(kWelcomeV2DarkRows[i])) | dim | color(kMuted));
    }

    if (is_apple) {
      // Apple Terminal Dark: repositioned clawd (TS L397-411)
      //
      // In Apple Terminal dark, the clawd shifts down by one row
      // compared to regular dark:
      //   t12 (was solid clawd row 1) → scattered * only
      //   t13 (was solid clawd row 2) → Apple ▗/▖ top
      //   t14 (was solid clawd row 3) → Apple bg-color bar

      // t12: 54 spaces + dim '*' + "  "
      // TS AppleTerminalWelcomeV2 L397:
      //   <Text>{"                                                      "}</Text>
      //   <Text dimColor={true}>*</Text><Text> </Text>
      art.push_back(hbox({
        text("                                                      "),
        text("*") | dim | color(kMuted),
        text(" "),
      }));

      // t13: 8 spaces + Apple ▗/▖ top + 23 spaces + bold '*' + 16 spaces
      // TS L404: {"        "} + ▗ + body_cavity + ▖ +
      //          {"                       "} + bold * + {"                "}
      const std::string suffix13 =
          "                       *                ";
      // Bold '*' is at byte position 23 within suffix (0-indexed).
      art.push_back(BuildAppleClawdTopRow(
          /*leading=*/8, suffix13, /*bold_star_col=*/23));

      // t14: 8 spaces + 9 bg-spaces bar + 5 spaces + dim '*' + 35 spaces
      // TS L411: {"        "} + {" ".repeat(9)}(bg=clawd_body) +
      //          {"      *                                   "}
      const std::string suffix14 =
          "      *                                   ";
      // Dim '*' at byte position 6 within suffix.
      art.push_back(BuildAppleClawdBarRow(
          /*leading=*/8, suffix14, /*dim_star_col=*/6));

    } else {
      // Regular Dark: 3 solid clawd rows at position 6 (TS L163-189)
      //
      // TS L163-189 faithful rendering:
      //   t12 (L171): "      " + clawd_body(" █████████ ") +
      //               "                                       " + dim("*") + " "
      //   t13 (L178): "      " + clawd_body+bg("██▄█████▄██") +
      //               "                        " + bold("*") + "                "
      //   t14 (L185): "      " + clawd_body(" █████████ ") +
      //               "     *                                   "

      // Helper: split a dark clawd row into (prefix, body, suffix)
      // and render with per-row * styling.
      //   body_pos:  byte offset of body within row (always 6 = 6 spaces)
      //   body_len:  byte length of body (9 █ = 27 bytes, or 11-char body w/ spaces)
      //   star_mode: how to render '*' in the suffix: 0=dim, 1=bold, 2=normal
      //   body_bg:   if true, apply bgcolor(kClawdBackground) to body
      auto render_dark_clawd_row =
          [&](std::string_view row, std::size_t body_pos, std::size_t body_len,
              int star_mode, bool body_bg) -> Element {
        if (row.size() < body_pos + body_len) {
          return text(std::string(row));
        }
        std::string prefix(row.substr(0, body_pos));
        std::string body(row.substr(body_pos, body_len));
        std::string suffix(row.substr(body_pos + body_len));

        auto body_el = text(body) | color(kClawdBody);
        if (body_bg) body_el = body_el | bgcolor(kClawdBackground);

        // Find '*' in suffix and apply styling
        auto star_pos = suffix.find('*');
        if (star_pos == std::string::npos) {
          return hbox({text(prefix), body_el, text(suffix)});
        }

        std::string pre_star(suffix.substr(0, star_pos));
        std::string post_star(suffix.substr(star_pos + 1));

        Element star_el;
        switch (star_mode) {
          case 0: star_el = text("*") | dim | color(kMuted); break;
          case 1: star_el = text("*") | ftxui::bold; break;
          default: star_el = text("*"); break;
        }

        return hbox({text(prefix), body_el,
                     text(pre_star), star_el, text(post_star)});
      };

      // t12: body = 9 █ (27 bytes), star = dim, no body bg
      // TS L171: suffix has dim '*' at the end
      art.push_back(render_dark_clawd_row(
          kWelcomeV2DarkRows[12], /*body_pos=*/6, /*body_len=*/27,
          /*star_mode=*/0, /*body_bg=*/false));

      // t13: body = 9 █ (27 bytes), star = bold, body has bg
      // TS L178: clawd body has backgroundColor="clawd_background", star is bold
      art.push_back(render_dark_clawd_row(
          kWelcomeV2DarkRows[13], /*body_pos=*/6, /*body_len=*/27,
          /*star_mode=*/1, /*body_bg=*/true));

      // t14: body = 9 █ (27 bytes), star = normal, no body bg
      // TS L185: suffix has normal '*' (no dim, no bold)
      art.push_back(render_dark_clawd_row(
          kWelcomeV2DarkRows[14], /*body_pos=*/6, /*body_len=*/27,
          /*star_mode=*/2, /*body_bg=*/false));
    }
  }

  // --- Footer (TS t15 / t16 / t18) ---
  art.push_back(is_apple ? BuildAppleFooter(is_light)
                         : BuildRegularFooter(is_light));

  return vbox(std::move(art)) | size(WIDTH, EQUAL, kWelcomeV2FixedWidth);
}

// --------------------------------------------------------------------
// Convenience: resolve WelcomeV2Theme from TS theme string + terminal type.
// Mirrors TS WelcomeV2.tsx dispatch (L9-19):
//   env.terminal === "Apple_Terminal" → AppleTerminal variant
//   ["light", "light-daltonized", "light-ansi"].includes(theme) → Light
//   else → Dark
// TS REF: src/components/LogoV2/WelcomeV2.tsx::WelcomeV2
[[nodiscard]] inline auto ResolveWelcomeV2Theme(
    std::string_view theme_name,
    bool is_apple_terminal) -> WelcomeV2Theme {
  const bool is_light =
      (theme_name == "light" || theme_name == "light-daltonized" ||
       theme_name == "light-ansi");
  if (is_apple_terminal) {
    return is_light ? WelcomeV2Theme::AppleTerminalLight
                    : WelcomeV2Theme::AppleTerminalDark;
  }
  return is_light ? WelcomeV2Theme::Light : WelcomeV2Theme::Dark;
}

// Convenience: render with a TS theme string + Apple Terminal flag.
[[nodiscard]] inline auto RenderWelcomeV2(
    std::string_view version,
    std::string_view theme_name,
    bool is_apple_terminal) -> Element {
  return RenderWelcomeV2(version,
                         ResolveWelcomeV2Theme(theme_name, is_apple_terminal));
}

// --------------------------------------------------------------------
// §5b  Feed / FeedColumn system — faithful port of Feed.tsx + FeedColumn.tsx
// --------------------------------------------------------------------
// TS Feed types (Feed.tsx L6-19):
//   type FeedLine  = { text: string; timestamp?: string }
//   type FeedConfig = {
//     title: string; lines: FeedLine[];
//     footer?: string; emptyMessage?: string;
//     customContent?: { content: ReactNode; width: number }
//   };
//
// calculateFeedWidth (Feed.tsx L24-50):
//   maxWidth = stringWidth(title)
//   if customContent: maxWidth = max(maxWidth, customContent.width)
//   else if lines.empty && emptyMessage: maxWidth = max(maxWidth, stringWidth(emptyMessage))
//   else: maxTimestampWidth = max(0, ...lines.map(l => l.timestamp?.width ?? 0))
//         for each line: w = textWidth + (timestamp>0 ? timestampWidth+2 : 0)
//                        maxWidth = max(maxWidth, w)
//   if footer: maxWidth = max(maxWidth, stringWidth(footer))
struct FeedLine {
  std::string text;
  std::optional<std::string> timestamp;
};

struct FeedConfig {
  std::string title;
  std::vector<FeedLine> lines;
  std::optional<std::string> footer;
  std::optional<std::string> empty_message;
};

namespace detail {

// TS utils/format.ts truncate(s, maxWidth) — suffix-truncate with
// trailing U+2026 HORIZONTAL ELLIPSIS when the string is too long.
// Simple byte-based truncation (sufficient for ASCII paths/models).
// Defined early (before RenderFeedColumn) so it is visible to all helpers.
// TS REF: src/utils/format.ts::truncate
[[nodiscard]] inline auto truncate_str(std::string s, int max_width)
    -> std::string {
  if (max_width <= 0) return "";
  if (static_cast<int>(s.size()) <= max_width) return s;
  if (max_width == 1) return "\xE2\x80\xA6";
  s.resize(static_cast<std::size_t>(max_width - 3));
  s += "\xE2\x80\xA6";  // … U+2026
  return s;
}

// TS Feed::calculateFeedWidth — faithful logic (byte-width approx).
[[nodiscard]] inline auto calculate_feed_width(const FeedConfig& cfg) -> int {
  auto sw = [](const std::string& s) { return static_cast<int>(s.size()); };

  int max_w = sw(cfg.title);
  if (cfg.lines.empty() && cfg.empty_message.has_value()) {
    max_w = std::max(max_w, sw(*cfg.empty_message));
  } else {
    int max_ts = 0;
    for (const auto& l : cfg.lines) {
      if (l.timestamp.has_value()) {
        max_ts = std::max(max_ts, sw(*l.timestamp));
      }
    }
    for (const auto& l : cfg.lines) {
      int w = sw(l.text);
      if (max_ts > 0) w += max_ts + 2;  // "  " gap between timestamp + text
      max_w = std::max(max_w, w);
    }
  }
  if (cfg.footer.has_value()) {
    max_w = std::max(max_w, sw(*cfg.footer));
  }
  return max_w;
}

// TS Feed component (Feed.tsx L51-106) — renders a single feed:
//   <Text bold color=claude>{title}</Text>
//   customContent.content  OR  (emptyMessage  OR  lines+padding+footer)
// Each row: [timestamp padEnd(maxTs) + "  " + truncate(text, textWidth)]
[[nodiscard]] inline auto RenderFeed(const FeedConfig& cfg, int actual_width)
    -> Element {
  using namespace ftxui;
  actual_width = std::max(actual_width, 10);

  // Title row: bold + claude color.
  Element title_el = text(truncate_str(cfg.title, actual_width))
                   | bold | color(kClaude);

  // Body rows.
  int max_ts = 0;
  for (const auto& l : cfg.lines) {
    if (l.timestamp.has_value()) {
      max_ts = std::max(max_ts, static_cast<int>(l.timestamp->size()));
    }
  }

  Elements body;
  if (cfg.lines.empty() && cfg.empty_message.has_value()) {
    body.push_back(text(truncate_str(*cfg.empty_message, actual_width))
                   | dim | color(kMuted));
  } else {
    body.reserve(cfg.lines.size() + (cfg.footer ? 1 : 0));
    const int text_width = std::max(10,
        actual_width - (max_ts > 0 ? max_ts + 2 : 0));
    for (const auto& l : cfg.lines) {
      Elements row_parts;
      if (max_ts > 0) {
        std::string ts = l.timestamp.value_or("");
        // padEnd: right-pad to max_ts with spaces.
        if (static_cast<int>(ts.size()) < max_ts) {
          ts += std::string(static_cast<std::size_t>(
              max_ts - static_cast<int>(ts.size())), ' ');
        }
        row_parts.push_back(text(ts) | dim | color(kMuted));
        row_parts.push_back(text("  "));  // gap
      }
      row_parts.push_back(text(truncate_str(l.text, text_width)));
      body.push_back(hbox(std::move(row_parts)));
    }
    if (cfg.footer.has_value()) {
      body.push_back(text(truncate_str(*cfg.footer, actual_width))
                     | dim);
    }
  }

  Elements all;
  all.reserve(1 + body.size());
  all.push_back(std::move(title_el));
  for (auto& r : body) all.push_back(std::move(r));
  return vbox(std::move(all))
       | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actual_width);
}

// TS FeedColumn (FeedColumn.tsx L11-54):
//   widths = feeds.map(calculateFeedWidth)
//   actualWidth = min(max(...widths), maxWidth)
//   feeds.map((f, i) => <>
//     <Feed config=f actualWidth=actualWidth />
//     {i < feeds.length-1 && <Divider color=claude width=actualWidth />}
//   </>)
// Returns the rightWidth used for geometry hints (caller's benefit).
struct RenderedFeedColumn {
  Element element;
  int actual_width;
};

[[nodiscard]] inline auto RenderFeedColumn(std::vector<FeedConfig> feeds,
                                            int max_width)
    -> RenderedFeedColumn {
  if (feeds.empty()) {
    return { text(""), 0 };
  }
  int max_of_all = 0;
  for (const auto& f : feeds) {
    max_of_all = std::max(max_of_all, calculate_feed_width(f));
  }
  int actual_width = std::min(max_of_all, std::max(max_width, 10));

  Elements rows;
  rows.reserve(feeds.size() * 2);
  for (std::size_t i = 0; i < feeds.size(); ++i) {
    rows.push_back(RenderFeed(feeds[i], actual_width));
    if (i + 1 < feeds.size()) {
      // TS Divider color=claude width=actualWidth — 1-row horizontal
      // divider of actualWidth chars in claude accent colour.
      // Using FTXUI separator() styled with the claude colour.
      rows.push_back(ftxui::separator() | color(kClaude)
                   | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actual_width));
    }
  }
  return { vbox(std::move(rows))
               | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, actual_width),
           actual_width };
}

} // namespace detail

// Output geometry for horizontal-layout render — mirrors what the caller
// needs to know for slot sizing / layout debugging.
struct HorizontalLayoutOutput {
  Element layout;
  int left_width  = 0;
  int right_width = 0;
};

// --------------------------------------------------------------------
// §6  Compact & Horizontal card layouts
// --------------------------------------------------------------------
namespace detail {

// TS logoV2Utils.MAX_USERNAME_LENGTH = 20.
inline constexpr int kMaxUsernameLength = 20;
// TS logoV2Utils layout constants — faithful to L17-22.
inline constexpr int kMaxLeftWidth    = 50;
inline constexpr int kBorderPadding   = 4;
inline constexpr int kDividerWidth    = 1;
inline constexpr int kContentPadding  = 2;

// TS formatWelcomeMessage(username):
//   username empty/null OR longer than MAX_USERNAME_LENGTH(20) → "Welcome back!"
//   Otherwise → "Welcome back {username}!"
// NOTE: The first-run "Welcome to Claude Code" variant is NOT produced by
// this function — it is handled separately by the WelcomeV2 card renderer
// (see RenderWelcomeV2).
// TS REF: src/components/LogoV2/LogoV2.tsx::formatWelcomeMessage
[[nodiscard]] inline auto format_welcome_message(
    const std::optional<std::string>& username) -> std::string {
  if (!username.has_value() || username->empty()
      || static_cast<int>(username->size()) > kMaxUsernameLength) {
    return "Welcome back!";
  }
  return "Welcome back " + *username + "!";
}

} // namespace detail

// Compact mode (cols < 70, TS LogoV2.tsx L253-330):
//
// Structure (faithful, line-by-line):
//   <OffscreenFreeze>
//     <Box flexDirection="column"
//          borderStyle="round" borderColor="claude"
//          borderText={compactBorderTitle}   ← " Claude Code " in claude color
//          paddingX={1} paddingY={1}
//          alignItems="center" width={columns}>
//       <Text bold>{welcomeMessage}</Text>
//       <Box marginTop={1}><Clawd /></Box>
//       <Text dimColor>{modelDisplayName}</Text>
//       <Text dimColor>{billingType}</Text>
//       <Text dimColor>{agent ? "@agent · cwd" : cwd}</Text>
//     </Box>
//   </OffscreenFreeze>
//   notices…
//
// borderText: { content: compactBorderTitle, position: "top", align: "start", offset: 1 }
//   → the title text is inset 1 char from the left border corner.
//
// FTXUI Ink-compatibility note: Ink's borderText embeds text directly into the
// border stroke. FTXUI's closest equivalent is ftxui::window(title, body) which
// paints title inside a ╭─{title}─…─╮ box. The visual structure (inset title on
// the top border, rounded-style border) is preserved.
[[nodiscard]] inline auto RenderCompactLayout(const LogoV2Options& o,
                                               int term_cols,
                                               int /*is_new_user_unused*/ = 0)
    -> Element {
  using namespace detail;
  namespace logo = cc::ui::logo;
  using ftxui::window;

  const std::string welcome = format_welcome_message(o.username);
  // Compact card uses BORDER_PADDING (4 chars: 2 borders + 2 paddingX).
  // TS compact layout sets width={columns} (full terminal width).
  // Card inner width = columns - kBorderPadding (subtract 2 borders + 2 padX).
  const int card_width   = std::max(term_cols, 20);
  const int inner_width  = std::max(card_width - kBorderPadding, 12);

  // If welcome message is wider than available space, fall back to the
  // username=null variant (TS LogoV2.tsx L255-263).
  const std::string welcome_effective =
      static_cast<int>(welcome.size()) > inner_width
          ? format_welcome_message(std::nullopt)
          : welcome;

  // compactBorderTitle: color("claude", userTheme)(" Claude Code ")
  // Title sits on the top border, inset by offset=1 (TS borderText.offset).
  Element border_title = hbox({
      // offset=1: 1 leading space, then the title text.
      text(" ") | color(kClaude),
      text("Claude Code") | color(kClaude) | bold,
      text(" ") | color(kClaude),
  });

  // --- Clawd (9 cols × 3 rows) faithful to TS Clawd.tsx POSES.default. ---
  auto clawd = [&]() -> Element {
    const std::string_view r1L = " \xE2\x96\x90";                           //  ▐
    const std::string_view r1E = "\xE2\x96\x9B\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x88\xE2\x96\x9C";               // ▛███▜
    const std::string_view r1R = "\xE2\x96\x8C";                           // ▌
    const std::string_view r2L = "\xE2\x96\x9D\xE2\x96\x9C";               // ▝▜
    const std::string_view r2B = "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x88\xE2\x96\x88";               // █████
    const std::string_view r2R = "\xE2\x96\x9B\xE2\x96\x98";               // ▛▘
    const std::string_view r3  = "  \xE2\x96\x98\xE2\x96\x98 "
                                 "\xE2\x96\x9D\xE2\x96\x9D  ";             // ▘▘ ▝▝
    return vbox({
      // Row 1: {r1L (fg=clawd)} {r1E (fg=clawd, bg=clawd_bg)} {r1R (fg=clawd)}
      hbox({
        text(std::string(r1L)) | color(kClawdBody),
        text(std::string(r1E)) | color(kClawdBody),
        text(std::string(r1R)) | color(kClawdBody),
      }),
      // Row 2: {r2L} {r2B (bg)} {r2R}
      hbox({
        text(std::string(r2L)) | color(kClawdBody),
        text(std::string(r2B)) | color(kClawdBody),
        text(std::string(r2R)) | color(kClawdBody),
      }),
      // Row 3: feet (fg=clawd_body only, no bg)
      text(std::string(r3)) | color(kClawdBody),
    }) | ftxui::center;
  };

  // --- Model line + billing + agent·cwd, all dimColor (TS L289,293,297) ---
  // Width budget = inner_width (inside card, after borders + paddingX).
  // "paddingX={1}" on outer Box → content starts 2 cols in.
  const int content_budget = inner_width;

  // TS REF: LogoV2.tsx L169-178 — modelDisplayName = truncate(
  //   fullModelDisplayName + effortSuffix, LEFT_PANEL_MAX_WIDTH - 20)
  // Pre-truncate to kMaxLeftWidth - 20 = 30 before layout-specific clipping.
  const std::string model_raw = !o.model_display_name.empty()
      ? o.model_display_name : std::string("Claude");
  const int model_trunc_width =
      std::min(kMaxLeftWidth - 20, content_budget);
  const std::string model_display = truncate_str(model_raw, model_trunc_width);
  const std::string billing_display =
      truncate_str(o.billing_type, content_budget);

  const std::string cwd_line = [&]() -> std::string {
    // cwdAvailableWidth = agentName ? columns - 4 - 1 - stringWidth(agentName) - 3
    //                                   : columns - 4
    // TS L265-266. Note: 4 = layoutWidth (border+padding).
    int budget = content_budget;
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      // "@" + name + " · " = 1 + name.size() + 3
      budget = budget - 1 - static_cast<int>(o.agent_name->size()) - 3;
    }
    budget = std::max(budget, 10);
    std::string cwd = o.cwd;
    if (static_cast<int>(cwd.size()) > budget) {
      cwd = "\xE2\x80\xA6" + cwd.substr(
          cwd.size() - static_cast<std::size_t>(budget - 3));
    }
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      return "@" + *o.agent_name + " \xC2\xB7 " + cwd;
    }
    return cwd;
  }();

  // alignItems="center" → center each line horizontally.
  Element inner = vbox({
    text(welcome_effective) | bold | ftxui::center,
    text(""),                        // marginTop={1} (Clawd wrapped in it)
    clawd(),
    text(model_display) | dim | color(kMuted) | ftxui::center,
    text(billing_display) | dim | color(kMuted) | ftxui::center,
    text(cwd_line)      | dim | color(kMuted) | ftxui::center,
  }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, inner_width);

  // ftxui::window(title, body) — equivalent of Ink's borderText with
  // position="top" align="start". Border colour = claude (kClaude).
  Element card = window(std::move(border_title), std::move(inner))
               | color(kClaude);

  return card | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, card_width);
}

// Horizontal mode (cols >= 70, TS LogoV2.tsx L331-428):
//
// Outer structure (faithful):
//   <OffscreenFreeze>
//     <Box flexDirection="column"
//          borderStyle="round" borderColor="claude"
//          borderText={borderTitle}>          ← " Claude Code vX.X.X "
//       <Box flexDirection="row" paddingX={1} gap={1}>
//         {LEFT_PANEL}
//         {layoutMode === "horizontal" && <VERTICAL_DIVIDER />}
//         {layoutMode === "horizontal" && <FeedColumn feeds=... />}
//       </Box>
//     </Box>
//   </OffscreenFreeze>
//   notices…
//
// borderText offset=3 → the title text starts 3 chars in from the left corner.
//
// Left panel (TS L403-411):
//   <Box flexDirection="column" width={leftWidth}
//        justifyContent="space-between" alignItems="center" minHeight={9}>
//     <Box marginTop={1}><Text bold>{welcomeMessage}</Text></Box>
//     <Clawd />
//     <Box flexDirection="column" alignItems="center">
//       <Text dimColor>{modelLine}</Text>
//       <Text dimColor>{cwdLine}</Text>
//     </Box>
//   </Box>
//
// Layout math (faithful, see logoV2Utils L43-74):
//   optimalLeftWidth = min(max( welcome.width, cwdLine.width, modelLine.width, 20 ) + 4, 50)
//   if horizontal:
//     leftWidth  = optimalLeftWidth
//     usedSpace  = BORDER_PADDING(4) + CONTENT_PADDING(2) + DIVIDER(1) + leftWidth
//     rightWidth = max(30, columns - usedSpace)
//     totalWidth = min(leftWidth + rightWidth + DIVIDER + CONTENT_PADDING,
//                      columns - BORDER_PADDING)
//     if total clamped: rightWidth = totalWidth - leftWidth - DIVIDER - CONTENT_PADDING
//   (compact branch not reached here — horizontal mode selection is the caller's)
//
// Feed selection (TS L421, 4-armed ternary):
//   showOnboarding       → [ProjectOnboarding, RecentActivity]
//   showGuestPassesUpsell → [RecentActivity, GuestPasses]
//   showOverageCredit    → [RecentActivity, OverageCredit]
//   (default)            → [RecentActivity, What'sNew]
//
// We accept feeds as a vector<FeedConfig> via LogoV2Options; if empty we fall
// back to the default two-feed layout [RecentActivity, What'sNew] using the
// placeholders provided by the repl_screen caller data.

[[nodiscard]] inline auto RenderHorizontalLayout(
    const LogoV2Options& o,
    int term_cols,
    int /*is_new_user_unused*/ = 0,
    std::vector<FeedConfig> feeds = std::vector<FeedConfig>(),
    Element feed_column_override = Element())
    -> HorizontalLayoutOutput {
  using namespace detail;

  const int columns = std::max(term_cols, kHorizontalMinCols);

  // --- Compute welcome / modelLine / cwdLine. ---
  const std::string welcome = format_welcome_message(o.username);

  // modelLine: org ? `${model} · ${billing} · ${orgName}` : `${model} · ${billing}`
  // (TS LogoV2.tsx L332-333 — NOT the compact 3-separate-lines variant).
  //
  // TS REF: LogoV2.tsx L169-178 — modelDisplayName is pre-truncated to
  //   LEFT_PANEL_MAX_WIDTH - 20 (= 30) before being composed into modelLine.
  //   "-20 to account for the max length of subscription name
  //    '· Claude Enterprise'." (TS comment).
  const std::string model_raw = o.model_display_name.empty()
      ? std::string("Claude") : o.model_display_name;
  const std::string model_for_line =
      truncate_str(model_raw, kMaxLeftWidth - 20);

  std::string model_line_full = [&]() {
    std::string m = model_for_line;
    if (!o.billing_type.empty()) {
      m += " \xC2\xB7 " + o.billing_type;
    }
    if (o.org_name.has_value() && !o.org_name->empty()) {
      m += " \xC2\xB7 " + *o.org_name;
    }
    return m;
  }();

  // cwdLine: agentName ? `@${agent} · ${truncatedCwd}` : truncatedCwd
  // Left-panel budget = LEFT_PANEL_MAX_WIDTH (50).
  // TS L333: cwdAvailableWidth = agentName ? MAX(50) - 1 - name.size() - 3 : 50
  // (Note: TS uses agent name + "@" + " · " as the fixed prefix.)
  const std::string cwd_line_full = [&]() {
    const int max_w = kMaxLeftWidth;
    int agent_cost = 0;
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      agent_cost = 1 + static_cast<int>(o.agent_name->size()) + 3;
    }
    int budget = std::max(max_w - agent_cost, 10);
    std::string cwd = o.cwd;
    if (static_cast<int>(cwd.size()) > budget) {
      cwd = "\xE2\x80\xA6" + cwd.substr(
          cwd.size() - static_cast<std::size_t>(budget - 3));
    }
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      return "@" + *o.agent_name + " \xC2\xB7 " + cwd;
    }
    return cwd;
  }();

  // --- calculateOptimalLeftWidth (TS L80-92) ---
  auto sw = [](const std::string& s){ return static_cast<int>(s.size()); };
  const int content_width = std::max({
      sw(welcome), sw(cwd_line_full), sw(model_line_full), 20 /* min for clawd */ });
  const int optimal_left_width = std::min(content_width + 4, kMaxLeftWidth);

  // --- calculateLayoutDimensions, horizontal branch (TS L43-65) ---
  const int left_width  = optimal_left_width;
  const int used_space  = kBorderPadding + kContentPadding + kDividerWidth + left_width;
  int right_width       = std::max(30, columns - used_space);
  int total_width = std::min(
      left_width + right_width + kDividerWidth + kContentPadding,
      columns - kBorderPadding);
  if (total_width < left_width + right_width + kDividerWidth + kContentPadding) {
    right_width = total_width - left_width - kDividerWidth - kContentPadding;
  }
  right_width = std::max(right_width, 20);  // safety minimum

  // --- borderTitle (TS L251):
  //   ` ${color("claude", userTheme)("Claude Code")} ${color("inactive", userTheme)(`v${version}`)} `
  //   offset=3 → title starts at column 3 from the left border corner.
  const std::string v = o.version.empty() ? std::string("0.0.0") : o.version;
  Element border_title = hbox({
      text("   ") | color(kClaude),                // offset=3 leading spaces
      text("Claude Code") | color(kClaude) | bold,
      text(" v" + v + " ") | color(kMuted) | dim,  // inactive/muted version
  });

  // --- Clawd (standard 3 rows, 9 cols) centered. ---
  auto clawd = [&]() -> Element {
    using ftxui::vbox;
    const std::string_view r1L = " \xE2\x96\x90";
    const std::string_view r1E = "\xE2\x96\x9B\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x88\xE2\x96\x9C";
    const std::string_view r1R = "\xE2\x96\x8C";
    const std::string_view r2L = "\xE2\x96\x9D\xE2\x96\x9C";
    const std::string_view r2B = "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x88\xE2\x96\x88";
    const std::string_view r2R = "\xE2\x96\x9B\xE2\x96\x98";
    const std::string_view r3  = "  \xE2\x96\x98\xE2\x96\x98 "
                                 "\xE2\x96\x9D\xE2\x96\x9D  ";
    return vbox({
      ftxui::hbox({
        text(std::string(r1L)) | color(kClawdBody),
        text(std::string(r1E)) | color(kClawdBody),
        text(std::string(r1R)) | color(kClawdBody),
      }),
      ftxui::hbox({
        text(std::string(r2L)) | color(kClawdBody),
        text(std::string(r2B)) | color(kClawdBody),
        text(std::string(r2R)) | color(kClawdBody),
      }),
      text(std::string(r3)) | color(kClawdBody),
    }) | ftxui::center;
  };

  // --- Left panel: justify-content: space-between approximated. ---
  // FTXUI can't do true space-between; we push the welcome up, clawd in middle,
  // meta at bottom using flexbox with filler rows.
  Element left_panel = ftxui::vbox({
      // marginTop={1} → one blank row then welcome.
      text(""),
      hbox({ text(welcome) | bold }) | ftxui::center,
      text(""),
      clawd(),
      ftxui::vbox({
          text(truncate_str(model_line_full, left_width - 2))
              | dim | color(kMuted),
          text(truncate_str(cwd_line_full, left_width - 2))
              | dim | color(kMuted),
      }) | ftxui::center,
  }) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, left_width)
     | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 9);

  // --- Vertical divider (TS L414-417):
  //   <Box height="100%" borderStyle="single" borderColor="claude"
  //        borderDimColor borderTop/bottom/left=false />
  // This is a single full-height vertical rule in the accent colour (or a
  // dimmed variant). We approximate with 9 rows of "│" in kClaude, sized to
  // the minimum of the left panel (GREATER_THAN 9).
  Element divider =
      vbox(Elements(9, text("\xE2\x94\x82") | color(kClaude)))
      | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kDividerWidth)
      | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 9);

  // --- Feed column. ---
  Element feed;
  if (feed_column_override) {
    feed = std::move(feed_column_override);
  } else if (!feeds.empty()) {
    auto [fc, _aw] = detail::RenderFeedColumn(std::move(feeds), right_width);
    feed = std::move(fc);
  } else {
    // TS REF: LogoV2.tsx L421 — 4-branch feed priority chain:
    //   showOnboarding       → [ProjectOnboarding, RecentActivity]
    //   showGuestPassesUpsell → [RecentActivity, GuestPasses]
    //   showOverageCreditUpsell → [RecentActivity, OverageCredit]
    //   (default)            → [RecentActivity, What'sNew]
    //
    // Priority order: onboarding > guest passes > overage credit > default.
    // When the caller provides explicit `feeds`, those are used as-is (the
    // !feeds.empty() branch above). This else-branch fires only when no
    // feeds were supplied, and we resolve the priority chain from the
    // option flags.
    std::vector<FeedConfig> resolved_feeds;

    if (o.show_onboarding) {
      // Branch 1: Project onboarding takes highest priority.
      // TS createProjectOnboardingFeed(getSteps()) + createRecentActivityFeed().
      FeedConfig onboarding;
      onboarding.title = "Getting started";
      onboarding.lines = {
        FeedLine{ .text = "Ask Claude a question to start a conversation",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "Use /help to see available commands",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "Paste images into the prompt with Ctrl+V",
                  .timestamp = std::nullopt },
      };
      onboarding.footer = "Dismissed automatically after first message";
      resolved_feeds.push_back(std::move(onboarding));

      FeedConfig recent;
      recent.title = "Recent activity";
      recent.empty_message = "(no recent activity yet — engine wiring pending)";
      resolved_feeds.push_back(std::move(recent));
    } else if (o.show_guest_passes) {
      // Branch 2: Guest passes upsell.
      // TS createRecentActivityFeed(activities) + createGuestPassesFeed().
      FeedConfig recent;
      recent.title = "Recent activity";
      recent.empty_message = "(no recent activity yet — engine wiring pending)";
      resolved_feeds.push_back(std::move(recent));

      FeedConfig guest;
      guest.title = "Guest passes";
      guest.lines = {
        FeedLine{ .text = "You have 3 guest passes available",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "Share Claude with teammates at /passes",
                  .timestamp = std::nullopt },
      };
      guest.footer = "Use /passes to manage invitations";
      resolved_feeds.push_back(std::move(guest));
    } else if (o.show_overage_credit) {
      // Branch 3: Overage credit upsell.
      // TS createRecentActivityFeed(activities) + createOverageCreditFeed().
      FeedConfig recent;
      recent.title = "Recent activity";
      recent.empty_message = "(no recent activity yet — engine wiring pending)";
      resolved_feeds.push_back(std::move(recent));

      FeedConfig overage;
      overage.title = "Usage alert";
      overage.lines = {
        FeedLine{ .text = "Nearing monthly credit limit",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "Visit /billing to see usage details",
                  .timestamp = std::nullopt },
      };
      overage.footer = "Upgrades available at /billing";
      resolved_feeds.push_back(std::move(overage));
    } else {
      // Branch 4 (default): Recent activity + What's new.
      // TS createRecentActivityFeed(activities) + createWhatsNewFeed(changelog).
      FeedConfig recent;
      recent.title = "Recent activity";
      recent.empty_message = "(no recent activity yet — engine wiring pending)";
      resolved_feeds.push_back(std::move(recent));

      FeedConfig whats_new;
      whats_new.title = "What's new";
      whats_new.lines = {
        // TS REF: changelog placeholder lines — no timestamps on default feed
        FeedLine{ .text = "Paste images with Ctrl+V directly into the prompt",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "New logo modes: compact, condensed, horizontal",
                  .timestamp = std::nullopt },
        FeedLine{ .text = "Sandboxing bash commands with /sandbox toggle",
                  .timestamp = std::nullopt },
      };
      whats_new.footer = "See full changelog at /changelog";
      resolved_feeds.push_back(std::move(whats_new));
    }

    auto [fc, _aw] = detail::RenderFeedColumn(std::move(resolved_feeds), right_width);
    feed = std::move(fc);
  }
  feed = std::move(feed)
       | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, right_width);

  // --- Inner row: paddingX={1} gap={1} approximated. ---
  // gap=1 → one space between panels.
  Element inner_row = hbox({
      text(" "),  // paddingX={1} left
      std::move(left_panel),
      text(" "),  // gap={1}
      std::move(divider),
      text(" "),  // gap={1}
      std::move(feed),
      text(" "),  // paddingX={1} right
  });

  // Wrap inner_row in window() with the borderTitle — equivalent of Ink's
  // borderStyle=round + borderColor=claude + borderText.
  Element outer = ftxui::window(std::move(border_title), std::move(inner_row))
                | color(kClaude);

  return { std::move(outer), left_width, right_width };
}

// --------------------------------------------------------------------
// §7  Top-level render_logo_v2 dispatch
// --------------------------------------------------------------------
// Complete faithful port of TS LogoV2.tsx render return paths.
//
// Dispatch:
//   1. is_condensed_mode → CondensedLogo + aggregatedNoticeStack
//   2. else layout_mode_from_cols(term_cols) → Compact or Horizontal
//        card + aggregatedNoticeStack
struct LogoV2Result {
  Element root;                      // combined vbox(card, notices)
  Element header;                    // just the card / condensed header
  Elements notices;                 // just the aggregated notice rows
  LogoLayoutMode mode;
  // Geometry hints (populated only for Horizontal mode; 0 otherwise).
  int left_width  = 0;
  int right_width = 0;
};

[[nodiscard]] inline auto RenderLogoV2(const LogoV2Options& o,
                                       int term_cols,
                                       std::vector<FeedConfig> feeds = std::vector<FeedConfig>(),
                                       Element feed_column_override = Element())
    -> LogoV2Result {
  if (o.is_condensed_mode) {
    // --- CONDENSED path (L179-247 TS LogoV2.tsx) ---
    cc::ui::logo::LogoDisplayData inner;
    inner.version            = o.version;
    inner.cwd                = o.cwd;
    inner.billing_type       = o.billing_type;
    inner.agent_name         = o.agent_name;
    inner.model_display_name = o.model_display_name;

    Element header = cc::ui::logo::RenderCondensedLogoElement(inner, term_cols);
    Elements notices = RenderNoticeStackAggregated(o);

    Elements combined;
    combined.reserve(1 + notices.size());
    combined.push_back(std::move(header));
    for (auto& n : notices) combined.push_back(std::move(n));
    return { vbox(std::move(combined)), /*header=*/{}, std::move(notices),
             LogoLayoutMode::Condensed };
  }

  // --- CARD path ---
  const LogoLayoutMode mode = layout_mode_from_cols(term_cols);
  Element card;
  struct W { int l = 0, r = 0; };
  W widths;
  if (mode == LogoLayoutMode::Compact) {
    // Compact mode: 3rd param is (unused) is_new_user compatibility flag.
    card = RenderCompactLayout(o, term_cols, 0);
  } else {
    auto [h, lw, rw] = RenderHorizontalLayout(o, term_cols, 0,
                                              std::move(feeds),
                                              std::move(feed_column_override));
    card = std::move(h);
    widths = {lw, rw};
  }
  Elements notices = RenderNoticeStackAggregated(o);
  Elements combined;
  combined.reserve(1 + notices.size());
  combined.push_back(std::move(card));
  for (auto& n : notices) combined.push_back(std::move(n));
  return { vbox(std::move(combined)), std::move(card), std::move(notices), mode,
           widths.l, widths.r };
}

// Convenience overload: returns just the combined-root Element (the
// common case in repl_screen::RenderWelcomeHeader).
[[nodiscard]] inline auto render_logo_v2(const LogoV2Options& o,
                                          int term_cols = 120,
                                          std::vector<FeedConfig> feeds = {})
    -> Element {
  return RenderLogoV2(o, term_cols, std::move(feeds)).root;
}

/// Thin sticky logo header bar (1 line).  Mirrors TS Messages.tsx
/// LogoHeader which sits ABOVE VirtualMessageList and stays visible
/// even when the welcome card scrolls off due to pin-to-bottom.
///
/// Visual: "◆ Claude Code  v0.0.0  ·  ModelName"  (left-aligned, dim)
[[nodiscard]] inline Element render_logo_header_bar(
    std::string_view version,
    std::string_view model_display_name,
    int /*term_cols*/ = 120)
{
    using namespace ftxui;
    Elements parts;
    parts.push_back(text("\xe2\x97\x86 ") | color(Color::Cyan));  // ◆ diamond
    parts.push_back(text("Claude Code") | bold);
    if (!version.empty()) {
        parts.push_back(text("  v" + std::string(version)) | dim);
    }
    if (!model_display_name.empty()) {
        parts.push_back(text("  \xc2\xb7 ") | dim);  // · separator
        parts.push_back(text(std::string(model_display_name)) | dim);
    }
    return hbox(std::move(parts)) | size(HEIGHT, EQUAL, 1);
}

} // namespace cc::ui::logo_v2
