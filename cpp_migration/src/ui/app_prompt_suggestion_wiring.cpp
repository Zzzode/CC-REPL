// app_prompt_suggestion_wiring.cpp — impl unit for cc.ui.app. Keeps the heavy
// cc.services.prompt_suggestion import (1004-line service) OUT of app.cppm so
// app's BMI stays under clang's 2GB source-location budget — test_ui.cpp and
// other cc.ui importers would otherwise blow the budget transitively. Defines
// wire_prompt_suggestion_hook(), which is declared (not defined) in app.cppm.
module;

#include <memory>
#include <optional>
#include <string>
#include <variant>

module cc.ui.app;

import cc.types.types;
import cc.hooks.lifecycle_hooks;
import cc.services.prompt_suggestion;
import cc.ui.repl_screen;

namespace cc::ui {

// SL-11: register a QueryEnd hook that reads the engine conversation, runs the
// deterministic PromptSuggestionService ranker (no LLM / speculation — those
// subsystems don't exist in cpp), and stores the top suggestion on
// ReplScreenState::next_action_suggestion for the empty-prompt renderer.
void wire_prompt_suggestion_hook(cc::hooks::LifecycleHookRegistry& hooks,
                                 core::QueryEngine* engine,
                                 std::shared_ptr<repl_screen::ReplScreenState> state) {
    hooks.on_query_end([engine, state](const cc::hooks::QueryEndEvent& ev) {
        if (!ev.success) {
            state->next_action_suggestion.reset();
            return;
        }
        try {
            cc::services::prompt_suggestion::SuggestionRequest req;
            for (const auto& msg : engine->get_conversation()) {
                std::visit([&](const auto& m) {
                    using M = std::remove_cvref_t<decltype(m)>;
                    std::string role;
                    if constexpr (std::is_same_v<M, cc::core::UserMessage>)
                        role = "user";
                    else if constexpr (std::is_same_v<M, cc::core::AssistantMessage> ||
                                       std::is_same_v<M, cc::core::ToolUseMessage>)
                        role = "assistant";
                    else
                        return;  // System / ToolResult skipped
                    std::string body;
                    for (const auto& blk : m.content) {
                        if (auto* tb = std::get_if<cc::core::TextBlock>(&blk)) {
                            if (!body.empty()) body.push_back('\n');
                            body += tb->text;
                        }
                    }
                    if (body.empty()) return;
                    req.recent_turns.push_back(
                        {std::move(role), std::move(body), m.timestamp});
                }, msg);
            }
            req.max_suggestions = 1;
            req.include_speculative = true;
            const auto ranked =
                cc::services::prompt_suggestion::PromptSuggestionService{}.suggest(req);
            if (ranked && !ranked->empty() && !(*ranked)[0].text.empty()) {
                state->next_action_suggestion = (*ranked)[0].text;
            } else {
                state->next_action_suggestion.reset();
            }
        } catch (...) {
            state->next_action_suggestion.reset();
        }
    });
}

}  // namespace cc::ui
