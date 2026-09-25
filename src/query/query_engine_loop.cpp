// Implementation unit for cc.query.query_engine — the top-level query
// state machine: blocking query(), streaming stream_query(), the internal
// tool-call loop, and the retry/fallback call_api wrapper. This is the
// ONLY implementation unit that imports cc.constants.cost_tracker (the
// global /cost sync).
module;

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.utils.error;
import cc.hooks.lifecycle_hooks;
import cc.constants.cost_tracker;

namespace cc::core {

[[nodiscard]] Result<QueryResponse> QueryEngine::query(
    std::string_view user_message,
    const QueryOptions& options) {
    auto start = std::chrono::steady_clock::now();

    // Reset abort flag for new query
    aborted_.store(false);

    // Check budget first
    if (budget_tracker_.budget_exceeded) {
        return std::unexpected(Error::make(
            ErrorCode::ContextWindowExceeded,
            std::format("Budget exceeded: ${:.4f} of ${:.4f}",
                budget_tracker_.current_spend_usd,
                budget_tracker_.max_budget_usd)));
    }

    // Construct user message and append to history
    auto msg = make_user_message(user_message, options.prompt_uuid);
    append_message(Message{std::move(msg)});

    // Execute the tool-call loop
    auto result = execute_tool_loop(options);
    if (!result) return std::unexpected(result.error());

    // Append the final assistant message to conversation for persistence
    append_message(Message{result->message});

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    auto& [response_msg, usage, rounds] = *result;
    (void)usage;
    (void)rounds;

    // Count this completed turn toward post-turn memory extraction and,
    // when enough new material exists, kick off the background extractor.
    messages_since_last_extraction_ += 2;  // user + assistant
    maybe_run_memory_extraction();

    return QueryResponse{
        std::move(response_msg),
        usage,
        rounds,
        elapsed,
        budget_tracker_.budget_exceeded,
        true,
        {}
    };
}

void QueryEngine::stream_query(
    std::string_view user_message,
    const QueryOptions& options) {
    if (budget_tracker_.budget_exceeded) {
        if (options.on_event) {
            StreamError err{"budget_exceeded",
                std::format("Budget exceeded: ${:.4f} of ${:.4f}",
                    budget_tracker_.current_spend_usd,
                    budget_tracker_.max_budget_usd)};
            (*options.on_event)(err);
        }
        return;
    }

    // Reset abort flag for new query
    aborted_.store(false);
    auto query_start_time = std::chrono::steady_clock::now();

    // Emit query start hook
    if (lifecycle_hooks_) {
        lifecycle_hooks_->emit_query_start(cc::hooks::QueryStartEvent{
            .query_text = std::string(user_message),
            .model = config_.model_params.model,
            .timestamp = std::chrono::system_clock::now()
        });
    }

    auto msg = make_user_message(user_message, options.prompt_uuid);
    // AT-02: append materialized @-mention attachments so the model sees
    // the referenced file contents (not the literal "@path" string).
    for (const auto& block : options.attachments) {
        msg.content.push_back(block);
    }
    append_message(Message{std::move(msg)});

    std::uint32_t round = 0;
    std::uint32_t continuation_count = 0;
    std::uint32_t max_tokens_retries = 0;
    const std::uint32_t max_rounds = options.max_tool_rounds.value_or(20);

    while (round < max_rounds && !should_abort() && !budget_tracker_.budget_exceeded) {
        // Check stop hooks before each iteration
        if (lifecycle_hooks_) {
            if (auto stop_reason = lifecycle_hooks_->check_stop_hooks()) {
                if (options.on_event) {
                    StreamError err{"hook_stop",
                        std::format("Query stopped by hook: {}", *stop_reason)};
                    (*options.on_event)(err);
                }
                break;
            }
        }

        append_pending_native_agent_notifications();
        // Stream a single API call
        auto stream_call = stream_single_api_call(options);
        if (stream_call.failed) {
            break;
        }
        auto& assistant_msg = stream_call.message;
        auto& round_usage = stream_call.usage;
        const bool has_tool_use = stream_call.has_tool_use;

        budget_tracker_.add_usage(round_usage, config_.model_params.model, model_cost_);
        cumulative_usage_ += round_usage;

        // Sync to global CostTracker so /cost reads real values
        global_cost_tracker().record(config_.model_params.model, round_usage,
            std::format("round_{}", round));

        append_message(Message{assistant_msg});

        // P1-3: Max output tokens recovery (streaming path)
        if (assistant_msg.stop_reason == "max_tokens" && max_tokens_retries < 3) {
            config_.model_params.max_tokens = std::max(config_.model_params.max_tokens, std::uint32_t{64000});
            auto resume = make_user_message(
                "Your previous response was cut off at the token limit. "
                "Please continue from where you left off.");
            append_message(Message{std::move(resume)});
            ++max_tokens_retries;
            ++round;
            continue;
        }

        // If no tool use requested, check for budget continuation
        if (!has_tool_use) {
            // P1-10: Token budget continuation (auto-continue when under budget)
            if (token_budget_ > 0 && continuation_count < 5) {
                auto used = cumulative_usage_.output_tokens;
                if (used < static_cast<std::uint32_t>(token_budget_ * 0.9)) {
                    auto pct = (used * 100) / token_budget_;
                    auto nudge = std::format(
                        "You've used {}% of the output budget ({}/{} tokens). "
                        "Keep working — do not summarize or stop early.",
                        pct, used, token_budget_);
                    auto cont_msg = make_user_message(nudge);
                    append_message(Message{std::move(cont_msg)});
                    ++continuation_count;
                    ++round;
                    continue;
                }
            }
            break;  // Done — no more tool calls, budget satisfied
        }

        // Execute tools and feed results back
        auto tool_results = execute_pending_tools(assistant_msg, options);
        for (auto& tr : tool_results) {
            append_message(Message{std::move(tr)});
        }

        ++round;
    }

    if (budget_tracker_.budget_exceeded && options.on_event) {
        (*options.on_event)(StreamError{"budget_exceeded", "Budget limit reached"});
    }

    // Emit query end hook
    if (lifecycle_hooks_) {
        auto query_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - query_start_time);
        lifecycle_hooks_->emit_query_end(cc::hooks::QueryEndEvent{
            .success = !budget_tracker_.budget_exceeded,
            .rounds = round,
            .tools_executed = round,  // Approximate
            .duration = query_duration,
            .timestamp = std::chrono::system_clock::now()
        });
    }
}

[[nodiscard]] Result<QueryEngine::ToolLoopResult> QueryEngine::execute_tool_loop(const QueryOptions& options) {
    TokenUsage loop_usage{};
    std::uint32_t round = 0;
    std::uint32_t max_tokens_retries = 0;
    const std::uint32_t max_rounds = options.max_tool_rounds.value_or(20);

    while (round < max_rounds && !should_abort() && !budget_tracker_.budget_exceeded) {
        // Check stop hooks before each iteration
        if (lifecycle_hooks_) {
            if (auto stop_reason = lifecycle_hooks_->check_stop_hooks()) {
                return std::unexpected(Error::make(
                    ErrorCode::InternalError,
                    std::format("Query stopped by hook: {}", *stop_reason)));
            }
        }

        append_pending_native_agent_notifications();
        auto api_result = call_api(options);
        if (!api_result) return std::unexpected(api_result.error());

        auto& [msg, usage] = *api_result;
        loop_usage += usage;
        cumulative_usage_ += usage;

        // Track budget
        budget_tracker_.add_usage(usage, config_.model_params.model, model_cost_);
        // Sync to global CostTracker so /cost reads real values
        global_cost_tracker().record(config_.model_params.model, usage,
            std::format("round_{}", round));
        if (budget_tracker_.budget_exceeded) {
            return std::unexpected(Error::make(
                ErrorCode::ContextWindowExceeded,
                "Budget limit exceeded"));
        }

        // P1-3: Max output tokens recovery
        if (msg.stop_reason == "max_tokens" && max_tokens_retries < 3) {
            // Escalate max_tokens to 64k
            config_.model_params.max_tokens = std::max(config_.model_params.max_tokens, std::uint32_t{64000});
            // Append partial response and a resume nudge
            append_message(Message{msg});
            auto resume = make_user_message(
                "Your previous response was cut off at the token limit. "
                "Please continue from where you left off.");
            append_message(Message{std::move(resume)});
            ++max_tokens_retries;
            ++round;
            continue;
        }

        // Check if response contains tool_use blocks
        bool has_tool_use = false;
        for (const auto& block : msg.content) {
            if (std::holds_alternative<ToolUseBlock>(block)) {
                has_tool_use = true;
                break;
            }
        }

        if (!has_tool_use) {
            // No more tool calls - return final response
            return ToolLoopResult{std::move(msg), loop_usage, round};
        }

        // Execute tools and append results
        append_message(Message{msg});
        auto results = execute_pending_tools(msg, options);
        for (auto& r : results) {
            append_message(Message{std::move(r)});
        }
        ++round;
    }

    return std::unexpected(Error::make(
        ErrorCode::InternalError,
        std::format("Tool loop exceeded maximum rounds ({})", max_rounds)));
}

[[nodiscard]] Result<QueryEngine::ApiCallResult> QueryEngine::call_api(const QueryOptions& options) {
    auto& policy = config_.retry_policy;
    auto delay = policy.initial_delay;
    std::size_t fallback_idx = 0;

    for (std::uint32_t attempt = 0; attempt <= policy.max_retries; ++attempt) {
        if (should_abort()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                "Query aborted"));
        }

        auto result = send_request(options);
        if (should_abort()) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError,
                "Query aborted"));
        }
        if (result) return *result;

        // Determine if error is retryable
        auto& err = result.error();
        bool retryable = is_retryable_error(err.code);

        // P1-4: Model fallback on capacity/overload errors
        if ((err.code == ErrorCode::OverloadedError || err.code == ErrorCode::RateLimited) &&
            fallback_idx < config_.fallback_models.size()) {
            config_.model_params.model = config_.fallback_models[fallback_idx++];
            continue;  // Retry immediately with fallback model
        }

        if (!retryable || attempt == policy.max_retries) {
            return std::unexpected(err);
        }

        // Add jitter to prevent thundering herd
        auto jittered_delay = add_jitter(delay);
        std::this_thread::sleep_for(jittered_delay);

        // Exponential backoff with cap
        delay = std::chrono::milliseconds(
            std::min<long long>(
                static_cast<long long>(delay.count() * policy.backoff_multiplier),
                policy.max_delay.count()));
    }

    return std::unexpected(Error::make(
        ErrorCode::InternalError,
        "Retry logic exhausted"));
}

} // namespace cc::core
