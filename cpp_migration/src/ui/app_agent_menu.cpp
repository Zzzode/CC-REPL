// app_agent_menu.cpp — impl unit for AppAdapter methods kept OUT of
// app_autocomplete.cpp to stay under clang's 2GB source-location budget.
//
// Contains: FormatAgentsMenuOutput, LoadAgentCardsForMenu, SyncState,
//           ConsumePendingResult, WaitForInFlightPastes,
//           get_permission_callback, trigger_orphan_cleanup_for_testing.
//
// Splitting these out removes agent_display + agent_cards + dialogs.triggers
// + dialogs.system imports from app_autocomplete.cpp.
module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

module cc.ui.app;

// ── Base imports (shared with app_autocomplete.cpp) ─────────────────────
import cc.ui.repl_screen;
import cc.utils.session_storage;
import cc.utils.parse_references;
import cc.utils.debug;
import cc.tools.agent_runtime;

// ── Agent-menu-only imports (moved out of app_autocomplete.cpp) ─────────
import cc.tools.agent_display;
import cc.ui.agents.agent_cards;
import cc.ui.dialogs.triggers;
import cc.ui.dialogs.system;
import cc.hooks.cost_hook;

namespace cc::ui {

namespace repl = cc::ui::repl_screen;
namespace agent_runtime = cc::tools::agent_runtime;
namespace agent_cards = cc::ui::agents::cards;
namespace agent_display = cc::tools::agent_display;
namespace dtrig = cc::ui::dialogs::triggers;
namespace dsys = cc::ui::dialogs::system;

// ── FormatAgentsMenuOutput (moved out to remove agent_display import) ────
std::string AppAdapter::FormatAgentsMenuOutput(
    const std::vector<agent_cards::AgentCardData>& agents,
    int selected_position) {
    const auto selectable = selectable_agent_indices(agents);
    const int selectable_count = static_cast<int>(selectable.size());
    const int display_count = static_cast<int>(agents.size());
    const int item_count = std::max(1, selectable_count + 1);
    selected_position = std::clamp(selected_position, 0, item_count - 1);

    std::string out;
    out += "Agents\n";
    if (selectable_count == 0) {
        out += "No agents found\n";
    } else {
        out += std::format(
            "{} agent{}\n",
            display_count,
            display_count == 1 ? "" : "s");
    }

    out += "\n";
    out += selected_position == 0 ? "› Create new agent\n"
                                  : "  Create new agent\n";

    if (selectable_count == 0) {
        out += "\n";
        out += "No agents found. Create specialized subagents that Claude can delegate to.\n";
        out += "Each subagent has its own context window, custom system prompt, and specific tools.\n";
        out += "Try creating: Code Reviewer, Code Simplifier, Security Reviewer, Tech Lead, or UX Reviewer.\n";
    } else {
        int position = 1;
        for (const auto& group : agent_display::agent_source_groups()) {
            if (group.source == "built-in") continue;

            bool has_group = false;
            for (const auto& agent : agents) {
                if (agent.source == group.source) {
                    has_group = true;
                    break;
                }
            }
            if (!has_group) continue;

            out += "\n";
            out += group.label;
            out += "\n";
            for (const auto& agent : agents) {
                if (agent.source != group.source) continue;
                out += position == selected_position ? "› " : "  ";
                out += agent.name;
                const auto model = agent_model_label(agent);
                if (!model.empty()) {
                    out += " · ";
                    out += model;
                }
                out += "\n";
                ++position;
            }
        }
    }

    bool has_built_in = false;
    for (const auto& agent : agents) {
        if (is_built_in_agent(agent)) {
            has_built_in = true;
            break;
        }
    }
    if (has_built_in) {
        out += "\n────────────────────────────────────────────────────────────────\n\n";
        out += selectable_count == 0
            ? "Built-in (always available):\n"
            : "Built-in agents (always available)\n";
        for (const auto& agent : agents) {
            if (!is_built_in_agent(agent)) continue;
            out += agent.name;
            const auto model = agent_model_label(agent);
            if (!model.empty()) {
                out += " · ";
                out += model;
            }
            out += "\n";
        }
    }

    out += "\nPress ↑↓ to navigate · Enter to select · Esc to go back";
    return out;
}

// ── LoadAgentCardsForMenu (moved out to remove agent_runtime import) ─────
void AppAdapter::LoadAgentCardsForMenu() {
    std::optional<std::filesystem::path> cwd;
    if (!screen_state_->cwd.empty()) {
        cwd = std::filesystem::path(screen_state_->cwd);
    }

    auto definitions = agent_runtime::get_all_agent_definitions(std::move(cwd));
    std::ranges::sort(definitions, agent_display::CompareAgentsByName{});

    screen_state_->agent_cards.clear();
    screen_state_->agent_cards.reserve(definitions.size());
    for (const auto& definition : definitions) {
        screen_state_->agent_cards.push_back(
            project_agent_definition_card(definition));
    }
    screen_state_->agents_component.reset();
}

// ── SyncState (moved out to remove debug import) ─────────────────────────
void AppAdapter::SyncState() {
    auto messages = engine_->get_conversation();
    // TS Messages.tsx:520 collapse chain (background-bash so far).
    messages = ApplyMessageCollapsePipeline(std::move(messages));
    cc::utils::debug("app.sync",
        "SyncState: engine has {} messages", messages.size());
    screen_state_->messages.clear();
    screen_state_->messages.reserve(messages.size());
    // Assign a 24-char prefix per source Message.
    auto make_uuid24 = [](std::uint64_t msg_idx, const std::string& seed) {
        std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
        for (char c : seed) { h ^= static_cast<std::uint64_t>(c); h *= 1099511628211ULL; }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "msg_%012llx%08llx",
                      (unsigned long long)msg_idx,
                      (unsigned long long)(h & 0xFFFFFFFFULL));
        return std::string(buf, 24);
    };
    std::uint64_t msg_idx = 0;
    for (const auto& msg : messages) {
        if (std::holds_alternative<cc::core::SystemMessage>(msg)) continue;
        auto projected = project_messages(msg);
        std::string seed_preview;
        std::visit([&](const auto& m) {
            if constexpr (requires{ m.content; }) {
                for (const auto& blk : m.content) {
                    if (const auto* tb = std::get_if<cc::core::TextBlock>(&blk)) {
                        seed_preview += tb->text.substr(0, 64);
                        break;
                    }
                }
            }
        }, msg);
        const std::string u24 = make_uuid24(msg_idx, seed_preview);
        for (auto& e : projected) {
            e.id = u24;
            screen_state_->messages.push_back(std::move(e));
        }
        ++msg_idx;
    }
    AppendLocalMessagesToScreenState();
    std::stable_sort(
        screen_state_->messages.begin(),
        screen_state_->messages.end(),
        [](const repl::MessageDisplayEntry& a,
           const repl::MessageDisplayEntry& b) {
            return a.timestamp < b.timestamp;
        });

    // Debug: log projected message summary
    {
        std::size_t n_user = 0, n_asst = 0, n_sys = 0, n_tool = 0;
        for (const auto& e : screen_state_->messages) {
            if (e.role == "user") ++n_user;
            else if (e.role == "assistant") {
                ++n_asst;
                if (e.is_tool_use) ++n_tool;
            }
            else if (e.role == "system") ++n_sys;
        }
        cc::utils::debug("app.sync",
            "SyncState done: {} projected entries "
            "(user={}, asst={}, tool_use={}, sys={})",
            screen_state_->messages.size(),
            n_user, n_asst, n_tool, n_sys);
        for (std::size_t i = 0; i < screen_state_->messages.size(); ++i) {
            const auto& e = screen_state_->messages[i];
            if (e.role == "assistant" && !e.is_tool_use && !e.is_thinking) {
                cc::utils::debug("app.sync",
                    "  msg[{}] assistant text: len={}, streaming={}, preview='{}'",
                    i, e.content_preview.size(), e.is_streaming,
                    e.content_preview.substr(0, 80));
            }
        }
    }

    this->ProjectRuntimeMetadataToScreenState();

    // Notify cost hook subscribers (drives CostThreshold dialog, etc.).
    cc::hooks::update_cost(cc::hooks::CostUpdate{
        .session_cost = engine_->budget_tracker().current_spend_usd,
        .monthly_cost = 0.0,
        .input_tokens = screen_state_->status_bar.input_tokens,
        .output_tokens = screen_state_->status_bar.output_tokens,
    });
}

// ── ConsumePendingResult (moved out to remove debug import) ──────────────
void AppAdapter::ConsumePendingResult() {
    // Drain a completed local '!' bash command first.
    {
        std::optional<PendingBashResult> bash_res;
        {
            std::lock_guard lk(bash_result_mutex_);
            bash_res.swap(pending_bash_result_);
        }
        if (bash_res.has_value()) {
            AppendLocalCommandMessage(
                bash_res->output.empty()
                    ? std::string("(no output)")
                    : std::move(bash_res->output),
                bash_res->is_error);
        }
    }

    if (query_running_.load()) return;
    if (screen_state_->spinner_mode == repl::SpinnerMode::Hidden) return;

    cc::utils::debug("app.consume",
        "ConsumePendingResult firing — spinner_mode={}, calling SyncState",
        static_cast<int>(screen_state_->spinner_mode));

    std::lock_guard lk(result_mutex_);

    auto pending_error = std::move(pending_error_);
    pending_error_.reset();

    this->SyncState();
    if (pending_error && !pending_error->empty()) {
        repl::MessageDisplayEntry error_entry;
        error_entry.role = "system";
        error_entry.content_preview = "Error: " + *pending_error;
        error_entry.is_error = true;
        error_entry.timestamp = std::chrono::system_clock::now();
        error_entry.id = "err_00000000000000000000";
        error_entry.retry_after_ms = 3000.0;
        error_entry.retry_attempt  = 1;
        error_entry.max_retries    = 3;
        const auto& err = *pending_error;
        if (err.find("401") != std::string::npos ||
            err.find("unauthorized") != std::string::npos ||
            err.find("session") != std::string::npos ||
            err.find("expired") != std::string::npos) {
            error_entry.session_expired = true;
        }
        screen_state_->messages.push_back(std::move(error_entry));
    }
    screen_state_->spinner_mode = repl::SpinnerMode::Hidden;
    screen_state_->spinner_verb = std::nullopt;
    screen_state_->spinner_tip = std::nullopt;
    streaming_text_.clear();
    streaming_markdown_.reset();
    streaming_tools_.clear();

    if (storage_) {
        std::vector<cc::utils::Message> storage_msgs;
        for (const auto& msg : engine_->get_conversation()) {
            std::visit([&storage_msgs](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                std::string text;
                for (const auto& block : m.content) {
                    if (const auto* tb = std::get_if<cc::core::TextBlock>(&block))
                        text += tb->text;
                }
                if constexpr (std::is_same_v<T, cc::core::UserMessage>)
                    storage_msgs.push_back(cc::utils::UserMessage{{cc::utils::TextBlock{text}}});
                else if constexpr (std::is_same_v<T, cc::core::AssistantMessage>)
                    storage_msgs.push_back(cc::utils::AssistantMessage{{cc::utils::TextBlock{text}}});
            }, msg);
        }
        (void)storage_->save_session(current_session_id_, "Session", storage_msgs);
    }

    this->TriggerStatuslineUpdate();
}

// ── WaitForInFlightPastes (moved out to remove parse_references import) ──
void AppAdapter::WaitForInFlightPastes(const std::string& text) {
    const auto refs = cc::utils::parse_references(text);
    if (refs.empty()) return;
    std::unordered_set<int> needed;
    for (const auto& r : refs) needed.insert(r.id);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        this->ProcessCompletedPastes();
        bool still_waiting = false;
        for (int id : needed) {
            if (in_flight_pastes_.contains(id)) {
                still_waiting = true;
                break;
            }
        }
        if (!still_waiting) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    this->ProcessCompletedPastes();
}

// ── get_permission_callback (moved out to remove dialogs.system/triggers) ─
std::function<bool(std::string_view, std::string_view)> AppAdapter::get_permission_callback() {
    return [this](std::string_view tool_name, std::string_view description) -> bool {
        {
            std::lock_guard lk(permission_mutex_);
            if (always_allowed_tools_.contains(std::string(tool_name)))
                return true;

            dtrig::PushToolPermission(
                screen_state_->dialog_queue,
                std::string(tool_name),
                std::string(description),
                /*on_response=*/[this, tool_name = std::string(tool_name)](
                    dsys::ToolPermissionPayload::Decision decision,
                    bool /*sandbox*/)
                {
                    std::lock_guard lk(permission_mutex_);
                    bool allowed =
                        (decision == dsys::ToolPermissionPayload::Decision::AllowOnce ||
                         decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow);
                    if (decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow) {
                        always_allowed_tools_.insert(tool_name);
                    }
                    screen_state_->dialog_queue.pop_overlay();
                    permission_response_ = allowed;
                    permission_cv_.notify_one();
                },
                /*on_abort=*/[this] {
                    std::lock_guard lk(permission_mutex_);
                    screen_state_->dialog_queue.pop_overlay();
                    permission_response_ = false;
                    permission_cv_.notify_one();
                },
                /*can_always_allow=*/true);

            permission_response_.reset();
        }
        PostRenderEvent();

        std::unique_lock lk(permission_mutex_);
        permission_cv_.wait(lk, [this] { return permission_response_.has_value(); });
        bool allowed = *permission_response_;
        permission_response_.reset();
        return allowed;
    };
}

// ── trigger_orphan_cleanup_for_testing (moved out for parse_references) ──
void AppAdapter::trigger_orphan_cleanup_for_testing() {
    if (pasted_contents_.empty()) return;
    const auto refs = cc::utils::parse_references(screen_state_->input_text);
    std::unordered_set<int> referenced_ids;
    for (const auto& r : refs) referenced_ids.insert(r.id);
    for (auto it = pasted_contents_.begin(); it != pasted_contents_.end(); ) {
        if (!referenced_ids.contains(it->first)) {
            it = pasted_contents_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace cc::ui
