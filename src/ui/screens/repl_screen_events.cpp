// repl_screen_events.cpp - impl unit for cc.ui.screens.repl_screen
// (RFC 0001 Phase C batch 9). both ReplScreen() component factories - the Renderer lambda and the
// full CatchEvent tier (dialog queue > panel dialogs > global shortcuts >
// readline input).
//
// Phase A (#184957): textual FTXUI GMF headers + `import std;`
// (the textual FTXUI includes keep the reduced-BMI writer happy).
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

module cc.ui.screens.repl_screen;

import std;

import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.foundation.design_figures;
import cc.ui.foundation.theme_provider;
import cc.ui.prompt.prompt_input_footer;

namespace cc::ui::repl_screen {
using namespace ftxui;

/// Build the REPL screen as an FTXUI Component.
/// Engine updates the externally-held state between frames.
/// Event tiers (TS global+command keybindings):
///   Dialog(Esc/y/n/a/c/r/q) > Global(Ctrl+C/D/L/O) > Input(Enter/
///     Ctrl+J/Tab/Shift+Tab/Up/Down/Esc/printable/Backspace)
[[nodiscard]] Component ReplScreen(
    std::shared_ptr<ReplScreenState> state,
    ReplScreenCallbacks cbs) {
    auto cb = std::make_shared<ReplScreenCallbacks>(std::move(cbs));
    return Renderer([state, cb]() -> Element {
        // M7.5: All dialogs flow through DialogQueue — no legacy bridge needed.
        // Engine pushes via PushXxx() (app.cppm / query_engine.cppm).

        // M6: Faithful permission panels — bash / file_edit / file_write.
        // Rendered as a dbox overlay (matching TS overlay slot).
        if (state->mode == ReplMode::ToolPermission &&
            state->permission_request) {
            // dlg-permission-legacy: state-owned TS-faithful panel
            // (TS REF: PermissionRequest.tsx:47-82 dispatch by tool
            // identity) replaces the legacy paragraph(
            // render_permission_dialog(...)) ANSI string.
            Element base = RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
            Element panel = dialog_router::render_tool_permission(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), panel | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        // UI3: SettingsView modal — render the tabbed settings dialog
        // over the dimmed REPL background.
        if (state->mode == ReplMode::SettingsView) {
            Element base = RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
            Element settings_content = dialog_router::render_settings(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), settings_content | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        if (state->mode == ReplMode::AgentsView) {
            Element agents_content = dialog_router::render_agents_menu(state, cb);
            return vbox({ filler(),
                          separator() |
                              color(cc::ui::design::theme::current_theme().palette->muted),
                          hbox({ text("  "),
                                 agents_content | flex,
                                 text("  ") }) | flex_shrink }) | flex;
        }
        // UI8: TrustDialog is a STANDALONE slot — it takes over the full
        // terminal.  No chrome, no prompt, no messages show behind it.
        if (state->mode == ReplMode::TrustDialog) {
            return vbox({
                filler(),
                hbox({ filler(),
                       dialog_router::render_trust_dialog(state, cb) | flex_shrink,
                       filler() }) | flex_shrink,
                filler() }) | flex;
        }
        return RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
    })
         | CatchEvent([state, cb](Event ev) -> bool {
    // --- M7: dialog_queue event dispatch (priority 0) ---
    // Standalone > Modal > Overlay > Bottom.  This block runs FIRST
    // so that a ToolPermission overlay consumes y/n/a before the
    // legacy in_dialog / input paths see it.
    bool tool_animating = state->spinner_mode != SpinnerMode::Hidden;
    if (dialog_queue_render::DispatchDialogQueueEvents(
            *state, ev, state->is_prompt_input_active,
            /*allow_dialogs_with_animation=*/!tool_animating)) {
        return true;
    }

    // --- Panel-mode predicate ---
    auto is_panel = [](ReplMode m){ return
        m==ReplMode::Normal || m==ReplMode::TasksView
        || m==ReplMode::TeamsView || m==ReplMode::HelpView
        || m==ReplMode::QuickOpen; };
    const bool in_dialog = !is_panel(state->mode);

    // 0) Dialog queue — takes priority over legacy ReplMode dialogs
    //    (M7.5: migration path from ReplMode to DialogQueue).
    //    NOTE: has_standalone/modal/overlay/bottom are checked inline
    //    inside DispatchDialogQueueEvents() (called above), so this
    //    block is intentionally empty — the dedicated per-slot helper
    //    namespace functions are not re-exposed as free predicates here
    //    to avoid a duplicate definition with dialog_queue_render.
    (void)state;

    // 1) Dialog-context events
    if (in_dialog) {
        // UI8 TrustDialog: highest priority (standalone slot), takes every
        // event so that the 4-tier selection / countdown / YES-typing
        // gating can work reliably.
        if (state->mode == ReplMode::TrustDialog) {
            return dialog_router::forward_trust_dialog(state, cb, ev);
        }
        // Ctrl+L is a GLOBAL redraw (TS defaultBindings.ts:42, global
        // context) — it must work even while a tool-permission panel/dialog
        // is open, so handle it before forwarding the event to any panel
        // (which otherwise unconditionally consumes it).
        if (ev == Event::Character('\x0C')) {
            if (cb->on_redraw) cb->on_redraw();
            return true;
        }
        // dlg-permission-legacy: the panel owns all its documented keys;
        // runs BEFORE the Esc switch and legacy y/n/a block (left as
        // harmless dead fallback).  TS dispatch REF:
        // PermissionRequest.tsx:47-82.
        if (state->mode == ReplMode::ToolPermission) {
            return dialog_router::forward_tool_permission(state, cb, ev);
        }
        // UI13 agent wizard: forward every event to the wizard component
        // (it manages Esc/Enter/buttons internally).
        if (state->mode == ReplMode::CreateAgent ||
            state->mode == ReplMode::EditAgent) {
            return dialog_router::forward_agent(state, cb, ev);
        }
        if (state->mode == ReplMode::AgentsView) {
            return dialog_router::forward_agents_menu(state, cb, ev);
        }
        // UI15 wizard modes: forward every event to the wizard
        // component (they manage Esc/Enter/buttons internally).
        if (ev == Event::Escape) {
            // Critical dialogs defer to y/n/a/c/r/q handlers — EXCEPT
            // CostThreshold where Esc MUST ACKNOWLEDGE (never quit / data-loss).
            switch (state->mode) {
              case ReplMode::ToolPermission:
              case ReplMode::SandboxPermission:
              case ReplMode::WorkerSandboxPermission:
                break;
              case ReplMode::CostThreshold: {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, Event::Escape);
                return true; }
              default:
                state->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                return true; } }
        // CostThreshold: Enter / Space / shortcuts all fire on_done().
        // Delegate to the unified HandleCostThresholdEvent.
        if (state->mode == ReplMode::CostThreshold) {
            if (ev == Event::Return ||
                (ev.is_character() && ev.character() == " ")) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
        if (ev.is_character()) {
            char c = ev.character()[0];
            const bool is_perm =
                state->mode==ReplMode::ToolPermission ||
                state->mode==ReplMode::SandboxPermission ||
                state->mode==ReplMode::WorkerSandboxPermission ||
                state->mode==ReplMode::Elicitation ||
                state->mode==ReplMode::PromptHook;
            if (is_perm) {
                if (c=='y'||c=='Y'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,false);
                    return true;
                }
                if (c=='n'||c=='N'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(false,std::nullopt);
                    return true;
                }
                if (c=='a'||c=='A'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,true);
                    return true;
                }
            }
            // CostThreshold: ALL characters are swallowed by the unified
            // handler.  Shortcuts (g/y/o/k) will fire on_done(); any other
            // character is silently consumed to prevent prompt-injection.
            if (state->mode == ReplMode::CostThreshold) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                (void)ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
    }

    // 2) Global shortcuts
    if (ev == Event::Character('\x03'))
        { if (cb->on_interrupt) cb->on_interrupt(); return true; }
    if (ev == Event::Character('\x04'))
        { if (cb->on_exit) cb->on_exit(); return true; }
    // Ctrl+L: force terminal redraw WITHOUT mutating input.
    // TS REF: src/keybindings/defaultBindings.ts:42 'ctrl+l' -> 'app:redraw'
    //   (Global context, so it works while dialogs are open too) and
    //   useGlobalKeybindings.tsx:225-228 handleRedraw -> ink forceRedraw,
    //   which writes ERASE_SCREEN (CSI 2 J = '\x1b[2J') + CURSOR_HOME
    //   (CSI H = '\x1b[H') and repaints the current content; input_text,
    //   cursor and autocomplete suggestions are never touched.
    if (ev == Event::Character('\x0C')) {
        if (cb->on_redraw) cb->on_redraw();
        return true;
    }
    // Ctrl+O: toggle transcript mode (TS: app:toggleTranscript, global context).
    // In transcript mode the message list shows ALL message types (bypassing
    // brief/dropText filters), capped at last 30 unless show_all_in_transcript.
    // TS REF: Messages.tsx L459 (isTranscriptMode = screen === 'transcript')
    //         + REPL.tsx Ctrl+O → setScreen('transcript') toggle.
    if (!in_dialog && ev == Event::Character('\x0F')) {
        state->is_transcript_mode = !state->is_transcript_mode;
        // When exiting transcript mode, also reset show_all_in_transcript
        // so re-entering starts from the capped default (TS: showAllInTranscript
        // defaults false — user must press Ctrl+E each session to lift the cap).
        if (!state->is_transcript_mode) {
            state->show_all_in_transcript = false;
        }
        return true;
    }

    // Ctrl+R: enter history search mode by injecting "@history " into input.
    // This triggers the @history autocomplete branch in RefreshAutocompleteSuggestions
    // which reads persisted prompt history from ~/.loom/history.jsonl.
    // TS REF: src/hooks/useHistorySearch.ts:151 (handleStartSearch — Ctrl+R enters
    //   history search mode with substring matching against persisted history)
    // TS REF: src/components/PromptInput/PromptInput.tsx — Ctrl+R keyboard shortcut
    //   dispatches 'chat:openHistorySearch' which opens the HistorySearchDialog.
    if (!in_dialog && ev == Event::Character('\x12')) {
        if (!state->input_text.starts_with("@history")) {
            state->input_text = "@history ";
            state->input_cursor = state->input_text.size();
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->dismissed_autocomplete_for_input.clear();
        }
        state->is_prompt_input_active = true;
        state->last_keystroke = std::chrono::steady_clock::now();
        return true;
    }

    // Ctrl+E: dual behavior depending on mode.
    //   - In transcript mode: toggle show_all_in_transcript (lift/restore 30-msg cap).
    //     TS: transcript:toggleShowAll (Transcript context, defaultBindings L163).
    //   - Otherwise: toggle expand/collapse of all tool rows in visible transcript.
    //     TS REF: Messages.tsx expandedKeys (L563) — user can expand tool results
    //     to see full output.  This shortcut toggles ALL tool rows at once.
    if (!in_dialog && ev == Event::Character('\x05')) {
        if (state->is_transcript_mode) {
            // Transcript mode: lift or restore the 30-message cap.
            state->show_all_in_transcript = !state->show_all_in_transcript;
        } else {
            // Normal mode: expand or collapse all tool rows.
            if (state->expanded_keys.empty()) {
                for (const auto& m : state->messages) {
                    if (m.tool_name && !m.tool_name->empty()) {
                        state->expanded_keys.insert(*m.tool_name);
                    }
                }
            } else {
                state->expanded_keys.clear();
            }
        }
        return true;
    }

    // Ctrl+S: stash / restore prompt (TS: 'chat:stash' action, defaultBindings L85).
    // TS REF: src/components/PromptInput/PromptInput.tsx:1356-1383 — handleStash():
    //   - If input is empty and stashedPrompt exists → pop stash (restore)
    //   - If input is non-empty → push stash (save text + cursorOffset + pastedContents),
    //     clear input, clear pastedContents.
    // The stash notice (prompt_stash_notice.cppm) renders above the input area
    // when HasStashedPrompt() is true, so the user knows their typed text was saved.
    // Auto-restore happens on the next non-slash-command submit (RestoreStashedPrompt
    // called at lines 3804 and 3828 below).
    if (!in_dialog && ev == Event::Character('\x13')) {
        if (state->input_text.empty() && HasStashedPrompt(state)) {
            // Input empty + stash exists → restore (pop stash into input)
            RestoreStashedPrompt(state);
        } else if (!state->input_text.empty()) {
            // Input non-empty → stash current input, then clear it
            StashCurrentPrompt(state);
            state->input_text.clear();
            state->input_cursor = std::string::npos;
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
        }
        state->last_keystroke = std::chrono::steady_clock::now();
        state->is_prompt_input_active = true;
        return true;
    }

    if (!in_dialog &&
        state->active_local_jsx_command &&
        cb->on_local_jsx_event &&
        cb->on_local_jsx_event(ev)) {
        return true;
    }

    if (!in_dialog && state->active_local_jsx_command && ev == Event::Escape) {
        if (cb->on_local_jsx_cancel) {
            cb->on_local_jsx_cancel();
        } else {
            state->active_local_jsx_command = false;
            state->active_local_jsx_command_name.clear();
            state->active_local_jsx_command_args.clear();
            state->active_local_jsx_content.clear();
        }
        return true;
    }

    if (!in_dialog && ev.is_mouse()) {
        if (ev.mouse().button == Mouse::WheelUp) {
            return ScrollTranscript(state, -3);
        }
        if (ev.mouse().button == Mouse::WheelDown) {
            return ScrollTranscript(state, 3);
        }
    }
    if (!in_dialog && state->autocomplete_suggestions.empty()) {
        const int page = std::max(1, state->viewport_height_lines / 2);
        if (ev == Event::PageUp) {
            return ScrollTranscript(state, -page);
        }
        if (ev == Event::PageDown) {
            return ScrollTranscript(state, page);
        }
    }

    // 3) Input-context events
    const bool accept_input = !in_dialog;
    if (accept_input) {
        const int asn = static_cast<int>(state->autocomplete_suggestions.size());
        // Enter
        if (ev == Event::Return && asn > 0) {
            const int selected = std::clamp(
                state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
                0,
                asn - 1);
            const bool submit =
                state->autocomplete_suggestions[static_cast<std::size_t>(selected)]
                    .submit_on_return;
            auto accepted = accept_selected_prompt_suggestion(state);
            if (submit && accepted && cb->on_submit) {
                // TS REF: src/components/PromptInput/inputModes.ts:23-29
                //   (getValueFromInput)
                // Strip '!' mode prefix from accepted value before engine.
                namespace figs = cc::ui::design::figures;
                std::string submit_text =
                    std::string(figs::strip_mode_prefix(*accepted));
                cb->on_submit(submit_text, state->input_mode);
                // TS REF: src/components/PromptInput/inputModes.ts:4-14
                //   (prependModeCharacterToInput) + REPL.tsx:3318
                // History stores the mode-prefixed form for round-trip
                // mode detection on recall.  If the user toggled bash via
                // bare '!' (input_text has NO '!'), prepend it.  If the
                // text already carries '!' (direct "!cmd" typing), keep
                // it as-is to avoid double-prefix.
                {
                    namespace figs = cc::ui::design::figures;
                    const bool text_has_prefix =
                        !accepted->empty() &&
                        (*accepted)[0] == figs::kBashModeChar;
                    const bool is_bash =
                        state->input_mode == InputMode::Bash;
                    const std::string hist_entry =
                        (is_bash && !text_has_prefix)
                            ? figs::prepend_mode_char(
                                  *accepted,
                                  figs::PromptMode::kBash)
                            : std::string(*accepted);
                    state->input_history.push_back(hist_entry);
                }
                if (state->input_history.size() > 1000) state->input_history.pop_front();
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
                state->is_prompt_input_active = false;
                // GAP 2: auto-restore stashed prompt after submit completes.
                // TS REF: REPL.tsx L3344-3348 — restore stashedPrompt when
                // the input is cleared by a non-slash-command submit.
                RestoreStashedPrompt(state);
            }
            return true;
        }
        if (ev == Event::Return && !state->input_text.empty()) {
            // TS REF: src/components/PromptInput/inputModes.ts:23-29 (getValueFromInput)
            // Strip the '!' mode prefix before passing to engine.
            // History keeps the prefix for round-tripping
            // (prependModeCharacterToInput semantics in inputModes.ts:4-14).
            namespace figs = cc::ui::design::figures;
            std::string submit_text =
                std::string(figs::strip_mode_prefix(state->input_text));
            if (cb->on_submit) cb->on_submit(submit_text, state->input_mode);
            // TS REF: src/components/PromptInput/inputModes.ts:4-14
            //   (prependModeCharacterToInput) + REPL.tsx:3318
            // History stores the mode-prefixed form for round-trip
            // mode detection on arrow-up recall.  If the user toggled
            // bash via bare '!' (input_text has NO '!'), prepend it.
            // If the text already carries '!' (direct "!cmd" typing),
            // keep it as-is to avoid double-prefix.
            {
                namespace figs = cc::ui::design::figures;
                const bool text_has_prefix =
                    !state->input_text.empty() &&
                    state->input_text[0] == figs::kBashModeChar;
                const bool is_bash =
                    state->input_mode == InputMode::Bash;
                const std::string hist_entry =
                    (is_bash && !text_has_prefix)
                        ? figs::prepend_mode_char(
                              state->input_text,
                              figs::PromptMode::kBash)
                        : std::string(state->input_text);
                state->input_history.push_back(hist_entry);
            }
            if (state->input_history.size() > 1000) state->input_history.pop_front();
            state->history_index = std::string::npos;
            state->input_text.clear();
            state->input_cursor = std::string::npos;
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->is_prompt_input_active = false;
            // GAP 2: auto-restore stashed prompt after submit.
            // TS REF: REPL.tsx L3344-3348 — restore stashedPrompt when
            // the input is cleared by a non-slash-command submit.
            RestoreStashedPrompt(state);
            return true; }
        // Ctrl+Enter -> newline (Ctrl+J in terminals)
        if (ev == Event::Character('\x0A')) {
            insert_prompt_text(state, "\n");
            return true; }
        // Shift+Enter -> newline. Two terminal encodings:
        //   xterm modifyOtherKeys / CSI-u : ESC [ 13 ; 2 u
        //   kitty keyboard protocol       : ESC [ 27 ; 2 ; 13 ~
        if (ev.input() == "\x1b[13;2u" || ev.input() == "\x1b[27;2;13~") {
            insert_prompt_text(state, "\n");
            return true; }
        // Tab / Shift+Tab -> autocomplete
        if (ev == Event::Tab && asn > 0) {
            // AT-07: complete to the common prefix of all visible suggestion
            // insert_texts when it strictly extends what's typed (e.g. "@sr"
            // with {@src/readme, @src/main} → "@src/"); otherwise accept the
            // selected suggestion. Faithful to TS typeahead Tab behavior.
            const auto& sugg = state->autocomplete_suggestions;
            std::string common = sugg[0].insert_text;
            for (int k = 1; k < asn && !common.empty(); ++k) {
                const auto& ins = sugg[k].insert_text;
                std::size_t j = 0;
                while (j < common.size() && j < ins.size() && common[j] == ins[j]) ++j;
                common.resize(j);
            }
            const int idx = std::clamp(state->autocomplete_index, 0, asn - 1);
            const auto& sel = sugg[idx];
            const std::size_t rs = (sel.replacement_start == std::string::npos)
                ? 0 : sel.replacement_start;
            const std::size_t re = (sel.replacement_end == std::string::npos)
                ? state->input_text.size() : sel.replacement_end;
            const std::string typed = (rs <= re && re <= state->input_text.size())
                ? state->input_text.substr(rs, re - rs) : std::string{};
            if (!common.empty() && common.size() > typed.size() &&
                common.compare(0, typed.size(), typed) == 0) {
                state->input_text.replace(rs, re - rs, common);
                state->input_cursor = rs + common.size();
                state->is_prompt_input_active = true;
                state->last_keystroke = std::chrono::steady_clock::now();
            } else {
                (void)accept_selected_prompt_suggestion(state);
            }
            return true;
        }
        // Shift+Tab (ISO backtab) = \x1B[Z
        if (ev.input() == "\x1B[Z" && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ? asn - 1
                : (state->autocomplete_index - 1 + asn) % asn; return true; }
        // Shift+Tab without suggestions → cycle permission mode.
        // TS REF: PromptInput.tsx:1667 'chat:cycleMode' shortcut → handleCycleMode
        // → cyclePermissionMode().  The footer renders "(shift+tab to cycle)"
        // when a non-default permission mode is active; this makes it actually work.
        if ((ev.input() == "\x1B[Z" || ev == Event::TabReverse) && asn == 0) {
            namespace pif = cc::ui::prompt::footer;
            state->permission_mode = pif::GetNextPermissionMode(state->permission_mode);
            if (cb->on_permission_cycle) {
                cb->on_permission_cycle(state->permission_mode);
            }
            return true;
        }
        // Up / Down navigate visible autocomplete suggestions before history.
        if (ev == Event::ArrowUp && asn > 0) {
            state->autocomplete_index = state->autocomplete_index <= 0 ? asn - 1
                : state->autocomplete_index - 1; return true; }
        if (ev == Event::ArrowDown && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ||
                state->autocomplete_index >= asn - 1
                ? 0 : state->autocomplete_index + 1; return true; }
        // Ctrl+N (\x0e) / Ctrl+P (\x10) navigate autocomplete suggestions.
        // TS REF: src/hooks/useTypeahead.tsx:1344-1353 (raw ctrl+n/ctrl+p
        //   dispatched to handleAutocompleteNext/Previous) and
        //   :1242-1255 — next wraps selected>=length-1 -> 0; previous wraps
        //   selected<=0 -> length-1. Both early-return when suggestions are
        //   empty (and when a chord is pending — the CPP port has no chord
        //   system, so that gate is omitted). When asn==0 the event
        //   intentionally falls through (TS readline cursor/history movement
        //   is not implemented in this port).
        if (ev == Event::Character('\x0e') && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ||
                state->autocomplete_index >= asn - 1
                ? 0 : state->autocomplete_index + 1; return true; }
        if (ev == Event::Character('\x10') && asn > 0) {
            state->autocomplete_index = state->autocomplete_index <= 0 ? asn - 1
                : state->autocomplete_index - 1; return true; }
        // Up (history back) / Down (history forward)
        if (ev == Event::ArrowUp && state->input_text.empty()
            && !state->input_history.empty()) {
            state->history_index = state->history_index == std::string::npos
                ? state->input_history.size() - 1
                : std::max<std::size_t>(0, state->history_index - 1);
            state->input_text = state->input_history[state->history_index];
            // TS REF: src/components/PromptInput/inputModes.ts:16-21
            //   (getModeFromInput)
            // Sync input_mode from the recalled entry's leading char so that
            // the prefix glyph stays correct after the user clears the text.
            {
                namespace figs = cc::ui::design::figures;
                state->input_mode =
                    (figs::get_mode_from_input(state->input_text) ==
                     figs::PromptMode::kBash)
                        ? InputMode::Bash
                        : InputMode::Normal;
            }
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        if (ev == Event::ArrowDown
            && state->history_index != std::string::npos) {
            if (state->history_index + 1 >= state->input_history.size()) {
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
            } else {
                state->input_text = state->input_history[++state->history_index];
                // TS REF: inputModes.ts:16-21 (getModeFromInput) — sync mode
                // from the recalled entry's leading prefix character.
                {
                    namespace figs = cc::ui::design::figures;
                    state->input_mode =
                        (figs::get_mode_from_input(state->input_text) ==
                         figs::PromptMode::kBash)
                            ? InputMode::Bash
                            : InputMode::Normal;
                }
                state->input_cursor = state->input_text.size(); }
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        // Esc
        if (ev == Event::Escape) {
            // TS REF: PromptInput.tsx:1904 — Escape at cursor 0 exits any
            // special (bash) mode.  Runs first and does NOT itself consume the
            // event, so the existing autocomplete/selection/clear-text
            // priorities below still apply exactly as before.
            const bool mode_exited = exit_input_mode_if_at_start(state);
            if (!state->autocomplete_suggestions.empty()) {
                // INF-05: remember the dismissed input so a later non-mutating
                // keystroke (e.g. arrow keys) doesn't reopen the popup.
                state->dismissed_autocomplete_for_input = state->input_text;
                state->autocomplete_suggestions.clear();
                state->autocomplete_index = -1; return true; }
            if (state->selected_message_idx >= 0)
                { state->selected_message_idx = -1; return true; }
            // Esc double-press to clear non-empty input.
            // TS REF: src/hooks/useTextInput.ts:126-153 (handleEscape) +
            // src/hooks/useDoublePress.ts:6 DOUBLE_PRESS_TIMEOUT_MS = 800.
            // The autocomplete dismiss above mirrors PromptInput.tsx
            // disableEscapeDoublePress = suggestions.length>0: while the
            // popup is open the first Esc dismisses instead of arming, so
            // clearing takes Esc (dismiss) + Esc (arm) + Esc (clear).
            if (!state->input_text.empty()) {
                namespace pif = cc::ui::prompt::footer;
                const auto now_dp = std::chrono::steady_clock::now();
                constexpr auto kDoublePressWindow =
                    std::chrono::milliseconds(800);
                const bool armed = state->escape_pending_since.has_value() &&
                    (now_dp - *state->escape_pending_since) <= kDoublePressWindow;
                if (armed) {
                    // Second press inside the window: clear timer state,
                    // remove the hint immediately, persist BEFORE clearing
                    // (TS addToHistory(originalValue) guarded by trim()!==''),
                    // then clear text/offset/history.
                    state->escape_pending_since.reset();
                    pif::QueueRemoveNotification(
                        state->footer_notification_queue,
                        "escape-again-to-clear");
                    bool has_non_space = false;
                    for (unsigned char c : state->input_text) {
                        if (c != ' ' && c != '\t' && c != '\n' &&
                            c != '\r' && c != '\v' && c != '\f') {
                            has_non_space = true;
                            break;
                        }
                    }
                    if (has_non_space && cb->on_save_to_history) {
                        cb->on_save_to_history(state->input_text);
                    }
                    state->input_text.clear();
                    state->input_cursor = std::string::npos;
                    state->history_index = std::string::npos;
                    return true;
                }
                // First press (or an expired previous press): arm and show
                // the hint; input is NOT cleared. Remove first because
                // QueueAddNotification dedups an already-current same-key
                // item and would otherwise not refresh the 1000ms timeout.
                state->escape_pending_since = now_dp;
                pif::NotificationItem item;
                item.key = "escape-again-to-clear";
                item.text = "Esc again to clear";  // TS exact string
                item.color = "";
                item.priority = pif::NotificationPriority::Immediate;
                item.timeout_ms = 1000;            // TS timeoutMs: 1000
                pif::QueueRemoveNotification(
                    state->footer_notification_queue,
                    "escape-again-to-clear");
                pif::QueueAddNotification(
                    state->footer_notification_queue, item);
                return true;
            }
            // Empty input: TS handleEscape's setPending callback early-
            // returns (no arming, no notification). The mode-exit (bash ->
            // prompt) above still coexists when it happened.
            // Only the mode-exit happened (empty input, no popup/selection):
            // still consume the event so the reset is reflected.
            if (mode_exited) return true; }
        if (ev == Event::ArrowLeft) {
            move_prompt_cursor_left(state);
            return true;
        }
        if (ev == Event::ArrowRight) {
            move_prompt_cursor_right(state);
            return true;
        }
        if (ev == Event::Home) {
            state->input_cursor = 0;
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::End) {
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::Delete) {
            if (delete_prompt_text(state)) return true;
            // At cursor 0 with nothing to delete: exit bash/special mode
            // (TS parity, PromptInput.tsx:1904 lists key.delete).
            if (exit_input_mode_if_at_start(state)) return true;
        }
        // Ctrl+U (\x15): kill from cursor to start of line.  When the cursor is
        // already at position 0 (nothing to kill) this exits bash/special mode
        // instead (TS parity, PromptInput.tsx:1904 lists `key.ctrl && char==='u'`).
        if (ev == Event::Character("\x15")) {
            const auto cursor = input_cursor_or_end(*state);
            if (cursor > 0) {
                state->input_text.erase(0, cursor);
                set_prompt_input_text(state, std::move(state->input_text), 0);
                return true;
            }
            if (exit_input_mode_if_at_start(state)) return true;
            return true;  // consume Ctrl+U even when it's a no-op
        }
        // Printable chars — accepts both ASCII and multi-byte UTF-8 (CJK).
        // FTXUI delivers composed IME characters as a single character event
        // containing the full UTF-8 byte sequence in ev.character().
        if (ev.is_character()) {
            const std::string& ch = ev.character();
            if (!ch.empty()) {
                unsigned char first = static_cast<unsigned char>(ch[0]);
                // Accept printable ASCII (0x20..0x7E) and UTF-8 multi-byte
                // start bytes (0xC0..0xFF).  Continuation bytes (0x80..0xBF)
                // should never appear as the first byte of a character event.
                const bool is_printable =
                    (first >= 0x20 && first < 0x7F) || first >= 0xC0;
                if (is_printable) {
                    namespace figs = cc::ui::design::figures;

                    // Footer "Pasting text…" feedback (TS usePasteHandler.ts):
                    // terminals deliver a paste as one multi-char batch, while
                    // a single CJK keystroke is at most 4 UTF-8 bytes. Stamp
                    // the burst time; the footer hides the hint 100ms later.
                    if (ch.size() > 4) {
                        state->pasting_since =
                            std::chrono::steady_clock::now();
                    }
                    // ── P0-1: TS-equivalent single-char mode interception ──
                    //
                    // TS PromptInput.tsx lines 869-901: when the user types a
                    // single '!' with cursor at offset 0 into an EMPTY input
                    // buffer, that's a MODE TRANSITION — NOT a character to
                    // store.  The '!' is swallowed, InputMode flips, and the
                    // prefix glyph changes without the text ever landing in
                    // the input state (so history persists cleanly).
                    //
                    // All other cases (multi-byte paste of "!cmd", cursor
                    // nonzero, typing '!' into existing text) → fall through
                    // to insert_prompt_text as normal; HandleSubmit will
                    // strip the prefix on submit for those.
                    const bool cursor_at_zero =
                        state->input_cursor == std::string::npos ||
                        state->input_cursor == 0;
                    if (state->input_text.empty() && cursor_at_zero &&
                        figs::is_mode_character(ch) &&
                        ch.size() == 1) {
                        // Swallow the char, flip mode.  Toggle Normal↔Bash.
                        state->input_mode =
                            (state->input_mode == InputMode::Bash)
                                ? InputMode::Normal
                                : InputMode::Bash;
                        state->is_prompt_input_active = true;
                        state->last_keystroke =
                            std::chrono::steady_clock::now();
                        // Also wipe any dismissed-suggestion memory since
                        // mode change implicitly changes the autocomplete
                        // provider context.
                        state->dismissed_autocomplete_for_input.clear();
                        return true;
                    }
                    // ── P0-1: TS-equivalent multi-char "!cmd" interception ──
                    //
                    // TS PromptInput.tsx lines 878-886: when "!cmd" lands as a
                    // single multi-char insertion at cursor-0 into an EMPTY
                    // input (IME composition, bracketed paste, or any path
                    // that bypasses the single-char-by-single-char typing
                    // flow), the '!' is stripped, inputMode flips to 'bash',
                    // and the clean "cmd" text is stored — NOT "!cmd".
                    //
                    // Without this, the user sees "! !cmd" visually (prefix
                    // glyph + text both carrying '!') because the single-char
                    // interceptor only fires for ch.size()==1.
                    if (state->input_text.empty() && cursor_at_zero &&
                        figs::get_mode_from_input(ch) ==
                            figs::PromptMode::kBash &&
                        ch.size() > 1) {
                        // Strip '!', enter bash mode, insert clean text.
                        state->input_mode = InputMode::Bash;
                        state->is_prompt_input_active = true;
                        state->last_keystroke =
                            std::chrono::steady_clock::now();
                        state->dismissed_autocomplete_for_input.clear();
                        std::string clean(figs::strip_mode_prefix(ch));
                        insert_prompt_text(state, clean);
                        return true;
                    }
                    insert_prompt_text(state, ch);
                    return true;
                }
            }
        }
        // Backspace — handle multi-byte UTF-8 correctly by erasing a full
        // codepoint, not just the last byte.  CJK characters are 3 bytes in
        // UTF-8, so a plain pop_back() would leave a partial/invalid sequence.
        if (ev == Event::Backspace) {
            if (backspace_prompt_text(state)) return true;
            // At cursor 0 (nothing to erase): exit bash/special mode (TS parity,
            // PromptInput.tsx:1904).  This is the fix for being unable to leave
            // bash mode after a bare '!' left the buffer empty.
            if (exit_input_mode_if_at_start(state)) return true;
        }
    }
    return false; });
}

/// Convenience: self-owned state (demos/tests only).
/// Production: use externally-held shared_ptr<ReplScreenState> overload.
[[nodiscard]] Component ReplScreen(ReplScreenCallbacks cbs) {
    return ReplScreen(std::make_shared<ReplScreenState>(), std::move(cbs));
}

}  // namespace cc::ui::repl_screen
