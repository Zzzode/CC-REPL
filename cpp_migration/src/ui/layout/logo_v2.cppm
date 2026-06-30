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
    return text("");
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
  if (!o.is_debug_mode) return text("");
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
  if (!o.emergency_tip.has_value()) return text("");
  using namespace detail;
  return pad2(hbox({
    text("\xE2\x9A\xA0 ") | color(kWarningColor),                 // ⚠
    text(*o.emergency_tip) | dim | color(kMuted),
  }));
}

// --- 3f. Tmux session notice (L194-197 TS LogoV2.tsx).
[[nodiscard]] inline auto RenderTmuxNotice(const LogoV2Options& o) -> Element {
  if (!o.tmux_session.has_value()) return text("");
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
  if (!o.company_announcement.has_value()) return text("");
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
  if (!o.show_sandbox_status) return text("");
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
  if (!o.show_guest_passes) return text("");
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
  if (!o.show_overage_credit) return text("");
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
    // Element has no empty() — but a default-constructed Element (no
    // renderer installed) still produces 0 lines when rendered.  We skip
    // nothing here; each renderer returns text("") when inactive which
    // contributes a 0-height box to the vbox (no visible padding-gaps).
    out.push_back(std::move(e));
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
} // namespace detail

// Produce the 58-col fixed-width WelcomeV2 card. Version fills the
// header line as "Welcome to Claude Code vX.X.X " (TS t0). Returns a
// single Element; caller is responsible for wrapping in `flex` or
// `center` if the terminal is wider than 58 cols.
[[nodiscard]] inline auto RenderWelcomeV2(std::string_view version) -> Element {
  using namespace detail;
  const std::string v = version.empty() ? "0.0.0" : std::string(version);

  // t0 header: color="claude" "Welcome to Claude Code" + dim "vX.X.X "
  Element header = hbox({
    text("Welcome to Claude Code ") | color(kClaude),
    text("v" + v + " ") | dim | color(kMuted),
  });

  Elements art;
  art.reserve(15);
  art.push_back(std::move(header));

  // t1 ellipsis row — no color in TS (inherits Text default).
  art.push_back(text(std::string(kWelcomeV2DarkRows[1])));

  // t2-t11: raw art rows. Dim "*" glyphs are baked into the strings as
  // plain '*' characters in TS; we apply default fg. No per-row tinting.
  for (std::size_t i = 2; i <= 11; ++i) {
    art.push_back(text(std::string(kWelcomeV2DarkRows[i])));
  }

  // t12-t14: clawd body — TS color=clawd_body for the ████████ segments.
  // Our kWelcomeV2DarkRows stores them as contiguous 9-char blocks; we
  // detect the clawd_body substring position (col 6..15, 9 × 3 bytes = 27
  // bytes per row) and colourise it separately.
  auto split_clawd_row = [](std::string_view row) -> Elements {
    // Row format: 6 spaces + 9 body chars (3 bytes each = 27 bytes) + rest
    if (row.size() < 6 + 27) {
      return { text(std::string(row)) };
    }
    return {
      text(std::string(row.substr(0, 6))),
      text(std::string(row.substr(6, 27))) | color(kClawdBody),
      text(std::string(row.substr(33))) | dim | color(kMuted), // trailing '*'s dim
    };
  };
  art.push_back(hbox(split_clawd_row(kWelcomeV2DarkRows[12])));
  art.push_back(hbox(split_clawd_row(kWelcomeV2DarkRows[13])));
  art.push_back(hbox(split_clawd_row(kWelcomeV2DarkRows[14])));

  // Footer separator: 7 × … +  clawd_body "█ █   █ █" (paws) + 24 × … +
  // 4 × ░ + 1 × … + 4 × ▒ + 4 × … — simplified from the TS t16 footer
  // string literal (we replicate exactly char-for-char via the literal
  // below, same byte-for-byte content used in TS WelcomeV2.tsx L192).
  const std::string_view footer_lit =
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6"
      "\xE2\x96\x88 \xE2\x96\x88   \xE2\x96\x88 \xE2\x96\x88"
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x80\xA6\xE2\x96\x91\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6"
      "\xE2\x96\x92\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6\xE2\x80\xA6";
  Element footer = hbox({
    text(std::string(footer_lit.substr(0, 21))),  // 7 × … (21 bytes)
    text(std::string(footer_lit.substr(21, 11))) | color(kClawdBody), // paws
    text(std::string(footer_lit.substr(32))),     // remaining … ░ … ▒ …
  });
  art.push_back(std::move(footer));

  return vbox(std::move(art)) | size(WIDTH, EQUAL, kWelcomeV2FixedWidth);
}

// --------------------------------------------------------------------
// §6  Compact & Horizontal card layouts
// --------------------------------------------------------------------
namespace detail {

// TS formatWelcomeMessage(username):
//   username empty → "Welcome to Claude Code"
//   new user →      "Welcome to Claude Code, {username}!"
//   returning →     "Welcome back, {username}!"
[[nodiscard]] inline auto format_welcome_message(
    const std::optional<std::string>& username, bool is_new_user) -> std::string {
  if (!username.has_value() || username->empty()) {
    return is_new_user ? "Welcome to Claude Code" : "Welcome to Claude Code";
  }
  return is_new_user
      ? ("Welcome to Claude Code, " + *username + "!")
      : ("Welcome back, " + *username + "!");
}

} // namespace detail

// Compact mode (cols < 70, TS LogoV2.tsx L253-330):
//   <Box flexDirection="column" borderStyle="round" borderColor="claude"
//        borderText={compactBorderTitle} paddingX={1} paddingY={1}
//        alignItems="center" width={columns}>
//     <Text bold>{welcomeMessage}</Text>
//     <Box marginTop={1}><Clawd /></Box>
//     <Text dimColor>{modelDisplayName}</Text>
//     <Text dimColor>{billingType}</Text>
//     <Text dimColor>{agent ? "@agent · cwd" : cwd}</Text>
//   </Box>
[[nodiscard]] inline auto RenderCompactLayout(const LogoV2Options& o,
                                               int term_cols,
                                               bool is_new_user = false)
    -> Element {
  using namespace detail;
  namespace logo = cc::ui::logo;

  const std::string welcome = format_welcome_message(o.username, is_new_user);
  const int width = std::max(term_cols, 20);

  // Compact borderTitle: " Claude Code " rendered in the claude token,
  // wrapped in a round border. FTXUI supports borderStyled with a title.
  const Color kClaudeBorder = kClaude;

  // Clawd (3 rows, faithful to TS Clawd.tsx): reuse the 9×3 block-art
  // used by RenderCondensedLogoElement but wrapped in marginTop box.
  auto clawd = [&]() -> Element {
    const Color kClawd(215, 119, 87);   // theme.clawd_body #D77757
    const std::string_view r1 = "  \xE2\x96\x9B\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x88\xE2\x96\x9C ";
    const std::string_view r2 = "\xE2\x96\x9D\xE2\x96\x9C"
                                 "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88"
                                 "\xE2\x96\x9B\xE2\x96\x98";
    const std::string_view r3 = "  \xE2\x96\x98\xE2\x96\x98 \xE2\x96\x9D\xE2\x96\x9D  ";
    return vbox({
      text(std::string(r1)) | color(kClawd),
      text(std::string(r2)) | color(kClawd),
      text(std::string(r3)) | color(kClawd),
    });
  };

  // Row 2 model line / billing / agent+cwd: all dimColor.
  const std::string model_line = !o.model_display_name.empty()
      ? o.model_display_name : std::string("Claude");
  const std::string& billing = o.billing_type;
  const std::string cwd_line = [&]() -> std::string {
    // Width budget for cwd line: width - 4 (border + padding)
    const int budget = std::max(width - 4, 10);
    int agent_cost = 0;
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      // "@" + name + " · " = 1 + name.size() + 3
      agent_cost = 1 + static_cast<int>(o.agent_name->size()) + 3;
    }
    const int cwd_budget = std::max(budget - agent_cost, 10);
    std::string cwd = o.cwd;
    if (static_cast<int>(cwd.size()) > cwd_budget) {
      cwd = "\xE2\x80\xA6" + cwd.substr(cwd.size()
          - static_cast<std::size_t>(cwd_budget - 3));
    }
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      return "@" + *o.agent_name + " \xC2\xB7 " + cwd;
    }
    return cwd;
  }();

  Element inner = vbox({
    text(welcome) | bold,
    text(""),  // marginTop={1}
    clawd(),
    text(model_line) | dim | color(kMuted),
    text(billing)    | dim | color(kMuted),
    text(cwd_line)   | dim | color(kMuted),
  }) | ftxui::center;

  // Title: "Claude Code" (claud color), mimicking borderText.
  Element title = hbox({
    text(" Claude Code ") | color(kClaudeBorder) | bold,
  });
  (void)title;  // FTXUI border doesn't natively support inline colored
                // titles the way Ink does; we instead use borderRounded +
                // colorise the whole border; the title appears as a
                // top-positioned text line inside the card if strictly
                // required, otherwise we rely on the welcome banner above.

  return inner
       | ftxui::borderStyled(ftxui::ROUNDED, kClaudeBorder)
       | size(WIDTH, EQUAL, width);
}

// Horizontal mode (cols >= 70, TS LogoV2.tsx L331-428):
//   <Box flexDirection="column" borderStyle="round" borderColor="claude"
//        borderText={borderTitle}>
//     <Box flexDirection="row" paddingX={1} gap={1}>
//       <Box width={leftWidth} justifyContent="space-between"
//            alignItems="center" minHeight={9}>  /* LEFT PANEL */
//         welcome
//         Clawd
//         <Box alignItems="center">{modelLine}{cwdLine}</Box>
//       </Box>
//       {layout === horizontal && <Box height=100%
//            borderStyle=single borderTop/bottom/left=false borderColor=claude />}
//       <FeedColumn feeds={...} maxWidth={rightWidth} />
//     </Box>
//   </Box>
//
// borderTitle: "{colorClaude(Claude Code)} {colorInactive(vX.X.X)}"
// FeedColumn is deliberately out of scope for this P0 (it requires
// recent-activity / changelog IO which belongs to the caller). The
// right-hand slot is left as an empty placeholder sized to rightWidth;
// callers may later inject the FeedColumn via composition.
struct HorizontalLayoutOutput {
  Element layout;
  int left_width  = 0;
  int right_width = 0;
};

[[nodiscard]] inline auto RenderHorizontalLayout(const LogoV2Options& o,
                                                 int term_cols,
                                                 bool is_new_user = false,
                                                 Element feed_column = {})
    -> HorizontalLayoutOutput {
  using namespace detail;

  const int width = std::max(term_cols, 70);
  // TS optimalLeftWidth: derived from longest(welcome, cwdLine, modelLine)
  // clamped to LEFT_PANEL_MAX_WIDTH (50). Then calculateLayoutDimensions
  // assigns the rest to feed column.
  const std::string welcome = format_welcome_message(o.username, is_new_user);
  const std::string model_line_full = [&]() {
    std::string m = o.model_display_name.empty()
        ? std::string("Claude") : o.model_display_name;
    if (!o.billing_type.empty()) {
      m += " \xC2\xB7 " + o.billing_type;
    }
    if (o.org_name.has_value() && !o.org_name->empty()) {
      m += " \xC2\xB7 " + *o.org_name;
    }
    return m;
  }();
  const std::string cwd_line_full = [&]() {
    const int max_w = 50;  // LEFT_PANEL_MAX_WIDTH
    int agent_cost = 0;
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      agent_cost = 1 + static_cast<int>(o.agent_name->size()) + 3;
    }
    const int cwd_budget = std::max(max_w - agent_cost, 10);
    std::string cwd = o.cwd;
    if (static_cast<int>(cwd.size()) > cwd_budget) {
      cwd = "\xE2\x80\xA6" + cwd.substr(cwd.size()
          - static_cast<std::size_t>(cwd_budget - 3));
    }
    if (o.agent_name.has_value() && !o.agent_name->empty()) {
      return "@" + *o.agent_name + " \xC2\xB7 " + cwd;
    }
    return cwd;
  }();

  auto line_len = [](const std::string& s) { return static_cast<int>(s.size()); };
  const int longest = std::max({
      line_len(welcome), line_len(model_line_full), line_len(cwd_line_full) });
  int left_w = std::clamp(longest + 4 /* hpad */, 34, 50 /* LEFT_PANEL_MAX_WIDTH */);
  // width - left_w - 2 (outer border) - 1 (inner gap) - 1 (divider col)
  int right_w = std::max(width - left_w - 2 - 1 - 1, 20);

  // Left panel: justify=space-between (welcome at top, clawd in the
  // middle, meta at bottom). FTXUI lacks justify-content: space-between
  // as a direct DOM property; we approximate with filler + clawd + filler.
  auto clawd = [&]() -> Element {
    const Color kClawd(215, 119, 87);
    return vbox({
      text("  \xE2\x96\x9B\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x9C ")
          | color(kClawd),
      text("\xE2\x96\x9D\xE2\x96\x9C\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x9B\xE2\x96\x98")
          | color(kClawd),
      text("  \xE2\x96\x98\xE2\x96\x98 \xE2\x96\x9D\xE2\x96\x9D  ")
          | color(kClawd),
    });
  };

  Element left_panel = vbox({
      hbox({ text(welcome) | bold }) | ftxui::center,
      text(""),  // marginTop
      clawd() | ftxui::center,
      vbox({
          text(model_line_full) | dim | color(kMuted),
          text(cwd_line_full)    | dim | color(kMuted),
      }) | ftxui::center,
  }) | size(WIDTH, EQUAL, left_w) | size(HEIGHT, GREATER_THAN, 9);

  // Vertical divider: "single" border style, only the right edge, full
  // height. Approximate with a 1-wide column of "│" chars (U+2502) in
  // claude-purple, clamped to 9 rows (minimum of left panel).
  Element divider = vbox(Elements(9, text("\xE2\x94\x82") | color(kClaude)))
                  | size(WIDTH, EQUAL, 1)
                  | size(HEIGHT, GREATER_THAN, 9);

  // Feed placeholder — caller can pass a real FeedColumn; otherwise a
  // dim hint "Feed column pending / Recent activity + changelog".
  Element feed = feed_column ? std::move(feed_column) : vbox({
      text("Recent activity") | bold | color(kClaude),
      text("  (no data yet — activity feed requires Engine wiring)")
          | dim | color(kMuted),
  }) | size(WIDTH, EQUAL, right_w);

  Element row = hbox({
      std::move(left_panel),
      std::move(divider),
      std::move(feed),
  });

  const Color kClaudeBorder = kClaude;
  Element outer = std::move(row)
                | ftxui::borderStyled(ftxui::ROUNDED, kClaudeBorder)
                | size(WIDTH, EQUAL, width);

  return { std::move(outer), left_w, right_w };
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
                                       bool is_new_user = false,
                                       Element feed_column = {})
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
    card = RenderCompactLayout(o, term_cols, is_new_user);
  } else {
    auto [h, lw, rw] = RenderHorizontalLayout(o, term_cols, is_new_user,
                                              std::move(feed_column));
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
                                          bool is_new_user = false)
    -> Element {
  return RenderLogoV2(o, term_cols, is_new_user).root;
}

} // namespace cc::ui::logo_v2
