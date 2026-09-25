/// @file repl_screen.cppm
/// @brief Main REPL screen skeleton: enums, state, layout orchestration,
///        dialog routing, and event binding.  Rendering sub-modules delegate
///        to dedicated UIx agents (see ownership matrix below).
/// Migrated from src/screens/REPL.tsx (5005 lines)
///
/// =========================================================
/// PHASE 4 COMPONENT MATRIX — Sub-Component -> Agent Ownership
/// =========================================================
/// UI2  PromptInput full              UI11 Onboarding / wizards
/// UI3  Settings / model picker      UI12 Tasks panel UI
/// UI4  Assistant msg (markdown)     UI13 Agents panel / editor
/// UI5  User msg / attachments       UI14 Teams panel
/// UI6  CustomSelect / dropdowns     UI15 Install wizards
/// UI7  Structured diff viewer       UI16 Plugin dialogs / recs
/// UI8  Trust + sandbox dialogs      UI17 Code rendering (hl/md/term)
/// UI9  Permissions system           UI18 Feedback surveys
/// UI10 MCP dialogs (elicit/OAuth)   UI19 Spinner animations + tree
/// =========================================================
///
/// RENDERING DELEGATION (this file does NOT contain render bodies):
///   status bar   -> cc.ui.prompt.prompt_input_footer (UI1, user-configurable command output)
///   spinner      -> cc.ui.components.spinner_widget (UI19)
///   msg list     -> cc.ui.messages.messages (RenderMessages wrapper, UI4/5)
///   prompt input -> cc.ui.prompt.prompt_input_full (UI2)
///   dialogs      -> cc.ui.dialogs.* (DialogQueue 4-slot system, UI8-UI11/UI16)
///
/// RFC 0001 Phase C batch 9: every body lives in ten module implementation
/// units — repl_screen_messages.cpp (row projection), repl_screen_scroll.cpp
/// (unseen divider / scroll bounds), repl_screen_prompt_buffer.cpp (input
/// mutation + stash), repl_screen_welcome.cpp (status bar / spinner / welcome
/// header), repl_screen_prompt_render.cpp (prompt input + suggestions),
/// repl_screen_dialog_queue.cpp (the 4-slot queue renderer),
/// repl_screen_layout.cpp (RenderReplScreen + RouteDialog),
/// repl_screen_agents.cpp (agent wizard + agents menu),
/// repl_screen_dialog_panels.cpp (settings / trust / tool permission), and
/// repl_screen_events.cpp (both ReplScreen factories).  This primary keeps
/// only declarations, AgentMenuOptions, and the declaration-only
/// AgentMenuListBase class.
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.screens.repl_screen;

import std;

export import cc.ui.screens.repl_state;

// Core engine types (ImageBlock in the stash signatures; only named
// globally qualified as ::cc::core::ImageBlock).
import cc.types.types;  // arch-check: keep-import
// UnseenDivider used by RenderMessages / ComputeUnseenDivider.
import cc.ui.messages.messages_list;
// StreamingMarkdown pointer in surviving declarations (only named
// globally qualified as ::cc::ui::StreamingMarkdown*).
import cc.ui.visual.markdown;  // arch-check: keep-import
// AgentCardData is a member type of AgentMenuOptions.
import cc.ui.features.agents.agent_cards;

// Forward imports (implement bodies in owning agent modules):
//   cc.ui.dialogs.{permission_prompts,mcp_dialogs,trust_dialog,
//                  sandbox_dialog,settings_dialog,model_picker,
//                  ide_dialogs,plugin_dialog,
//                  feedback_survey,config_dialog,mcp_dialogs}
//   cc.ui.prompt.{prompt_input_full,autocomplete,vim_input}
//   cc.ui.messages.{assistant_message,user_message,structured_diff}
//   cc.ui.components.{custom_select,diff_view,spinner_widget,
//                     file_tree,text_input_widget,notification,
//                     cost_display,dev_bar,status_line}
//   cc.ui.{design.dialog,hooks.hooks_ui,permissions.permission_views,
//          agents.agent_editor,tasks.task_list_ui,markdown,terminal}

export namespace cc::ui::repl_screen {
using namespace ftxui;

// =========================================================
// Status bar / spinner / welcome primitives
// (bodies in repl_screen_welcome.cpp)
// =========================================================

// UI1: status bar — delegates to cc.ui.components.status_line.
[[nodiscard]] Element RenderStatusBar(const StatusBarData& d);

// UI19: spinner line shell.  Single loom-gold theme, cycling
// TEARDROP_ASTERISK glyph, random playful verb, 3-dot blink cadence.
[[nodiscard]] Element RenderSpinner(
    SpinnerMode m, const std::optional<std::string>& verb,
    const std::optional<std::string>& tip,
    int frame = 0);

// Column-width truncation helper; also used by the prompt-render unit.
[[nodiscard]] std::string truncate_columns(std::string text, int max_cols);

[[nodiscard]] std::string repeat_welcome_segment(std::string_view text, int count);

// =========================================================
// Unseen divider + transcript projection / scroll
// (bodies in repl_screen_scroll.cpp)
// =========================================================

// TS REF: FullscreenLayout.tsx countUnseenAssistantTurns / computeUnseenDivider.
namespace unseen_detail {

/// Whether an assistant entry has visible text content.
[[nodiscard]] bool assistant_has_visible_text(
    const MessageDisplayEntry& e);

/// Count assistant turns in entries[start_idx..end).
[[nodiscard]] std::size_t count_unseen_assistant_turns(
    const std::vector<MessageDisplayEntry>& entries,
    std::size_t start_idx);

}  // namespace unseen_detail

/// Compute the UnseenDivider from state.divider_index + messages.
[[nodiscard]] std::optional<::cc::ui::messages_list::UnseenDivider>
ComputeUnseenDivider(const ReplScreenState& s);

/// Project state.messages, appending the active local-jsx command rows.
[[nodiscard]] std::vector<MessageDisplayEntry> BuildVisibleMessages(
    const ReplScreenState& s);

// UI4/UI5: message list.  Delegates to messages_list.cppm (UI21).
// `spinner_frame` drives the tool-use header spinner; `unseen_divider` is
// the in-transcript "N new messages" anchor.
[[nodiscard]] Element RenderMessages(
    const std::vector<MessageDisplayEntry>& entries,
    int sel = -1, int vlines = 40,
    int offs = 0, bool pinned = true,
    int spinner_frame = 0,
    std::optional<cc::ui::messages_list::UnseenDivider> unseen_divider =
        std::nullopt,
    Elements leading_elements = {},
    bool is_brief_mode = false,
    const std::unordered_set<std::string>& expanded_keys = {},
    bool is_transcript_mode = false,
    bool show_all_in_transcript = false,
    // When true, build_visible_rows hides ALL completed thinking rows so
    // only the streaming-thinking tail is visible.
    bool streaming_thinking_globally_visible = false,
    // Retry callback for SystemAPIError rich cards.
    std::function<void()> on_retry = nullptr,
    // Clear-session callback for session-expired error cards.
    std::function<void()> on_clear_session = nullptr,
    // StreamingMarkdown stable-prefix cache for the streaming-text tail row.
    ::cc::ui::StreamingMarkdown* streaming_md = nullptr);

/// Count '\n'-separated lines in text (minimum 1).
[[nodiscard]] int CountTextLines(std::string_view text);

/// Estimate rendered transcript height for the non-virtual scroll path.
[[nodiscard]] int EstimateTranscriptRows(
    const std::vector<MessageDisplayEntry>& entries);

/// Wheel / PageUp / PageDown scroll against the virtual JumpHandle or the
/// crude row estimator; maintains the unseen-divider snapshot.
bool ScrollTranscript(const std::shared_ptr<ReplScreenState>& state,
                      int delta);

// =========================================================
// Legacy ASCII-art welcome helpers (preserved, no longer called by
// RenderWelcomeHeader; bodies in repl_screen_welcome.cpp).
// =========================================================
[[nodiscard]] Element RenderLoomMascotMark(Color body, Color bg);
[[nodiscard]] Element RenderWelcomeFeed(std::string title,
                                        std::string message,
                                        int width,
                                        Color accent,
                                        Color muted);
[[nodiscard]] Element RenderWelcomeFeedColumn(int width,
                                              Color accent,
                                              Color muted);
[[nodiscard]] Element RenderWelcomeWindow(Element title,
                                          Element body,
                                          int width,
                                          int title_width,
                                          Color accent);
[[nodiscard]] Element RenderWelcomeLeftPanel(const ReplScreenState& s,
                                             int spinner_frame,
                                             int width,
                                             Color accent,
                                             Color muted,
                                             Color text_color,
                                             Color bg);

// UI0: welcome header — LogoV2 3-mode dispatch (condensed / compact /
// horizontal) on a fresh idle session.
[[nodiscard]] Element RenderWelcomeHeader(const ReplScreenState& s,
                                          int spinner_frame = 0,
                                          int term_cols = 80,
                                          bool force_full_logo = false);

// =========================================================
// Prompt input buffer mutation
// (bodies in repl_screen_prompt_buffer.cpp)
// =========================================================

[[nodiscard]] bool is_utf8_continuation_byte(unsigned char c);
[[nodiscard]] std::size_t clamp_input_cursor(
    const std::string& text,
    std::size_t pos);
[[nodiscard]] std::size_t input_cursor_or_end(
    const ReplScreenState& s);
[[nodiscard]] std::size_t previous_utf8_boundary(
    const std::string& text,
    std::size_t pos);
[[nodiscard]] std::size_t next_utf8_boundary(
    const std::string& text,
    std::size_t pos);

void set_prompt_input_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string value,
    std::size_t cursor);
void insert_prompt_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string_view value);

// AT-09: drain inbound IDE at_mentioned tokens staged by the MCP receive
// thread. MUST be called on the render thread.
std::size_t DrainPendingAtMentionInserts(
    const std::shared_ptr<ReplScreenState>& state);

/// Stash the current input text and cursor position (GAP 2).
bool StashCurrentPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock> pasted_images = {},
    std::unordered_map<int, std::string> pasted_texts = {});

/// Restore the stashed prompt; returns pasted maps via out-params.
bool RestoreStashedPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock>* out_images = nullptr,
    std::unordered_map<int, std::string>* out_texts = nullptr);

/// True when a stashed prompt exists (for UI notice rendering).
bool HasStashedPrompt(const std::shared_ptr<ReplScreenState>& state);

bool backspace_prompt_text(const std::shared_ptr<ReplScreenState>& state);
// At-caret-0 Backspace/Escape/Delete/Ctrl-U exits a special input mode.
bool exit_input_mode_if_at_start(const std::shared_ptr<ReplScreenState>& state);
bool delete_prompt_text(const std::shared_ptr<ReplScreenState>& state);
void move_prompt_cursor_left(const std::shared_ptr<ReplScreenState>& state);
void move_prompt_cursor_right(const std::shared_ptr<ReplScreenState>& state);

// Text-derived bash mode (TS getInputMode equivalent).
[[nodiscard]] bool effective_is_bash(const ReplScreenState& s);

// =========================================================
// Prompt input rendering
// (bodies in repl_screen_prompt_render.cpp)
// =========================================================

// 4-tier contextual placeholder (adapter onto placeholder_cascade).
[[nodiscard]] std::optional<std::string> ComputePlaceholder(
    const ReplScreenState& s);

// The pure-function prompt-input renderer: TextInputImpl synced from the
// projection, vim badge, stash notice, declared caret.
[[nodiscard]] Element RenderPromptInput(const ReplScreenState& s,
                                        int term_cols);

[[nodiscard]] std::string pad_to_columns(std::string text, int width);

// Autocomplete suggestion list (inline footer or fullscreen overlay).
[[nodiscard]] Element RenderPromptSuggestions(const ReplScreenState& s,
                                              int term_cols,
                                              bool is_overlay = false,
                                              int term_rows = 24);

// Accept the currently-selected autocomplete suggestion into the buffer.
[[nodiscard]] std::optional<std::string> accept_selected_prompt_suggestion(
    const std::shared_ptr<ReplScreenState>& state);

// =========================================================
// M7: Queue-based dialog rendering
// (bodies in repl_screen_dialog_queue.cpp)
//
// Four slots in priority order: Standalone > Modal > Overlay > Bottom.
// =========================================================
namespace dialog_queue_render {

/// Build the render-time width/height context for the registry.
[[nodiscard]] DialogRenderContext MakeContext(
    int term_w = 120, int term_h = 40, bool is_modal = false,
    const void* repl_state = nullptr);

/// Standalone dialog: full-takeover render (no chrome).
[[nodiscard]] Element RenderStandaloneDialog(ReplScreenState& s,
                                             int w = 120, int h = 40);

/// Modal dialog (stack top): rendered full-width dbox above the rest.
[[nodiscard]] Element RenderModalDialog(ReplScreenState& s,
                                        int w = 120, int h = 40);

/// Overlay dialog (ToolPermission, Band3): centered floating dbox.
[[nodiscard]] Element RenderOverlayDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation = true,
    int w = 120, int h = 40);

/// Bottom slot: banner-style dialogs affixed above the prompt.
[[nodiscard]] Element RenderBottomDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120, int h = 40);

// Event dispatch in priority Standalone > Modal > Overlay > Bottom.
bool DispatchDialogQueueEvents(ReplScreenState& s,
                               const ftxui::Event& ev,
                               bool is_prompt_input_active,
                               bool allow_dialogs_with_animation);

/// Combine Overlay + Modal + Bottom into one dbox over the base chrome.
[[nodiscard]] Element LayerAllDialogs(
    Element base_chrome,
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120, int h = 40);

}  // namespace dialog_queue_render

/// Top-level dialog router: ReplMode -> overlay Element.  Legacy routing is
/// retired (M7); always returns nullopt today.
[[nodiscard]] std::optional<Element> RouteDialog(
    ReplMode m, const ReplScreenState& s);

// =========================================================
// Full layout composition
// (body in repl_screen_layout.cpp)
// =========================================================

/// Compose the REPL screen via the FullscreenLayout slot system and layer
/// the dialog queue on top.
[[nodiscard]] Element RenderReplScreen(ReplScreenState& s,
    std::function<void()> on_retry = nullptr,
    std::function<void()> on_clear_session = nullptr,
    ::cc::ui::StreamingMarkdown* streaming_md = nullptr);

// =========================================================
// Dialog routing: agent wizard / agents menu / settings / trust /
// tool-permission panels (bodies in repl_screen_agents.cpp and
// repl_screen_dialog_panels.cpp).
// =========================================================
namespace dialog_router {

// -------------------------------------------------------------------
// Agent wizard helpers
// -------------------------------------------------------------------

/// Lazily create (or re-create) the agent wizard component.
[[nodiscard]] std::shared_ptr<Component> get_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Forward an event to the agent wizard component.
bool forward_agent(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev);

/// Render the agent wizard content as an Element.
[[nodiscard]] Element render_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Reset (destroy) the agent wizard so the next entry starts fresh.
void reset_agent_wizard(const std::shared_ptr<ReplScreenState>& s);

// -------------------------------------------------------------------
// Agents menu helpers (UI13)
// -------------------------------------------------------------------

namespace agents_menu {

struct AgentMenuOptions {
    std::vector<cards::AgentCardData> agents;
    std::function<void()> on_create_new;
    std::function<void(const std::string& id)> on_select;
    std::function<void()> on_cancel;
};

[[nodiscard]] bool is_built_in(const cards::AgentCardData& agent);

[[nodiscard]] std::string resolved_model_label(
    const cards::AgentCardData& agent);

[[nodiscard]] std::vector<std::size_t> selectable_indices(
    const std::vector<cards::AgentCardData>& agents);

class AgentMenuListBase : public ComponentBase {
public:
    explicit AgentMenuListBase(AgentMenuOptions opts);

    Element Render() override;
    bool OnEvent(Event ev) override;

private:
    [[nodiscard]] static int selected_position_for_index(
        const std::vector<std::size_t>& selectable,
        std::size_t index);

    [[nodiscard]] static Element render_create_row(bool selected, Color suggestion);

    [[nodiscard]] static Element render_agent_row(
        const cards::AgentCardData& agent,
        bool selected,
        bool selectable,
        Color suggestion,
        Color muted);

    AgentMenuOptions opts_;
    int selected_position_ = 0;  // 0 is "Create new agent".
};

[[nodiscard]] Component AgentMenuList(AgentMenuOptions opts);

}  // namespace agents_menu

void close_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

[[nodiscard]] std::shared_ptr<Component> get_agents_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

[[nodiscard]] Element render_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

bool forward_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev);

// -------------------------------------------------------------------
// Settings dialog helpers (UI3)
// -------------------------------------------------------------------

/// Lazily create (or re-create) the settings dialog component.
[[nodiscard]] std::shared_ptr<Component> get_settings_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Reset (destroy) the settings component so the next entry starts fresh.
void reset_settings_component(const std::shared_ptr<ReplScreenState>& s);

/// Render the settings dialog content as an Element.
[[nodiscard]] Element render_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Forward an event to the settings dialog component.
bool forward_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev);

// -------------------------------------------------------------------
// Trust dialog helpers (UI8)
// -------------------------------------------------------------------

/// Lazily create (or re-create) the trust dialog component.
[[nodiscard]] std::shared_ptr<Component> get_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Render the trust dialog content as an Element.
[[nodiscard]] Element render_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

/// Forward an event to the trust dialog component.
bool forward_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev);

// -------------------------------------------------------------------
// Tool-permission rich panel helpers (dlg-permission-legacy)
// -------------------------------------------------------------------
// TS-faithful panels for the dormant ReplMode::ToolPermission branch.
// The tool-name classifier and its PermissionPanelKind enum are TU-local
// to repl_screen_dialog_panels.cpp.

/// Lazily build the panel, keyed on request identity.
[[nodiscard]] std::shared_ptr<Component> get_tool_permission_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

[[nodiscard]] Element render_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb);

bool forward_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb, Event ev);

}  // namespace dialog_router

// =========================================================
// FTXUI Component factory (bodies in repl_screen_events.cpp)
// =========================================================

/// Build the REPL screen as an FTXUI Component.
/// Engine updates the externally-held state between frames.
[[nodiscard]] Component ReplScreen(
    std::shared_ptr<ReplScreenState> state,
    ReplScreenCallbacks cbs);

/// Convenience: self-owned state (demos/tests only).
/// Production: use externally-held shared_ptr<ReplScreenState> overload.
[[nodiscard]] Component ReplScreen(ReplScreenCallbacks cbs);

}  // namespace cc::ui::repl_screen
