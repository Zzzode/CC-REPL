// Implementation unit for cc.query.query_engine — tool dispatch and
// permission enforcement: execute_single_tool (lifecycle + user hooks and
// the registry call), the std::async fan-out in execute_pending_tools,
// error-result construction, the permission-hook check, native-agent
// notification draining, and the discovered-skills/denial accessors.
module;

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.types.tool_types;
import cc.utils.json;
import cc.hooks.tool_permissions;
import cc.hooks.lifecycle_hooks;
import cc.utils.hooks_registry;
import cc.utils.hooks_execution;
import cc.tools.agent_runtime;

namespace cc::core {

std::vector<std::string> QueryEngine::discovered_skills() const {
    std::lock_guard lock(state_mutex_);
    return {discovered_skills_.begin(), discovered_skills_.end()};
}

std::vector<PermissionDenial> QueryEngine::get_permission_denials() const {
    std::lock_guard lock(state_mutex_);
    return permission_denials_;
}

void QueryEngine::append_pending_native_agent_notifications() {
    for (auto& notification : cc::tools::agent_runtime::native_agent_store().take_pending_task_notifications()) {
        auto msg = make_user_message(notification);
        append_message(Message{std::move(msg)});
    }
}

std::vector<ToolResultMessage> QueryEngine::execute_pending_tools(
    const AssistantMessage& msg,
    const QueryOptions& options) {

    // Collect all tool_use blocks
    std::vector<const ToolUseBlock*> tool_uses;
    for (const auto& block : msg.content) {
        auto* tool_use = std::get_if<ToolUseBlock>(&block);
        if (tool_use) tool_uses.push_back(tool_use);
    }

    if (tool_uses.empty()) return {};

    // Check if all tools are read-only (safe for parallel execution)
    static const std::unordered_set<std::string> readonly_tools{
        "Read", "Glob", "Grep", "LS", "FileRead"
    };

    bool all_readonly = tool_uses.size() > 1 &&
        std::ranges::all_of(tool_uses, [](const ToolUseBlock* tu) {
            return readonly_tools.contains(tu->name);
        });

    if (all_readonly) {
        // P1-5: Parallel execution for read-only tools
        std::vector<std::future<ToolResultMessage>> futures;
        futures.reserve(tool_uses.size());

        for (const auto* tool_use : tool_uses) {
            futures.push_back(std::async(std::launch::async,
                [this, tool_use, &options]() -> ToolResultMessage {
                    return execute_single_tool(*tool_use, options);
                }));
        }

        std::vector<ToolResultMessage> results;
        results.reserve(futures.size());
        for (auto& f : futures) {
            results.push_back(f.get());
        }
        return results;
    }

    // Sequential execution for non-readonly or single tools
    std::vector<ToolResultMessage> results;
    for (const auto* tool_use : tool_uses) {
        results.push_back(execute_single_tool(*tool_use, options));
    }
    return results;
}

[[nodiscard]] ToolResultMessage QueryEngine::make_tool_error_result(
    const ToolUseBlock& tool_use,
    std::string message) {
    ToolResultMessage result_msg{};
    result_msg.id.value = generate_id();
    result_msg.timestamp = std::chrono::system_clock::now();
    result_msg.tool_use_id = tool_use.id;
    result_msg.tool_name = tool_use.name;
    result_msg.is_error = true;
    result_msg.content.push_back(TextBlock{std::move(message)});
    return result_msg;
}

[[nodiscard]] ToolResultMessage QueryEngine::execute_single_tool(
    const ToolUseBlock& tool_use,
    const QueryOptions& options) {
    if (!is_tool_enabled_for_query(tool_use.name, options)) {
        return make_tool_error_result(
            tool_use,
            std::format("Tool disabled for this query: {}", tool_use.name));
    }

    auto permission = check_tool_permission(tool_use.name, tool_use.input_json, tool_use.id.value);
    std::string effective_input_json = permission.updated_input_json.value_or(tool_use.input_json);
    if (!permission.allowed) {
        // Record denial
        {
            std::lock_guard lock(state_mutex_);
            permission_denials_.push_back(PermissionDenial{
                tool_use.name,
                tool_use.id.value,
                tool_use.input_json});
        }

        return make_tool_error_result(
            tool_use,
            permission.message.value_or(std::format("Permission denied for tool: {}", tool_use.name)));
    }

    // Emit pre-tool-use hook and check for blocking hooks
    auto exec_start = std::chrono::steady_clock::now();

    // M6: emit tool-execution-start stream event for live result preview UI.
    // This lets the UI show a progress line as soon as the tool starts
    // executing, instead of waiting for the full API round-trip.
    if (options.on_event) {
        ToolExecutionStart ev;
        ev.tool_use_id = tool_use.id.value;
        ev.tool_name = tool_use.name;
        ev.input_json = effective_input_json;
        (*options.on_event)(ev);
    }

    if (lifecycle_hooks_) {
        auto block_reason = lifecycle_hooks_->check_and_emit_pre_tool_use(cc::hooks::PreToolUseEvent{
            .tool_name = tool_use.name,
            .tool_input_json = effective_input_json,
            .tool_use_id = tool_use.id.value,
            .timestamp = std::chrono::system_clock::now()
        });
        if (block_reason) {
            // Hook denied the tool execution
            {
                std::lock_guard lock(state_mutex_);
                permission_denials_.push_back(PermissionDenial{
                    tool_use.name,
                    tool_use.id.value,
                    effective_input_json});
            }
            return make_tool_error_result(
                tool_use,
                std::format("Hook denied tool execution: {}", *block_reason));
        }
    }

    // User-configured PreToolUse hooks (cc.utils.hooks_execution engine).
    // Mirrors src/services/tools/toolExecution.ts:884-946 where the TS
    // engine runs executePreToolHooks before tool execution and honors
    // BlockToolCall/AbortQuery by denying permission. Guarded so behavior
    // is unchanged when no user hooks are configured.
    if (user_hooks_configured_ && !user_hooks_.empty()) {
        namespace he = cc::utils::hooks_execution;
        he::HookExecutionContext ctx = user_hooks_ctx_template_;
        // Matcher needs ctx["tool"]["name"]; materialise an owned doc.
        std::string tool_doc = std::string("{\"name\": \"") +
            he::json_escape(tool_use.name) + "\"}";
        if (auto td = cc::utils::json::parse(tool_doc)) {
            ctx.set_context_doc("tool", std::move(*td));
        }
        // Payload mirrors TS `toolInput`/processedInput passthrough.
        if (auto pd = cc::utils::json::parse(effective_input_json)) {
            ctx.set_payload_doc(std::move(*pd));
        }

        auto [modified_payload, action] = he::run_api_query_hooks(
            user_hooks_,
            cc::utils::hooks_registry::HookEventType::PreToolUse,
            ctx,
            effective_input_json);

        using HEAction = he::HookResponseAction;
        if (action.action == HEAction::BlockToolCall ||
            action.action == HEAction::AbortQuery) {
            {
                std::lock_guard lock(state_mutex_);
                permission_denials_.push_back(PermissionDenial{
                    tool_use.name,
                    tool_use.id.value,
                    effective_input_json});
            }
            std::string reason = action.reason.empty()
                ? std::string("user PreToolUse hook blocked the tool call")
                : action.reason;
            return make_tool_error_result(tool_use, std::move(reason));
        }
        // hookUpdatedInput passthrough (toolHooks.ts:556-563): if the hook
        // produced a modified payload distinct from the input, use it.
        // `modified_payload` (the pair's first element) carries any
        // prompt-passthrough merged with the hook's modified_payload.
        if (action.action == HEAction::RetryWithModifiedPayload &&
            !modified_payload.empty() &&
            modified_payload != effective_input_json) {
            effective_input_json = modified_payload;
        }
    }

    // Execute via registry
    auto input = ToolInput::from_json(effective_input_json);
    auto exec_result = tool_registry_->execute(tool_use.name, input);

    ToolResultMessage result_msg{};
    result_msg.id.value = generate_id();
    result_msg.timestamp = std::chrono::system_clock::now();
    result_msg.tool_use_id = tool_use.id;
    result_msg.tool_name = tool_use.name;

    if (exec_result) {
        // In-loop skill dispatch tracking: record each invoked skill so the
        // session knows which skills have been loaded (de-dup / telemetry).
        // The actual skill content load happens in execute_skill_tool; this
        // populates the previously-dead discovered_skills_ set.
        if (tool_use.name == "skill") {
            if (auto parsed = cc::utils::json::parse(effective_input_json)) {
                auto name_val = parsed->root().get("name");
                if (!name_val.is_str()) name_val = parsed->root().get("skill");
                if (name_val.is_str()) {
                    std::lock_guard lock(state_mutex_);
                    discovered_skills_.insert(std::string(name_val.as_str()));
                }
            }
        }
        auto& tr = *exec_result;
        result_msg.is_error = tr.is_error;
        for (auto& content : tr.content) {
            if (content.format && *content.format == "image" && content.media_type && content.data) {
                // NOTE: width/height/size_bytes are not returned by the
                // tool-use executor (it only forwards format/media_type/data).
                // Left as std::nullopt; user-facing renderer falls back to
                // "<no metadata>" text and an ASCII thumbnail seeded from
                // the base64 payload.
                ImageBlock ib;
                ib.media_type = std::move(*content.media_type);
                ib.data       = std::move(*content.data);
                ib.source     = ImageBlockSource::Unknown;
                result_msg.content.push_back(std::move(ib));
                continue;
            }
            if (content.format && *content.format == "document" && content.media_type && content.data) {
                result_msg.content.push_back(DocumentBlock{
                    .media_type = std::move(*content.media_type),
                    .data = std::move(*content.data),
                });
                continue;
            }
            // P1-11: Truncate large outputs with disk spill
            if (content.text.size() > 30000) {
                auto truncated = content.text.substr(0, 30000);
                truncated += "\n\n[Output truncated at 30000 chars. Full output omitted.]";
                result_msg.content.push_back(TextBlock{std::move(truncated)});
            } else {
                result_msg.content.push_back(TextBlock{std::move(content.text)});
            }
        }
    } else {
        result_msg.is_error = true;
        result_msg.content.push_back(TextBlock{exec_result.error().format()});
    }

    // Build the output preview once and reuse it for both the in-process
    // lifecycle event bus and the user-configured hook engine.
    std::string output_preview;
    if (!result_msg.content.empty()) {
        if (const auto* tb = std::get_if<TextBlock>(&result_msg.content[0])) {
            output_preview = tb->text.substr(0, 500);
        }
    }

    // M6: emit tool-execution-end stream event with the final result.
    // Completes the live result preview UI.
    if (options.on_event) {
        ToolExecutionEnd ev;
        ev.tool_use_id = tool_use.id.value;
        ev.result = output_preview;
        ev.is_error = result_msg.is_error;
        (*options.on_event)(ev);
    }

    // Emit post-tool-use lifecycle event
    if (lifecycle_hooks_) {
        auto exec_end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start);
        lifecycle_hooks_->emit_post_tool_use(cc::hooks::PostToolUseEvent{
            .tool_name = tool_use.name,
            .tool_use_id = tool_use.id.value,
            .is_error = result_msg.is_error,
            .output_preview = output_preview,
            .duration = duration,
            .timestamp = std::chrono::system_clock::now()
        });
    }

    // User-configured PostToolUse hooks (cc.utils.hooks_execution engine).
    // Mirrors src/services/tools/toolExecution.ts:1567-1577 where the TS
    // engine runs executePostToolHooks after the tool executes. On
    // BlockToolCall/AbortQuery the action reason is surfaced as an error
    // on the result (TS runPostToolUseHooks blockingError,
    // toolHooks.ts:105-115).
    if (user_hooks_configured_ && !user_hooks_.empty()) {
        namespace he = cc::utils::hooks_execution;
        he::HookExecutionContext ctx = user_hooks_ctx_template_;
        std::string tool_doc = std::string("{\"name\": \"") +
            he::json_escape(tool_use.name) + "\"}";
        if (auto td = cc::utils::json::parse(tool_doc)) {
            ctx.set_context_doc("tool", std::move(*td));
        }
        auto act = he::execute_post_tool_hooks(
            user_hooks_,
            ctx,
            tool_use.name,
            effective_input_json,
            output_preview);
        using HEAction = he::HookResponseAction;
        if (act.action == HEAction::BlockToolCall ||
            act.action == HEAction::AbortQuery) {
            result_msg.is_error = true;
            std::string reason = act.reason.empty()
                ? std::string("user PostToolUse hook blocked the result")
                : act.reason;
            result_msg.content.clear();
            result_msg.content.push_back(TextBlock{std::move(reason)});
        }
    }

    return result_msg;
}

[[nodiscard]] QueryEngine::ToolPermissionCheck QueryEngine::check_tool_permission(
    std::string_view tool_name,
    std::string_view input_json,
    std::string_view tool_use_id) {
    if (!permission_hook_) {
        // No hook configured — allow all (auto-approve mode)
        ToolPermissionCheck check{};
        check.allowed = true;
        return check;
    }

    permission_hook_->set_current_tool_use_id(tool_use_id);
    auto response = permission_hook_->can_use_response(tool_name, input_json);
    permission_hook_->clear_current_tool_use_id();
    const bool allowed = response.decision == cc::hooks::PermissionDecision::allow ||
                         response.decision == cc::hooks::PermissionDecision::allow_once;
    return ToolPermissionCheck{
        .allowed = allowed,
        .updated_input_json = std::move(response.updated_input_json),
        .message = std::move(response.message),
    };
}

} // namespace cc::core
