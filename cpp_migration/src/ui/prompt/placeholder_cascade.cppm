/// @file placeholder_cascade.cppm
/// @brief 4-tier memoized placeholder cascade: mode-specific → skill hint → default.
///
/// Faithful TS→CPP port of:
///   - src/components/PromptInput/usePromptInputPlaceholder.ts (76 lines)
///   - src/hooks/renderPlaceholder.ts (51 lines)
///   - src/components/PromptInput/PromptInput.tsx line 2014 (AI suggestion override)
///
/// TS cascade priority:
///   L1. input !== ''                              → undefined
///   L2. viewingAgentName                          → "Message @{name}..." (trunc 20)
///   L3. editable queued cmds + shown < 3          → "Press up to edit queued messages"
///   L4. submitCount < 1 + suggestions enabled     → getExampleCommandFromCache()
///
/// Plus PromptInput.tsx layer (applied before L2 in our merged logic):
///   L0. showPromptSuggestion && promptSuggestion  → promptSuggestion (AI override)
///
/// Rendering (renderPlaceholder.ts):
///   - hidePlaceholderText + cursor + focus → invert(' ')  (cursor block only)
///   - cursor + focus + terminalFocus      → invert(placeholder[0]) + dim(rest)
///   - no cursor / no focus                → dim(full placeholder)
///   - value.length === 0 && placeholder   → showPlaceholder = true

module;

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt.placeholder_cascade;

import cc.ui.common.types;  // PromptInputMode

export namespace cc::ui::placeholder {

using namespace ftxui;

// ── Constants (TS REF: usePromptInputPlaceholder.ts) ──────────────────────

/// Maximum teammate/agent display name length before truncation.
/// TS REF: usePromptInputPlaceholder.ts:23 MAX_TEAMMATE_NAME_LENGTH = 20
inline constexpr int kMaxTeammateNameLength = 20;

/// Number of times the queue hint can be shown before being suppressed.
/// TS REF: usePromptInputPlaceholder.ts:22 NUM_TIMES_QUEUE_HINT_SHOWN = 3
inline constexpr int kQueueHintMaxShowCount = 3;

/// Example commands for the onboarding placeholder (L4 of cascade).
/// TS REF: src/utils/exampleCommands.ts getExampleCommandFromCache()
/// The TS version samples from git history; we use a static list sufficient
/// for the placeholder UX without requiring git access at render time.
inline constexpr std::array<std::string_view, 6> kExampleCommands = {
    "fix lint errors",
    "how do I log an error?",
    "write a test for main",
    "refactor main.cpp",
    "explain this code",
    "/help",
};

// ── Context struct ────────────────────────────────────────────────────────

/// Input context for the placeholder cascade.
/// Decoupled from ReplScreenState so the cascade logic is reusable by
/// standalone TextInputImpl instances, dialogs, and other widgets.
struct PlaceholderContext {
    /// Current input buffer text.  Non-empty → no placeholder (L1).
    std::string_view input_text;

    /// Current input mode (Normal / Bash / VimInsert / etc).
    /// AI suggestion override (L0) only applies in Normal mode.
    cc::ui::common::PromptInputMode input_mode =
        cc::ui::common::PromptInputMode::Normal;

    /// Viewing agent/teammate name.  When set and input is empty,
    /// placeholder becomes "Message @{name}..." (L2).
    std::optional<std::string_view> viewing_agent_name;

    /// Number of user submissions (messages sent).
    /// Onboarding example (L4) shown only when submit_count < 1.
    int submit_count = 0;

    /// How many times the "Press up to edit queued messages" hint has been shown.
    /// Capped at kQueueHintMaxShowCount (3) in TS.
    int queued_hint_shown_count = 0;

    /// True when the command queue holds user-editable pending commands.
    /// TS REF: isQueuedCommandEditable check.
    bool has_editable_queued = false;

    /// Whether prompt suggestions (AI next-action hints) are enabled.
    /// Maps to TS AppState.promptSuggestionEnabled.
    bool prompt_suggestion_enabled = true;

    /// AI-generated next-action suggestion text (PromptInput.tsx:2014 layer).
    /// Applied as L0 override when:
    ///   - input_mode == Normal
    ///   - non-empty and does not start with '/'
    ///   - autocomplete_suggestions_empty
    ///   - no viewing_agent_name
    std::optional<std::string_view> next_action_suggestion;

    /// Whether autocomplete suggestions dropdown is empty.
    /// AI suggestion (L0) only shows when no autocomplete items are visible.
    bool autocomplete_suggestions_empty = true;
};

// ── ComputePlaceholder ────────────────────────────────────────────────────

/// Pick an example command string.  Uses a deterministic index based on
/// submit_count so the example changes occasionally but doesn't flicker
/// on every re-render.  TS REF: getExampleCommandFromCache uses memoize()
/// to sample once per session; we approximate with submit_count modulo.
[[nodiscard]] inline std::string GetExamplePlaceholder(int submit_count) {
    const std::size_t idx =
        static_cast<std::size_t>(submit_count) % kExampleCommands.size();
    return std::string("Try \"") + std::string(kExampleCommands[idx]) + "\"";
}

/// Compute the contextual placeholder via the 4-tier cascade + AI override.
/// Returns std::nullopt when no placeholder should be shown.
///
/// TS REF: usePromptInputPlaceholder.ts (useMemo cascade)
/// TS REF: PromptInput.tsx:2014 (showPromptSuggestion ? promptSuggestion : defaultPlaceholder)
[[nodiscard]] inline std::optional<std::string> ComputePlaceholder(
    const PlaceholderContext& ctx) {

    // ── L1: input non-empty → no placeholder ──────────────────────────────
    // TS REF: usePromptInputPlaceholder.ts:33-35
    if (!ctx.input_text.empty()) return std::nullopt;

    // ── L0 (AI suggestion override, applied before cascade tiers) ─────────
    // TS REF: PromptInput.tsx:2014
    //   const placeholder = showPromptSuggestion && promptSuggestion
    //       ? promptSuggestion
    //       : defaultPlaceholder;
    // where showPromptSuggestion = mode === 'prompt' && suggestions.length === 0
    //   && promptSuggestion && !viewingAgentTaskId
    // NOTE: next_action_suggestion starting with '/' is a slash-command
    // suggestion handled by autocomplete separately — don't use as placeholder.
    if (ctx.input_mode == cc::ui::common::PromptInputMode::Normal &&
        ctx.next_action_suggestion.has_value() &&
        !ctx.next_action_suggestion->empty() &&
        ctx.next_action_suggestion->front() != '/' &&
        ctx.autocomplete_suggestions_empty &&
        !ctx.viewing_agent_name.has_value()) {
        return std::string(*ctx.next_action_suggestion);
    }

    // ── L2: viewing teammate → "Message @{name}..." ──────────────────────
    // TS REF: usePromptInputPlaceholder.ts:38-44
    if (ctx.viewing_agent_name.has_value() &&
        !ctx.viewing_agent_name->empty()) {
        std::string display_name(*ctx.viewing_agent_name);
        if (static_cast<int>(display_name.size()) > kMaxTeammateNameLength) {
            display_name =
                display_name.substr(0, kMaxTeammateNameLength - 3) + "...";
        }
        return "Message @" + display_name + "…";
    }

    // ── L3: queued commands hint ─────────────────────────────────────────
    // TS REF: usePromptInputPlaceholder.ts:49-55
    if (ctx.has_editable_queued &&
        ctx.queued_hint_shown_count < kQueueHintMaxShowCount) {
        return "Press up to edit queued messages";
    }

    // ── L4: onboarding example ───────────────────────────────────────────
    // TS REF: usePromptInputPlaceholder.ts:60-66
    // NOTE: TS also checks !proactiveModule?.isProactiveActive() — we omit
    // the proactive-mode gate here because the CPP engine controls this via
    // prompt_suggestion_enabled (set false in proactive mode) which is
    // equivalent in effect.
    if (ctx.submit_count < 1 && ctx.prompt_suggestion_enabled) {
        return GetExamplePlaceholder(ctx.submit_count);
    }

    // ── Fallback: no placeholder ─────────────────────────────────────────
    return std::nullopt;
}

// ── RenderPlaceholder ─────────────────────────────────────────────────────

/// Result of rendering a placeholder string into an FTXUI Element.
struct RenderedPlaceholder {
    /// The rendered element (or nullopt if nothing to show).
    std::optional<Element> element;
    /// Whether the placeholder should be shown at all (value empty + has text).
    bool show_placeholder = false;
};

/// Helper: extract the first UTF-8 code point from a string.
/// Returns the first char + its byte length (1 for ASCII, more for multibyte).
[[nodiscard]] inline std::pair<std::string, std::size_t> FirstUtf8Codepoint(
    std::string_view s) {
    if (s.empty()) return {"", 0};
    std::string result{s[0]};
    std::size_t len = 1;
    // Continuation bytes: 0x80–0xBF
    while (len < s.size() &&
           (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) {
        result.push_back(s[len]);
        ++len;
    }
    return {result, len};
}

/// Render a placeholder string with the declared-cursor visual treatment.
///
/// TS REF: src/hooks/renderPlaceholder.ts
///
/// @param placeholder  The placeholder text (may be empty/nullopt).
/// @param value        Current input value (placeholder only shown when empty).
/// @param show_cursor  Whether the cursor block should be visible.
/// @param focused      Whether the input widget has focus.
/// @param terminal_focus  Whether the terminal itself is focused.
/// @param hide_text    When true, hide placeholder text and show only cursor
///                     (used in voice recording mode — TS hidePlaceholderText).
/// @param prefix       Optional prefix text to prepend (e.g. "❯ ").
/// @param prefix_color Optional color for the prefix glyph.
[[nodiscard]] inline RenderedPlaceholder RenderPlaceholder(
    std::optional<std::string_view> placeholder,
    std::string_view value,
    bool show_cursor,
    bool focused,
    bool terminal_focus = true,
    bool hide_text = false,
    std::string_view prefix = "",
    std::optional<Color> prefix_color = std::nullopt) {

    // TS REF: renderPlaceholder.ts:45
    //   const showPlaceholder = value.length === 0 && Boolean(placeholder)
    const bool has_text = placeholder.has_value() && !placeholder->empty();
    const bool show_placeholder = value.empty() && has_text;

    // If nothing to show at all, return empty.
    if (!show_placeholder && !hide_text) {
        return {std::nullopt, false};
    }

    Elements ph_parts;

    // Optional prefix (e.g. "❯ ").
    if (!prefix.empty()) {
        auto prefix_el = ftxui::text(std::string(prefix));
        if (prefix_color.has_value()) {
            prefix_el = prefix_el | color(*prefix_color);
        }
        ph_parts.push_back(std::move(prefix_el));
    }

    // TS REF: renderPlaceholder.ts:27-43
    if (hide_text) {
        // TS REF: renderPlaceholder.ts:28-31
        // Voice recording: show only the cursor, no placeholder text.
        if (show_cursor && focused && terminal_focus) {
            ph_parts.push_back(ftxui::text(" ") | inverted |
                               color(Color::White));
        }
        // else: empty string, nothing to render
    } else if (show_cursor && focused && terminal_focus) {
        // TS REF: renderPlaceholder.ts:36-41
        // Invert first character (declared cursor) + dim the rest.
        const auto [first_ch, byte_len] = FirstUtf8Codepoint(*placeholder);
        if (!first_ch.empty()) {
            ph_parts.push_back(ftxui::text(first_ch) | inverted |
                               color(Color::White));
            const std::string rest(placeholder->substr(byte_len));
            if (!rest.empty()) {
                ph_parts.push_back(ftxui::text(rest) | dim |
                                   color(Color::GrayLight));
            }
        } else {
            // Empty placeholder with cursor: show solid block.
            ph_parts.push_back(ftxui::text(" ") | inverted |
                               color(Color::White));
        }
    } else {
        // TS REF: renderPlaceholder.ts:33
        // No cursor / not focused: dim the full placeholder text.
        ph_parts.push_back(ftxui::text(std::string(*placeholder)) | dim);
    }

    if (ph_parts.empty()) {
        return {std::nullopt, show_placeholder};
    }

    return {hbox(ph_parts), show_placeholder};
}

}  // namespace cc::ui::placeholder
