// repl_screen_layout.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). RouteDialog and RenderReplScreen - terminal probe, unseen divider, the
// FullscreenLayout slot composition (scrollable/header/bottom) with both
// sticky/pill lambdas, and final dialog-queue layering.
//
// Phase A (#184957): textual FTXUI GMF headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <ftxui/dom/elements.hpp>

module cc.ui.screens.repl_screen;

import std;

import cc.ui.chrome.ink_utils;
import cc.utils.terminal_helpers;
import cc.ui.chrome.fullscreen_layout;
import cc.ui.foundation.logo_v2;
import cc.ui.features.teams.live_teammates;
import cc.ui.prompt.prompt_input_footer;
import cc.ui.prompt.voice_indicator;
import cc.ui.messages.virtual_list;
import cc.ui.visual.markdown;

namespace cc::ui::repl_screen {
using namespace ftxui;

/// Top-level dialog router: ReplMode -> overlay Element.
/// Panel modes (Normal / Tasks / Teams / Help / QuickOpen) return nullopt.
/// Priority matches TS getFocusedInputDialog() (REPL.tsx:2013):
///   Exit > message-selector > sandbox > permissions/hook/elicit >
///   cost/idle/ultraplan > onboarding > recs > panels.
[[nodiscard]] std::optional<Element> RouteDialog(
    ReplMode m, const ReplScreenState& s) {
    (void)s;
    // M7: All legacy ReplMode dialog routing is retired.  Dialogs now flow
    // exclusively via the 4-slot DialogQueue with priority
    // Standalone > Modal > Overlay > Bottom.  Any remaining ReplMode-driven
    // chrome (IdleReturn return-from-idle prompt, onboarding, etc.) is
    // rendered by the owning slot in dialog_queue_render.  This stub
    // preserves the file's historical RouteDialog() API so downstream
    // callers (RenderReplScreen) don't need another rewrite.
    switch (m) {
      default: break; }
    return std::nullopt;
}

// =========================================================
// Full layout composition
// =========================================================

/// M1: Composed via the FullscreenLayout slot-system (faithful port of TS
/// FullscreenLayout.tsx).  The previously-flat top-to-bottom vbox is now
/// slotted:
///   scrollable slot (flexGrow region) =
///     [WelcomeHeader (fresh session)] | Messages | Spinner | Tasks/Teams
///   bottom slot (pinned, flexShrink=0) =
    ///     PromptInput | Footer
///   modal slot (dbox overlay, bottom-anchored) =
///     RouteDialog() when non-null (with MODAL_TRANSCRIPT_PEEK peek)
///   overlay slot =
///     (reserved — engine wires PermissionRequest here in a future milestone;
///      currently permission flows through RouteDialog as the modal slot,
///      matching how it rendered before.)
///
    /// All existing render functions are PRESERVED (RenderWelcomeHeader,
    /// RenderMessages, RenderSpinner, RenderPromptInput,
/// RouteDialog) — only HOW they are composed changed.  Visual order is
/// preserved: messages scroll above, status/prompt pinned below.
///
/// Fix #1: welcome header atop the list on a fresh session.
    /// Fix #7: StatusLine lives inside the prompt footer, matching TS.
/// Fix #11: terminal size probed once per frame for adaptive clamping.
[[nodiscard]] Element RenderReplScreen(ReplScreenState& s,
    // GAP 3: msg-system-api-error-retry — retry callback threaded through
    // to RenderMessages so SystemAPIError rows can render a working Retry pill.
    std::function<void()> on_retry,
    // P2 gap api-error-retry: clear-session callback threaded through to
    // RenderMessages for session-expired error cards.
    // TS REF: SystemAPIErrorMessage.tsx onClearSession.
    std::function<void()> on_clear_session,
    // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — shared
    // StreamingMarkdown instance for the streaming-text tail row.
    ::cc::ui::StreamingMarkdown* streaming_md) {
    // Probe terminal size once per frame for adaptive layout (fix #11).
    auto [term_cols, term_rows] = cc::ui::ink_utils::query_terminal_size();
    if (term_cols <= 0) term_cols = 80;
    if (term_rows <= 0) term_rows = 24;
    s.viewport_height_lines = std::max(1, term_rows - 5);

    // Spinner frame tick: monotonically increments per render call so the
    // tool-use header spinner animates.  (The interactive ToolUseMessage
    // component also drives its own counter; this feeds the static render
    // path used by message_list row dispatch.)
    static int spinner_frame = 0;
    ++spinner_frame;

    // ── UnseenDivider computation (TS: useUnseenDivider) ────────────────
    // When the user has scrolled away from bottom, compute the in-transcript
    // "N new messages" divider anchor + count.  Cleared on repin by
    // ScrollTranscript / on_pill_click (divider_index.reset()).
    if (s.divider_index.has_value()) {
        s.unseen_divider = ComputeUnseenDivider(s);
        if (s.unseen_divider.has_value()) {
            s.unseen_message_count = static_cast<int>(s.unseen_divider->count);
            s.pill_visible = true;
        }
    } else {
        s.unseen_divider.reset();
    }

    namespace fl = cc::ui::layout::fullscreen;
    fl::FullscreenLayoutSlots slots;
    slots.term_cols = term_cols;
    slots.term_rows = term_rows;

    // ── scrollable slot (flexGrow region) ───────────────────────────────
    // Builds the same top→bottom order the old flat vbox had for the
    // message transcript area: welcome header (fresh session), messages.
    // Spinner is NOT a scroll row — it lives in the pinned chrome between
    // the messages list and the prompt input (TS BriefSpinner marginTop=1).
    //
    // TS PARITY (Fix 2026-07-02): Logo/welcome lives INSIDE the scrollable
    // area, not in a pinned header.  In TS Messages.tsx the LogoHeader is a
    // thin 1-row bar; the full welcome card (LogoV2 condensed / compact) is
    // rendered inside the VirtualMessageList scrollback.  Putting it here
    // means: (a) blank space appears BELOW messages, not between logo and
    // messages; (b) as messages arrive and pin-to-bottom engages, the logo
    // scrolls out of the visible viewport naturally.
    Elements L;
    Elements scroll_rows; scroll_rows.reserve(2);
    const auto visible_messages = BuildVisibleMessages(s);

    // ── Welcome / logo card (passed as leading element inside yframe) ───
    // TS PARITY (2026-07-03 fix): LogoV2 welcome card is the first element
    // inside the VirtualMessageList scrollback.  It is ALWAYS present in
    // the scroll content — when messages overflow the viewport and
    // pin-to-bottom engages, the logo scrolls above the visible window but
    // is reachable by scrolling up.
    //
    // EXCEPTION: when a local command overlay (/skills, /help, etc.) is
    // active with no real conversation messages (s.messages empty), we
    // skip the logo so the command output has full viewport space.  This
    // matches TS where command overlays are not "real" transcript entries.
    Elements logo_leading;
    const bool has_real_messages = !s.messages.empty();
    const bool has_command_overlay = s.active_local_jsx_command;
    if (has_real_messages || !has_command_overlay) {
        logo_leading.push_back(
            RenderWelcomeHeader(s, spinner_frame, term_cols)
            | size(WIDTH, EQUAL, term_cols));
    }

    // ── Messages yframe (logo prepended INSIDE, fills scrollable) ───────
    // render_messages_list_view returns yframe | vscroll_indicator | flex
    // with the logo card as its first scroll child.  Pin-to-bottom logic
    // in messages_list.cppm keeps short content top-aligned (no blank
    // space above messages), and scrolls to bottom only when content
    // exceeds viewport.
    scroll_rows.push_back(RenderMessages(
        visible_messages, s.selected_message_idx,
        s.viewport_height_lines, s.scroll_offset,
        s.scroll_pinned_to_bottom, spinner_frame,
        s.unseen_divider,
        std::move(logo_leading),
        s.is_brief_mode,
        s.expanded_keys,
        s.is_transcript_mode,
        s.show_all_in_transcript,
        // TS REF: Messages.tsx L382-389  isStreamingThinkingVisible.
        // Threaded from app.cppm's is_streaming_thinking_visible() helper.
        s.streaming_thinking_globally_visible,
        on_retry,
        // P2 gap api-error-retry: thread on_clear_session for session-expired
        // error cards.
        on_clear_session,
        // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — thread
        // the shared StreamingMarkdown instance to the messages list.
        streaming_md));
    // Spinner lives in the chrome BETWEEN messages list and prompt input
    // (TS BriefSpinner marginTop=1, NOT a message row inside scroll content).
    Element spinner_chrome = text("");
    if (s.spinner_mode != SpinnerMode::Hidden)
        spinner_chrome = RenderSpinner(s.spinner_mode, s.spinner_verb,
                                       s.spinner_tip, spinner_frame);
    // M7.5: Panel views (Tasks/Teams/Help/Settings/About/QuickOpen) are
    // now rendered as modal dialogs via DialogQueue — no longer inlined
    // in the scrollable slot.
    (void)L;
    slots.scrollable = vbox(std::move(scroll_rows));

    // ── Pinned header (non-scroll) ─────────────────────────────────────
    // TS Messages.tsx has a thin LogoHeader bar above VirtualMessageList
    // that stays visible even when the welcome card scrolls off.  Without
    // this, pin-to-bottom scrolls the full welcome card out of view and
    // the user sees "logo 也没了" (user report 2026-07-04).
    //
    // We render a compact 1-line logo bar here so the app identity is
    // always visible at the top of the terminal.  The full welcome card
    // still lives inside the scrollable area (first child of yframe).
    {
        namespace lv2 = cc::ui::logo_v2;
        const std::string version = s.app_version.empty()
            ? std::string("0.0.0") : s.app_version;
        const std::string model_line = !s.model_display_name.empty()
            ? s.model_display_name
            : s.settings_model;
        slots.header = lv2::render_logo_header_bar(version, model_line, term_cols);
    }

    if (!s.active_local_jsx_command) {
        // ── bottom slot (pinned, flexShrink=0) ──────────────────────────────
        // Chrome order: [spinner (marginTop=1)] → [suggestions overlay?] →
        //               [prompt input] → [footer]
        //
        // Faithful to TS PromptInputFooter structure:
        //   suggestions overlay?  →  prompt input  →  footer (left/right columns)
        //
        // The footer contains StatusLine (optional, user-configurable) +
        // PromptInputFooterLeftSide (mode indicator, tasks, teams, hints) on
        // the left, and bridge/notifications on the right.
        //
        // STABLE HEIGHT: The LeftSide row is always exactly 1 row so scroll
        // content never shifts when hints change.  StatusLine adds a row when
        // present but is conditionally shown only in prompt mode + not short.
        namespace pif = cc::ui::prompt::footer;

        // Build StatusLine options (user-configurable command-driven status).
        //
        // Faithful to TS StatusLine.tsx:
        //   - Configured by settings.statusLine, rendered only in prompt mode
        //     and hidden in short fullscreen layouts
        //   - content comes from executing the user's shell command
        //   - In fullscreen, reserves a row even while loading (stable height)
        //   - Text may contain ANSI escape codes for coloring
        const bool is_fullscreen = cc::utils::is_fullscreen_enabled();
        const bool is_short = is_fullscreen && term_rows < 24;
        const bool status_line_configured =
            s.status_line_enabled && !s.status_line_command.empty();

        // P0-6 builtin statusline: populate fallback data from screen state.
        // When the user's command returns empty (or no command configured),
        // RenderStatusLine() uses this to show folder/git/model/token info.
        pif::BuiltinStatusLineData builtin_data;
        builtin_data.cwd = s.cwd;
        builtin_data.git_branch = s.git_branch;
        // Prefer model_display_name (human-friendly), fall back to model_name.
        builtin_data.model_name = !s.model_display_name.empty()
            ? s.model_display_name
            : s.status_bar.model_name;
        builtin_data.input_tokens = s.status_bar.input_tokens;
        builtin_data.output_tokens = s.status_bar.output_tokens;
        builtin_data.context_token_count = s.status_bar.context_token_count;
        builtin_data.cost_usd = s.status_bar.cost_usd;
        // Context window size: use 200k default; model-specific overrides
        // could be added later from model metadata.
        builtin_data.context_window_size = 200000;

        pif::StatusLineOptions status_line_opts;
        status_line_opts.content = s.status_line_text;
        status_line_opts.builtin = std::move(builtin_data);
        // Show statusline when:
        //   (a) user has a configured statusLine command, OR
        //   (b) builtin data is available (always, since cwd is set)
        // Same mode guards apply: prompt mode, not bash, not short terminal.
        const bool in_prompt_mode =
            s.input_mode == InputMode::Normal &&
            !effective_is_bash(s) &&
            !is_short;
        status_line_opts.should_display = in_prompt_mode &&
            (status_line_configured || !status_line_opts.content.empty() ||
             status_line_opts.builtin.has_value());
        status_line_opts.is_fullscreen = is_fullscreen;
        status_line_opts.padding_x = s.status_line_padding;

        // Map InputMode to footer PromptInputMode.  Text-derived mode takes
        // precedence over state-toggle (TS getInputMode semantics — see
        // effective_is_bash()).
        // NOTE: Both InputMode and pif::PromptInputMode are now the same
        // unified type (cc::ui::common::PromptInputMode), so this is a
        // direct assignment with bash-detection override.
        pif::PromptInputMode footer_mode =
            static_cast<pif::PromptInputMode>(s.input_mode);
        if (effective_is_bash(s)) {
            footer_mode = pif::PromptInputMode::Bash;
        } else if (footer_mode == pif::PromptInputMode::Bash) {
            // State says bash but text doesn't start with '!' — normalize
            // to Normal (consistent with old switch default behavior).
            footer_mode = pif::PromptInputMode::Normal;
        }
        // ── Assemble the bottom slot ──
        // Chrome order: [marginTop gap] → [spinner (marginTop=1)] →
        //               [suggestions overlay?] → [prompt input] → [footer]
        //
        // TS REF: PromptInput.tsx:2244 — marginTop={briefOwnsGap ? 0 : 1} on the
        // outermost container.  In non-brief mode this is a 1-row gap between the
        // scrollback area and the top border of the input box.  We emulate with a
        // leading text("") row.
        L.reserve(5);
        L.push_back(text(""));   // marginTop=1
        if (s.spinner_mode != SpinnerMode::Hidden) {
            L.push_back(hbox({spinner_chrome, filler()}) | flex_shrink);
        }
        // Live teammate strip (TS CoordinatorAgentStatus.tsx AgentLine list):
        // one status + output-tail row per teammate, pinned just above the
        // prompt input. Pure render of state-owned data.
        if (!s.live_teammates.empty()) {
            L.push_back(hbox({
                teams::live::RenderLiveTeammateStrip(s.live_teammates, term_cols),
                filler(),
            }) | flex_shrink);
        }
        if (!s.autocomplete_suggestions.empty()) {
            // TS REF: FullscreenLayout.tsx L591-607 + PromptInputFooter.tsx L124-129
            // In fullscreen mode, suggestions are portaled to FullscreenLayout
            // as a floating overlay (position:absolute bottom:100% opaque:true).
            // In FTXUI we apply overlay styling (background + top border) when
            // is_fullscreen, and pass term_rows for adaptive maxVisibleItems.
            L.push_back(RenderPromptSuggestions(s, term_cols,
                /*is_overlay=*/is_fullscreen,
                /*term_rows=*/term_rows));
        }
        L.push_back(RenderPromptInput(s, term_cols));
        // PromptInputFooter: LeftSide carries mode/tasks/teams via
        // ModeIndicatorOptions; StatusLine is its own nested struct.
        pif::LeftSideOptions left_opts;
        // Pasting hint is visible for 100ms after the last paste batch
        // (TS PASTE_COMPLETION_TIMEOUT_MS = 100).
        if (s.pasting_since) {
            const auto age = std::chrono::steady_clock::now() - *s.pasting_since;
            if (age <= std::chrono::milliseconds(100)) {
                left_opts.is_pasting = true;
            } else {
                s.pasting_since.reset();
            }
        }
        // Idle Ctrl+C double-press footer ("Press <key> again to exit"),
        // projected from the app-layer ExitHandler. Expiry is event-driven
        // exactly like the pasting hint (no ticker).
        // TS REF: PromptInputFooterLeftSide.tsx:150
        //   `Press {exitMessage.key} again to exit` — with key "Ctrl-C"
        //   RenderLeftSide composes the exact TS string.
        if (s.exit_message_until) {
            if (std::chrono::steady_clock::now() <= *s.exit_message_until) {
                left_opts.exit_message_show = true;
                left_opts.exit_message_key = s.exit_message_key;
            } else {
                s.exit_message_until.reset();
            }
        }
        left_opts.mode_indicator.mode                 = footer_mode;
        left_opts.mode_indicator.permission_mode      = s.permission_mode;
        left_opts.mode_indicator.background_task_count = s.background_task_count;
        left_opts.mode_indicator.teammate_count        = s.teammate_count;
        left_opts.mode_indicator.teams_selected        = s.teams_footer_selected;
        // Transcript/brief mode pills (TS REF: Messages.tsx isTranscriptMode + isBriefOnly).
        left_opts.mode_indicator.is_transcript_mode    = s.is_transcript_mode;
        left_opts.mode_indicator.is_brief_mode         = s.is_brief_mode;
        if (status_line_opts.should_display) {
            left_opts.mode_indicator.show_hint = false;
        }
        pif::FooterOptions footer_opts;
        footer_opts.status_line = std::move(status_line_opts);
        footer_opts.left_side   = std::move(left_opts);
        footer_opts.is_fullscreen = is_fullscreen;
        footer_opts.is_narrow = term_cols < 80;

        // Bridge status pill (TS REF: PromptInputFooter.tsx BridgeStatusIndicator
        // + bridgeStatusUtil.ts:124 getBridgeStatus).
        if (s.bridge_enabled) {
            namespace bs = cc::ui::prompt::footer;
            bs::BridgeOptions bopt;
            // Priority: reconnecting > connected(session) > connected,
            // mirroring getBridgeStatus (failed is surfaced via notification).
            if (s.bridge_reconnecting) {
                bopt.status = bs::BridgeStatus::Reconnecting;
            } else if (s.bridge_connected || s.bridge_session_active) {
                bopt.status = bs::BridgeStatus::Connected;
            } else {
                bopt.status = bs::BridgeStatus::Disconnected;
            }
            bopt.explicit_remote = s.bridge_explicit_remote;
            bopt.selected = s.bridge_selected;
            footer_opts.bridge = std::move(bopt);
        }

        // P1 Footer notifications — populate from ReplScreenState
        // TS REF: src/components/PromptInput/Notifications.tsx
        {
            auto& nd = footer_opts.notification;
            nd.api_key_status = s.api_key_status;
            // Voice indicator projection (TS Notifications.tsx
            // NotificationContent early-return).  Elapsed seconds are
            // wall-clock from the Processing transition anchor, matching
            // TS ProcessingShimmer elapsedSec = time / 1000.
            nd.voice_state = s.voice_footer_status;
            nd.voice_enabled = s.voice_enabled;
            nd.voice_processing_elapsed_sec = 0.0;
            if (s.voice_footer_status ==
                    cc::ui::prompt::FooterVoiceState::Processing &&
                s.voice_processing_since) {
                nd.voice_processing_elapsed_sec =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() -
                        *s.voice_processing_since).count();
            }
            nd.is_remote = s.is_remote_session;
            nd.debug_mode = s.debug_mode;
            nd.verbose = s.verbose;
            nd.token_usage = s.status_bar.context_token_count;
            nd.is_overage_mode = s.show_overage_credit_upsell;
            nd.ide.connected = s.ide_connected;
            nd.ide.file_path = s.ide_file_path;
            nd.ide.selected_lines = s.ide_selected_lines;
            nd.dynamic_text = s.footer_dynamic_text;
            nd.dynamic_color = s.footer_dynamic_color;

            // P1: Advance the notification queue carousel — this is the
            // timer-based rotation through queued items.  Called here
            // (event-driven, on each render) rather than a constant ticker.
            // TS REF: src/context/notifications.tsx processQueue()
            namespace pif = cc::ui::prompt::footer;
            (void)pif::QueueAdvance(s.footer_notification_queue);
            nd.queue = s.footer_notification_queue;
        }

        // M4 faithful: outer chrome.
        //   * Clipboard image hint — stays nullopt until the engine wires up
        //     platform clipboard-image detection; no visual regression while
        //     empty.
        // NOTE: TS upstream does NOT render a brand pill in the footer
        // (PromptInputFooter.tsx has zero occurrences of "LOOM" /
        // "Loom" text).  Branding is rendered by CondensedLogo only
        // in the top header.
        L.push_back(pif::RenderPromptInputFooter(footer_opts));

        slots.bottom = vbox(std::move(L)) | flex_shrink;
    }

    // M7: Standalone slot (trust dialog, first-run onboarding) takes over
    // the entire terminal — no chrome, no prompt, no messages rendered.
    // TS REF: FullscreenLayout.tsx L422-426 — ModalContext provides actual
    // terminal dimensions to dialogs.  Pass real term_cols/term_rows instead
    // of the old hardcoded 120x40.
    if (s.dialog_queue.has_standalone()) {
        return dialog_queue_render::RenderStandaloneDialog(s, term_cols, term_rows);
    }

    // Legacy RouteDialog path — only used when dialog_queue has no
    // overlay/bottom/modal slots.  Eventually this will be phased out
    // in favour of the queue for all dialogs.

    // ── M1 FullscreenLayout: 3-state sticky prompt chrome ─────────────
    // TS REF: FullscreenLayout.tsx lines 339-351 (3-state discriminant,
    //        padCollapsed resolution, headerPrompt guard).
    slots.sticky_prompt         = s.sticky_prompt;
    slots.sticky_clicked        = s.sticky_prompt_clicked;
    slots.hide_sticky           = false;
    slots.pill_visible          = s.pill_visible;
    slots.hide_pill             = false;
    slots.new_message_count     = s.unseen_message_count;

    // on_sticky_click: the TS pattern "onClick={headerPrompt.scrollTo}"
    // (line 344) sets stickyPrompt='clicked' (the literal sentinel) via a
    // stable setState that reacts before scrollTo side-effects fire.  We
    // match that order in C++: (1) flip sticky_prompt_clicked to hide the
    // header + keep padCollapsed=true; (2) compute the delta between the
    // prompt's visual line and current scroll_top and ask ScrollTranscript
    // to jump there.
    //
    // Captures: `&s` is a ReplScreenState& whose lifetime is bound to the
    // outer `std::shared_ptr<ReplScreenState>` in MakeReplScreen; it is
    // stable across renders.  The callback is only invoked from within
    // FTXUI event dispatch (same thread), so no data races.
    slots.on_sticky_click = [&s](const fl::StickyPrompt& sp) {
        s.sticky_prompt_clicked = true;
        // Jump so the target visual line is at the TOP of the viewport.
        // scroll_target_row is measured from scroll_top=0 (content
        // coordinates); ScrollTranscript(delta) is relative — so delta =
        // target - current.  Clamp against viewport_rows to avoid
        // overshooting below min-scroll.
        int current = std::max(0, s.scroll_offset);
        int delta   = static_cast<int>(sp.scroll_target_row) - current;
        if (delta != 0) {
            // We need to call ScrollTranscript which takes
            // shared_ptr<ReplScreenState>.  Here we only have a bare ref;
            // but this callback is dispatched from FTXUI's event loop from
            // within the outer MakeReplScreen Component's OnEvent chain
            // which owns the shared_ptr.  For a purely visual change
            // (clicking the header is a scroll, not engine-state mutation),
            // a direct offset mutation achieves the same effect without
            // requiring the shared_ptr here.
            int viewport = std::max(1, s.viewport_height_lines);
            int total;
            if (s.virtual_list_active) {
                namespace vl = cc::ui::messages::virtual_list;
                total = s.virtual_jh.total();
            } else {
                const auto vm = BuildVisibleMessages(s);
                total = EstimateTranscriptRows(vm);
            }
            int max_top = std::max(0, total - viewport);
            int old_top = std::clamp(current, 0, max_top);
            int target  = std::clamp(old_top + delta, 0, max_top);
            if (target != old_top) {
                s.scroll_offset = target;
                s.scroll_pinned_to_bottom = (target >= max_top);
                if (s.virtual_list_state) {
                    namespace vl = cc::ui::messages::virtual_list;
                    s.virtual_list_state->scroll_top = target;
                    vl::update_sticky_after_scroll(*s.virtual_list_state,
                                                    old_top);
                }
            }
        }
        // TS note: after the click, stickyPrompt stays at 'clicked' until
        // the NEXT scroll event re-emits a fresh {text,scrollTo} from
        // StickyTracker.  Any movement (wheel, PageUp, click-to-select)
        // that moves the viewport will write a new sticky_prompt and
        // clear sticky_prompt_clicked.  We therefore do NOT clear the
        // flag ourselves here.
    };

    // on_pill_click: TS lines 371-381 — clicking the "N new messages" pill
    // re-pins to the bottom.  Same lifetime reasoning as on_sticky_click.
    slots.on_pill_click = [&s] {
        int viewport = std::max(1, s.viewport_height_lines);
        int total;
        if (s.virtual_list_active) {
            namespace vl = cc::ui::messages::virtual_list;
            total = s.virtual_jh.total();
        } else {
            const auto vm = BuildVisibleMessages(s);
            total = EstimateTranscriptRows(vm);
        }
        int max_top = std::max(0, total - viewport);
        int old_top = std::clamp(s.scroll_offset, 0, max_top);
        if (max_top != old_top) {
            s.scroll_offset = max_top;
            s.scroll_pinned_to_bottom = true;
            if (s.virtual_list_state) {
                namespace vl = cc::ui::messages::virtual_list;
                s.virtual_list_state->scroll_top = max_top;
                vl::update_sticky_after_scroll(*s.virtual_list_state, old_top);
            }
            // Clear the pill + unseen count on repin (mirrors TS onRepin
            // setting dividerIndex=null — the pill only shows while
            // pill_visible=true AND a divider snapshot exists.)
            s.pill_visible = false;
            s.unseen_message_count = 0;
            s.divider_index.reset();
            s.unseen_divider.reset();
        }
    };

    Element base = fl::ComposeFullscreen(std::move(slots));
    auto dlg = RouteDialog(s.mode, s);
    if (dlg) base = dbox({
        std::move(base) | dim,
        vbox({ filler(),
               hbox({ filler(), std::move(*dlg) | flex_shrink, filler() })
               | flex_shrink, filler() }) | flex });

    // M7: Layer Bottom + Overlay + Modal dialogs from the dialog_queue.
    // TS REF: FullscreenLayout.tsx L422-426 — ModalContext provides actual
    // terminal dimensions (cols-4, rows-PEEK-1) to modal dialogs.  Pass real
    // term_cols/term_rows here instead of the old hardcoded 120x40 so dialog
    // renderers get accurate viewport geometry.
    bool tool_animating = s.spinner_mode != SpinnerMode::Hidden;
    return dialog_queue_render::LayerAllDialogs(
        std::move(base), s, s.is_prompt_input_active,
        /*allow_dialogs_with_animation=*/!tool_animating, term_cols, term_rows);
}

}  // namespace cc::ui::repl_screen
