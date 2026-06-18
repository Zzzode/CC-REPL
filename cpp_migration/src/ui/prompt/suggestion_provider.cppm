/// @file suggestion_provider.cppm
/// @brief Adapter that surfaces real PromptSuggestionService results through the
///        interactive prompt UI's suggestion surface.
///
/// WORKSTREAM U1 - WIRING. The deterministic speculation/ranking engine in
/// cc.services.prompt_suggestion had no production consumer. This module is the
/// single seam between the service and the live prompt UI:
///
///   PromptSuggestionService::suggest(SuggestionRequest)
///        |
///        v
///   make_prompt_suggestion_provider(...)  <-- this module
///        |
///        v  (std::function matching TextInputOptions::get_suggestions)
///   uic::TextInput  -> RenderSuggestionsDropdown
///
/// Design notes:
///   - The TS engine surfaces speculative "what the user might type next"
///     suggestions primarily when the input buffer is EMPTY (ghost/next-action
///     suggestions). Slash-command and file autocomplete are driven by typing.
///     We mirror that: the provider returns speculative suggestions only when
///     the buffer is empty, and otherwise returns {} so the existing
///     command/file history providers own the typed-input path undisturbed.
///   - The provider is cheap and synchronous; suggest() is pure and offline.
///   - Conversation context (recent turns, open files, cwd) is captured by
///     reference into a shared ContextHandle so the host can update it each
///     render without rebuilding the provider or the TextInput component.
///   - Shell history is optional: if the host records shell executions into
///     the service, the service's collect_shell_suggestions surfaces a
///     "fix the failing command" suggestion automatically.
module;

#include <cstdlib>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <optional>
#include <chrono>

export module cc.ui.prompt.suggestion_provider;

import cc.types.types;
import cc.services.prompt_suggestion;
import ui.components.text_input;

export namespace cc::ui::prompt {

// text_input.cppm lives in the global `ui::components` namespace (not
// cc::ui::components). From inside cc::ui::prompt an unqualified
// `ui::components::X` would resolve to cc::ui::components::X, so alias the
// real namespace here.
namespace uic = ::ui::components;

// ============================================================
// Category mapping: services SuggestionSource -> UI SuggestionCategory
// ============================================================

/// Map a service-level SuggestionSource onto the UI dropdown's SuggestionCategory.
/// ConversationContext and Speculative suggestions are next-action prompts the
/// user is likely to type, so they surface as the generic History/None bucket
/// (there is no dedicated "ai/speculative" category in the UI enum). Filesystem
/// suggestions map to File; ShellHistory maps to Shell.
[[nodiscard]] inline uic::SuggestionCategory
to_ui_category(services::prompt_suggestion::SuggestionSource src) noexcept {
    namespace sp = services::prompt_suggestion;
    switch (src) {
        case sp::SuggestionSource::FileSystem:
            return uic::SuggestionCategory::File;
        case sp::SuggestionSource::ShellHistory:
            return uic::SuggestionCategory::Shell;
        case sp::SuggestionSource::CommandHistory:
            return uic::SuggestionCategory::History;
        // ConversationContext and Speculative both represent a next user turn;
        // the dropdown has no dedicated "speculative" bucket, so use History
        // (the closest visual semantic: a thing the user might type).
        case sp::SuggestionSource::ConversationContext:
        case sp::SuggestionSource::Speculative:
            return uic::SuggestionCategory::History;
    }
    return uic::SuggestionCategory::None;
}

/// Convert ONE service Suggestion into a UI dropdown Suggestion.
[[nodiscard]] inline uic::Suggestion
to_ui_suggestion(const services::prompt_suggestion::Suggestion& s) {
    uic::Suggestion out;
    out.text = s.text;
    // display_text == text: these are verbatim next-action prompts.
    out.display_text = s.text;
    out.description = s.description;
    out.category = to_ui_category(s.source);
    // Tag high-confidence speculative suggestions so they are visually
    // distinguishable in the dropdown (the renderer shows tags in yellow).
    if (s.source == services::prompt_suggestion::SuggestionSource::Speculative &&
        s.confidence >= 0.5) {
        out.tag = std::string{"suggested"};
    }
    return out;
}

// ============================================================
// ContextHandle
//
// Shared, mutable view of the live prompt context. The host updates the
// fields each render (cheap pointer writes); the provider reads them on
// demand when the TextInput asks for suggestions. Holding a shared_ptr keeps
// the provider stable across TextInput rebinds.
// ============================================================

/// Live context the suggestion engine reads. Updated in place by the host.
struct PromptSuggestionContext {
    /// Recent conversation turns (user + assistant), oldest-first. The host
    /// appends as the conversation grows; the ranker only inspects the tail.
    std::vector<services::prompt_suggestion::ConversationTurn> recent_turns;
    /// Current working directory, used by the file-suggestion collector.
    std::string current_directory;
    /// Open files in the editor / file tree, used to boost "Review <file>".
    std::vector<std::string> open_files;
    /// Maximum suggestions to surface in the dropdown.
    std::size_t max_suggestions{5};
    /// When false, the speculative ranker is skipped (only context/file/shell
    /// collectors run). Mirrors SuggestionRequest::include_speculative.
    bool include_speculative{true};
};

using ContextHandle = std::shared_ptr<PromptSuggestionContext>;

// ============================================================
// Provider factory
// ============================================================

/// Build a TextInputOptions::get_suggestions callback backed by a real
/// PromptSuggestionService.
///
/// @param service  Shared service instance. The host owns this (typically one
///                 per session) so shell history accumulates across turns.
/// @param ctx      Shared, mutable context handle (see PromptSuggestionContext).
/// @return A callback suitable for assignment to TextInputOptions::get_suggestions.
///
/// Behavior: when the input buffer is non-empty, returns {} (typed-input
/// autocomplete is owned by command/file/history providers). When the buffer
/// is empty, calls PromptSuggestionService::suggest() with the live context
/// and maps survivors into UI suggestions. On service error, returns {} (the
/// dropdown stays hidden; the user is never shown an error from this path).
[[nodiscard]] inline std::function<std::vector<uic::Suggestion>(
    const std::string&, int, const uic::PromptContext&)>
make_prompt_suggestion_provider(
    std::shared_ptr<services::prompt_suggestion::PromptSuggestionService> service,
    ContextHandle ctx)
{
    return [service = std::move(service), ctx = std::move(ctx)](
               const std::string& input, int /*cursor_pos*/,
               const uic::PromptContext& /*prompt_ctx*/)
               -> std::vector<uic::Suggestion> {
        // Only surface speculative/next-action prompts when the buffer is empty;
        // typed input is owned by the slash-command / file / history providers.
        if (!input.empty()) return {};
        if (!service || !ctx) return {};
        if (ctx->recent_turns.empty()) return {};

        services::prompt_suggestion::SuggestionRequest req;
        req.recent_turns = ctx->recent_turns;
        req.current_directory = ctx->current_directory;
        req.open_files = ctx->open_files;
        req.max_suggestions = ctx->max_suggestions;
        req.include_speculative = ctx->include_speculative;

        auto result = service->suggest(req);
        if (!result.has_value()) return {};

        std::vector<uic::Suggestion> out;
        out.reserve(result->size());
        for (const auto& s : *result) {
            out.push_back(to_ui_suggestion(s));
        }
        return out;
    };
}

/// Convenience: build a provider AND its context handle in one call. Useful for
/// hosts that do not need to share the context across multiple providers.
[[nodiscard]] inline auto make_prompt_suggestion_provider_with_context(
    std::shared_ptr<services::prompt_suggestion::PromptSuggestionService> service)
    -> std::pair<
        std::function<std::vector<uic::Suggestion>(
            const std::string&, int, const uic::PromptContext&)>,
        ContextHandle>
{
    auto ctx = std::make_shared<PromptSuggestionContext>();
    auto provider = make_prompt_suggestion_provider(std::move(service), ctx);
    return {std::move(provider), ctx};
}

} // namespace cc::ui::prompt
