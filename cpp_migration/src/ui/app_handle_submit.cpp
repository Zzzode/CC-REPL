// app_handle_submit.cpp — impl unit for AppAdapter::HandleSubmit and
// AppAdapter::HandleCommand, kept OUT of app_autocomplete.cpp to stay
// under clang's 2GB source-location budget.
//
// Contains: HandleSubmit (streaming query + @-mention materialization),
//           HandleCommand (slash command dispatch).
//
// Splitting these out removes 4 heavy imports (figures, at_attachments,
// message_pipeline, types) from app_autocomplete.cpp.
module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

module cc.ui.app;

// ── Base imports (shared with app_autocomplete.cpp) ─────────────────────
import cc.commands.registry;
import cc.tools.agent_runtime;
import cc.ui.autocomplete_sources;
import cc.ui.common.declared_cursor;
import cc.ui.prompt.file_index;
import cc.ui.prompt.fuzzy_rank_nucleo;
import cc.ui.repl_screen;
import cc.utils.debug;
import cc.utils.hyperlink;
import cc.utils.parse_references;
import cc.utils.path;
import cc.utils.skill_usage;
import cc.utils.session_storage;

// ── HandleSubmit-only imports (moved out of app_autocomplete.cpp) ───────
import cc.ui.design.figures;
import cc.ui.prompt.at_attachments;
import cc.ui.messages.message_pipeline;
import cc.types.types;
import cc.commands.command;
import cc.ui.dialogs.triggers;
import cc.vim.vim_mode;

namespace cc::ui {

namespace repl = cc::ui::repl_screen;
namespace agent_runtime = cc::tools::agent_runtime;
namespace acsrc = cc::ui::autocomplete_sources;
namespace frn = cc::ui::prompt::fuzzy_rank_nucleo;
namespace fidx = cc::ui::prompt::file_index;
namespace atatt = cc::ui::prompt::at_attachments;
namespace figs = cc::ui::design::figures;
namespace pl = cc::ui::messages::pipeline;
namespace dtrig = cc::ui::dialogs::triggers;

// ── HandleSubmit (moved out of app_autocomplete.cpp to reduce import closure) ──
void AppAdapter::HandleSubmit(const std::string& text,
                              repl::InputMode submit_mode) {
    // ── Wait for in-flight image pastes before snapshotting ──────────
    this->WaitForInFlightPastes(text);

    // TS REF: PromptInput.tsx L1066-1068
    const auto refs = cc::utils::parse_references(text);
    std::unordered_set<int> referenced_ids;
    int n_images = 0;
    for (const auto& r : refs) {
        if (pasted_contents_.contains(r.id)) {
            referenced_ids.insert(r.id);
            ++n_images;
        }
    }
    const bool has_images = n_images > 0;
    if (text.empty() && !has_images) return;

    if (text.starts_with('/')) {
        this->HandleCommand(text);
        return;
    }

    // P0-1: TS-equivalent bash (!) routing.
    const bool bash_by_mode   = submit_mode == repl::InputMode::Bash;
    const bool bash_by_prefix = text.starts_with('!');
    if (bash_by_mode || bash_by_prefix) {
        const std::string stripped(
            bash_by_prefix ? std::string(figs::strip_mode_prefix(text))
                           : text);
        if (!stripped.empty()) {
            this->RunLocalBashCommand(stripped);
            screen_state_->input_mode = repl::InputMode::Normal;
        }
        return;
    }

    if (query_running_.load()) return;

    screen_state_->submit_count++;

    // Persist to prompt history so @history / Ctrl+R can find this prompt.
    acsrc::append_prompt_history(text, current_session_id_, screen_state_->cwd);

    query_running_.store(true);
    last_submitted_text_ = text;
    screen_state_->spinner_mode = repl::SpinnerMode::Requesting;
    screen_state_->spinner_verb = "Thinking";
    {
        std::lock_guard lk(result_mutex_);
        pending_error_.reset();
        streaming_text_.clear();
        streaming_markdown_.reset();
        streaming_tools_.clear();
        streaming_thinking_.clear();
        event_dedup_.clear();
    }

    // Snapshot only still-referenced images for this submission.
    std::vector<ImageBlock> attachments;
    attachments.reserve(referenced_ids.size());
    for (int id : referenced_ids) {
        if (auto it = pasted_contents_.find(id); it != pasted_contents_.end()) {
            attachments.push_back(std::move(it->second));
        }
    }
    pasted_contents_.clear();

    // TS REF: history.ts L81 expandPastedTextRefs
    std::string expanded_text = cc::utils::expand_pasted_text_refs(
        text, [this](int id) -> std::optional<std::string> {
            auto it = pasted_text_contents_.find(id);
            if (it != pasted_text_contents_.end()) return it->second;
            return std::nullopt;
        });
    pasted_text_contents_.clear();

    query_thread_ = std::jthread([this, text = std::move(expanded_text), attachments = std::move(attachments)](std::stop_token st) {
        core::QueryOptions opts;
        for (const auto& img : attachments) {
            opts.attachments.push_back(img);
        }
        opts.on_event = [this, &st](const core::StreamEvent& ev) {
            if (st.stop_requested()) return;

            bool apply_event = true;

            std::visit([this, &apply_event](const auto& e) {
                using T = std::decay_t<decltype(e)>;

                if constexpr (std::is_same_v<T, core::StreamStart>) {
                    std::lock_guard lk(result_mutex_);
                    streaming_text_.clear();
                    streaming_markdown_.reset();
                    streaming_tools_.clear();
                    streaming_thinking_.clear();
                    event_dedup_.clear_indices();
                    apply_event = false;
                    return;
                }

                if constexpr (std::is_same_v<T, core::ContentBlockStart>) {
                    apply_event = event_dedup_.should_accept_start(e.index);
                    if (!apply_event) return;
                    std::lock_guard lk(result_mutex_);
                    if (const auto* tool = std::get_if<core::ToolUseBlock>(&e.block)) {
                        streaming_tools_[e.index] = StreamingToolPreview{
                            .tool_name = tool->name,
                            .tool_use_id = tool->id.value,
                            .input_json = tool->input_json == "{}" ? std::string{} : tool->input_json,
                            .result_preview = {},
                            .compact_preview = {},
                            .error_code = 0,
                            .truncated = false,
                            .complete = false,
                            .is_error = false,
                        };
                        screen_state_->spinner_mode = repl::SpinnerMode::ToolUse;
                        screen_state_->spinner_verb = tool->name;
                    } else if (const auto* thinking = std::get_if<core::ThinkingBlock>(&e.block)) {
                        streaming_thinking_[e.index] = StreamingThinkingPreview{
                            .text = thinking->thinking,
                            .complete = false,
                            .streaming_ended_at = std::nullopt,
                        };
                        screen_state_->spinner_mode = repl::SpinnerMode::Thinking;
                    }
                } else if constexpr (std::is_same_v<T, core::ContentBlockDelta>) {
                    apply_event = event_dedup_.should_accept_delta(e.index);
                    if (!apply_event) return;
                    std::lock_guard lk(result_mutex_);
                    if (auto tool = streaming_tools_.find(e.index); tool != streaming_tools_.end()) {
                        tool->second.input_json += e.delta_text;
                    } else if (auto thinking = streaming_thinking_.find(e.index); thinking != streaming_thinking_.end()) {
                        thinking->second.text += e.delta_text;
                    } else {
                        streaming_text_ += e.delta_text;
                        screen_state_->spinner_mode = repl::SpinnerMode::Responding;
                        screen_state_->spinner_verb = std::nullopt;
                    }
                } else if constexpr (std::is_same_v<T, core::ContentBlockStop>) {
                    apply_event = event_dedup_.should_accept_stop(e.index);
                    if (!apply_event) return;
                    std::lock_guard lk(result_mutex_);
                    if (auto tool = streaming_tools_.find(e.index); tool != streaming_tools_.end())
                        tool->second.complete = true;
                    if (auto thinking = streaming_thinking_.find(e.index); thinking != streaming_thinking_.end()) {
                        thinking->second.complete = true;
                        thinking->second.streaming_ended_at =
                            std::chrono::steady_clock::now();
                    }
                } else if constexpr (std::is_same_v<T, core::ToolExecutionStart>) {
                    apply_event = event_dedup_.should_accept_exec_start(e.tool_use_id);
                    if (!apply_event) return;
                    std::lock_guard lk(result_mutex_);
                    for (auto& [idx, preview] : streaming_tools_) {
                        if (preview.tool_use_id == e.tool_use_id &&
                            !preview.exec_done &&
                            preview.result_preview.empty()) {
                            preview.result_preview = "Starting…";
                            break;
                        }
                    }
                } else if constexpr (std::is_same_v<T, core::ToolExecutionProgress>) {
                    std::lock_guard lk(result_mutex_);
                    for (auto& [idx, preview] : streaming_tools_) {
                        if (preview.tool_use_id == e.tool_use_id && !preview.exec_done) {
                            preview.result_preview = e.partial_result;
                            break;
                        }
                    }
                } else if constexpr (std::is_same_v<T, core::ToolExecutionEnd>) {
                    apply_event = event_dedup_.should_accept_exec_end(e.tool_use_id);
                    if (!apply_event) return;
                    std::lock_guard lk(result_mutex_);
                    for (auto& [idx, preview] : streaming_tools_) {
                        if (preview.tool_use_id == e.tool_use_id) {
                            preview.result_preview = e.result;
                            preview.is_error       = e.is_error;
                            preview.exec_done      = true;
                            const auto aug = pl::augment_tool_result(
                                preview.result_preview, preview.is_error);
                            preview.truncated       = aug.truncated;
                            preview.error_code      = aug.error_code;
                            preview.compact_preview = std::move(aug.preview);
                            break;
                        }
                    }
                } else if constexpr (std::is_same_v<T, core::StreamError>) {
                    std::lock_guard lk(result_mutex_);
                    pending_error_ = e.message;
                }
            }, ev);

            if (apply_event) {
                PostRenderEvent();
            }
        };

        // AT-02: materialize @-mention file references into content blocks.
        auto materialized = atatt::materialize_at_mentions(text, screen_state_->cwd);
        for (auto& b : materialized.blocks) {
            opts.attachments.push_back(std::move(b));
        }

        engine_->stream_query(materialized.text, opts);

        query_running_.store(false);
        PostRenderEvent();
    });
}

// ── HandleCommand (moved out of app_autocomplete.cpp to reduce import closure) ──
void AppAdapter::HandleCommand(std::string_view cmd) {
    std::string command = trim_ascii_copy(cmd);
    std::string_view normalized = command;
    if (normalized.empty()) return;

    if (normalized == "/exit" || normalized == "/quit") {
        if (on_exit_) on_exit_();
        return;
    }
    if (normalized == "/clear") {
        engine_->clear_conversation();
        local_command_messages_.clear();
        screen_state_->divider_index.reset();
        screen_state_->unseen_divider.reset();
        screen_state_->unseen_message_count = 0;
        screen_state_->pill_visible = false;
        screen_state_->scroll_offset = 0;
        screen_state_->scroll_pinned_to_bottom = true;
        this->SyncState();
        return;
    }
    if (normalized == "/compact") {
        // P2 gap stashed-prompt: stash current input before compact so
        // the user's typed text survives the context compression.
        // TS REF: REPL.tsx — stashedPrompt preserves input across
        //   operations that would otherwise lose it (compact, tool-use).
        const auto& input = screen_state_->input_text;
        if (!input.empty()) {
            const auto refs = cc::utils::parse_references(input);
            std::unordered_map<int, ImageBlock> ref_images;
            std::unordered_map<int, std::string> ref_texts;
            for (const auto& r : refs) {
                if (auto it = pasted_contents_.find(r.id);
                    it != pasted_contents_.end()) {
                    ref_images[r.id] = it->second;
                }
                if (auto it = pasted_text_contents_.find(r.id);
                    it != pasted_text_contents_.end()) {
                    ref_texts[r.id] = it->second;
                }
            }
            repl::StashCurrentPrompt(screen_state_,
                std::move(ref_images), std::move(ref_texts));
            repl::set_prompt_input_text(screen_state_, {}, 0);
        }
        auto result = engine_->compact_conversation();
        if (result) {
            this->SyncState();
            // Restore stash after compact completes (TS: restore after
            // operation that triggered stash finishes).
            if (repl::HasStashedPrompt(screen_state_)) {
                std::unordered_map<int, ImageBlock> ri;
                std::unordered_map<int, std::string> rt;
                repl::RestoreStashedPrompt(screen_state_, &ri, &rt);
                for (auto& [id, img] : ri) pasted_contents_[id] = std::move(img);
                for (auto& [id, txt] : rt) pasted_text_contents_[id] = std::move(txt);
            }
        }
        return;
    }
    if (normalized == "/cost") {
        auto usage = engine_->get_usage();
        auto cost = engine_->budget_tracker().current_spend_usd;
        screen_state_->spinner_tip = std::format(
            "Cost: ${:.4f} | In: {} | Out: {} | Ctx: {:.0f}%",
            cost, usage.input_tokens, usage.output_tokens,
            engine_->context_utilization() * 100.0);
        return;
    }
    if (normalized.starts_with("/model")) {
        auto args_start = normalized.find(' ');
        if (args_start != std::string_view::npos) {
            auto new_model = normalized.substr(args_start + 1);
            auto params = engine_->model_params();
            std::string old_model = params.model;
            params.model = std::string(new_model);
            engine_->set_model_params(std::move(params));

            // M7.5: Show model switch confirmation banner
            dtrig::PushModelSwitch(
                screen_state_->dialog_queue,
                old_model,
                std::string(new_model),
                [this](bool confirm) {
                    (void)confirm; // always confirmed via /model command
                    screen_state_->dialog_queue.pop_bottom(
                        /*is_prompt_input_active=*/false);
                    PostRenderEvent();
                });
        }
        this->SyncState();
        return;
    }
    if (normalized.starts_with("/vim")) {
        auto args_start = normalized.find(' ');
        if (args_start == std::string_view::npos)
            vim_enabled_ = !vim_enabled_;
        else {
            auto arg = normalized.substr(args_start + 1);
            vim_enabled_ = (arg == "on" || arg == "1");
        }
        if (vim_enabled_) cc::vim::enable_vim_mode();
        else cc::vim::disable_vim_mode();
        this->TriggerStatuslineUpdate();
        return;
    }

    if (normalized == "/config" || normalized == "/settings") {
        screen_state_->mode = repl::ReplMode::SettingsView;
        screen_state_->settings_config = nullptr;
        return;
    }

    if (normalized == "/agents" || normalized == "/agents list") {
        this->OpenAgentsMenu();
        return;
    }

    if (normalized == "/skills") {
        this->OpenSkillsMenu();
        return;
    }

    if (auto parsed = cc::core::CommandRegistry::parse(normalized)) {
        const bool known_command =
            cmd_registry_ && cmd_registry_->has_command(parsed->name);
        if (!known_command) {
            if (auto skill =
                    acsrc::find_skill_suggestion(screen_state_->cwd, parsed->name)) {
                // SL-10: reject skills marked user_invocable=false (model-only).
                if (!skill->user_invocable) {
                    AppendLocalCommandInputMessage(std::string(normalized));
                    AppendLocalCommandMessage(
                        std::format("This skill can only be invoked by Claude, not "
                                    "directly by users. Ask Claude to use the \"{}\" skill.",
                                    skill->name),
                        true);
                    return;
                }
                std::string user_text;
                const auto args_start = normalized.find(' ');
                if (args_start != std::string_view::npos) {
                    user_text = trim_ascii_copy(normalized.substr(args_start + 1));
                }
                cc::utils::skill_usage::record_skill_usage(skill->name);  // SL-04
                this->HandleSubmit(acsrc::skill_invocation_prompt(*skill, user_text));
                return;
            }
            // SL-09: unknown-command file-path disambiguation
            const auto looks_like_command = [](std::string_view name) {
                return !name.empty() &&
                    name.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        "0123456789:-_") == std::string_view::npos;
            };
            if (looks_like_command(parsed->name)) {
                std::error_code ec;
                const std::filesystem::path candidate{"./" + parsed->name};
                if (std::filesystem::exists(candidate, ec) && !ec) {
                    AppendLocalCommandInputMessage(std::string(normalized));
                    AppendLocalCommandMessage(
                        std::format("\"/{}\" is not a command or skill, but \"{}\" "
                                    "exists in the current directory. If you meant to "
                                    "reference the file, drop the leading slash.",
                                    parsed->name, candidate.string()),
                        false);
                    return;
                }
            }
        }
    }

    if (cmd_registry_) {
        auto result = cmd_registry_->execute(
            command,
            command_context_for_engine(engine_, app_store_.get(), screen_state_->cwd));
        if (result) {
            if (result->status == CommandStatus::Injected) {
                this->HandleSubmit(result->message);
                return;
            }
            if (result->metadata == "EXIT" && on_exit_) {
                on_exit_();
                return;
            }
            // M7.5: Try to interpret command metadata as a dialog trigger.
            if (result->metadata && !result->metadata->empty()) {
                if (*result->metadata == "UI:permissions") {
                    screen_state_->settings_initial_tab = 3;
                    screen_state_->settings_component.reset();
                    screen_state_->mode = repl::ReplMode::SettingsView;
                    this->TriggerStatuslineUpdate();
                    PostRenderEvent();
                    return;
                }
                auto enqueue_fn = [this](std::string_view c) {
                    if (c.starts_with('/')) {
                        this->HandleCommand(c);
                    }
                };
                if (dtrig::PushFromCommandMetadata(
                        screen_state_->dialog_queue,
                        *result->metadata,
                        enqueue_fn))
                {
                    AppendCommandResult(*result);
                    PostRenderEvent();
                    return;
                }
            }
            AppendCommandResult(*result);
            return;
        }
        AppendLocalCommandMessage(result.error().message, true);
        return;
    } else {
        AppendLocalCommandMessage("Command registry is not available.", true);
    }
}

}  // namespace cc::ui
