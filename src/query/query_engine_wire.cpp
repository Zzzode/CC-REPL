// Implementation unit for cc.query.query_engine — request construction:
// wire backend selection, neutral RequestInput assembly, body preparation,
// thinking/context-management gating, output_config JSON, task-budget
// accounting, and the working-directory/context-utilization accessors.
// This is the ONLY implementation unit that imports the two concrete wire
// backends and cc.utils.tool_deny_rules.
module;

#include <cstdlib>

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.utils.json;
import cc.utils.tool_deny_rules;
import cc.utils.env_utils;
import cc.query.wire_protocol;
import cc.query.wire_anthropic;
import cc.query.wire_openai;
import cc.services.compact.api_microcompact;

namespace cc::core {

double QueryEngine::context_utilization() const noexcept {
    auto estimated = estimate_conversation_tokens();
    return static_cast<double>(estimated) / config_.context_window.max_context_tokens;
}

std::string QueryEngine::working_directory() const {
    return config_.cwd.value_or(std::filesystem::current_path().string());
}

[[nodiscard]] std::optional<cc::services::compact::ContextManagementConfig>
QueryEngine::api_context_management() const {
    return cc::services::compact::get_api_context_management({
        .has_thinking = thinking_enabled_for_request(),
        .is_redact_thinking_active = false,
        .clear_all_thinking = false,
    });
}

[[nodiscard]] bool QueryEngine::thinking_enabled_for_request() const {
    return config_.thinking_config.mode != ThinkingConfig::Mode::Disabled &&
        !cc::utils::is_env_truthy(std::getenv("LOOM_DISABLE_THINKING"));
}

void QueryEngine::add_output_config_to_json(
    cc::utils::json::JsonMutVal& root,
    cc::utils::json::JsonMutDoc& doc
) const {
    const bool has_budget = config_.task_budget.has_value();
    const bool has_schema = config_.response_schema.has_value();
    if (!has_budget && !has_schema) return;

    auto output_config = doc.object();
    if (has_budget) {
        auto task_budget = doc.object();
        task_budget.add("type", doc.string("tokens"));
        task_budget.add("total", doc.number(static_cast<int64_t>(config_.task_budget->total)));
        if (config_.task_budget->remaining) {
            task_budget.add(
                "remaining",
                doc.number(static_cast<int64_t>(*config_.task_budget->remaining)));
        }
        output_config.add("task_budget", task_budget);
    }
    if (has_schema) {
        // Structured output: force the model to return JSON conforming to
        // the supplied JSON schema (Anthropic output_config.format.json_schema).
        auto format = doc.object();
        format.add("type", doc.string("json_schema"));
        auto schema_obj = doc.object();
        schema_obj.add("name", doc.string(config_.response_schema->name));
        auto schema_val = doc.raw_json(config_.response_schema->schema_json);
        if (schema_val.raw()) {
            schema_obj.add("schema", schema_val);
        }
        format.add("json_schema", schema_obj);
        output_config.add("format", format);
    }
    root.add("output_config", output_config);
}

[[nodiscard]] std::string QueryEngine::build_output_config_json_for_testing() const {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    add_output_config_to_json(root, doc);
    doc.set_root(root);
    return doc.to_string();
}

void QueryEngine::update_task_budget_remaining_after_compact(std::uint32_t pre_compact_tokens) {
    if (!config_.task_budget) return;
    const auto current = config_.task_budget->remaining.value_or(config_.task_budget->total);
    config_.task_budget->remaining =
        pre_compact_tokens >= current ? 0 : current - pre_compact_tokens;
}

void QueryEngine::restore_task_budget_remaining_from_compact_boundaries_locked() {
    if (!config_.task_budget || config_.task_budget->remaining) return;

    std::uint64_t compacted_tokens = 0;
    for (const auto& message : conversation_) {
        const auto* system = std::get_if<SystemMessage>(&message);
        if (!system || !system->subtype || *system->subtype != "compact_boundary" ||
            !system->compact_metadata) {
            continue;
        }
        compacted_tokens += system->compact_metadata->pre_tokens;
        if (compacted_tokens >= config_.task_budget->total) {
            config_.task_budget->remaining = 0;
            return;
        }
    }

    if (compacted_tokens > 0) {
        config_.task_budget->remaining =
            config_.task_budget->total - static_cast<std::uint32_t>(compacted_tokens);
    }
}

[[nodiscard]] std::unique_ptr<cc::query::wire::WireBackend>
QueryEngine::make_wire_backend() const {
    using cc::query::wire::WireApi;
    if (wire_api_ == WireApi::OpenAi) {
        std::vector<std::pair<std::string, std::string>> auth;
        if (!api_config_.auth_token.empty()) {
            auth.emplace_back("Authorization",
                              std::format("Bearer {}", api_config_.auth_token));
        } else if (!api_config_.api_key.empty()) {
            auth.emplace_back("Authorization",
                              std::format("Bearer {}", api_config_.api_key));
        }
        return std::make_unique<cc::query::wire::OpenAiWireBackend>(
            api_config_.base_url, std::move(auth));
    }
    // Anthropic (default): credential precedence matches the historical
    // engine behaviour — bearer token wins over x-api-key.
    std::vector<std::pair<std::string, std::string>> auth;
    auth.push_back(cc::query::wire::anthropic_credential_header(
        api_config_.api_key, api_config_.auth_token));
    cc::query::wire::AnthropicWireOptions opts;
    opts.base_url = api_config_.base_url;
    opts.api_version = api_config_.api_version;
    opts.extra_headers = std::move(auth);
    // API-side context edits / task budget / response schema ride on the
    // options struct because they are not part of the neutral input.
    if (auto cm = api_context_management()) {
        for (const auto& edit : cm->edits) {
            cc::query::wire::ContextEdit ce;
            ce.type = edit.type;
            ce.trigger_input_tokens = edit.trigger_input_tokens;
            ce.clear_at_least_input_tokens = edit.clear_at_least_input_tokens;
            ce.has_thinking_keep = edit.has_thinking_keep;
            ce.keep_all_thinking = edit.keep_all_thinking;
            ce.keep_thinking_turns = edit.keep_thinking_turns;
            ce.keep_tool_uses = edit.keep_tool_uses;
            ce.clear_tool_inputs = edit.clear_tool_inputs;
            ce.exclude_tools = edit.exclude_tools;
            opts.context_edits.push_back(std::move(ce));
        }
    }
    if (config_.task_budget) {
        cc::query::wire::TaskBudget tb;
        tb.total = config_.task_budget->total;
        tb.remaining = config_.task_budget->remaining;
        opts.task_budget = std::move(tb);
    }
    if (config_.response_schema) {
        cc::query::wire::ResponseSchema rs;
        rs.name = config_.response_schema->name;
        rs.schema_json = config_.response_schema->schema_json;
        opts.response_schema = std::move(rs);
    }
    return std::make_unique<cc::query::wire::AnthropicWireBackend>(
        std::move(opts));
}

[[nodiscard]] cc::query::wire::RequestInput QueryEngine::build_wire_input(
    const QueryOptions& options,
    bool stream) const {
    cc::query::wire::RequestInput input;
    input.model = config_.model_params.model;
    input.max_tokens = config_.model_params.max_tokens;
    input.stream = stream;
    input.temperature = config_.model_params.temperature;
    input.top_p = config_.model_params.top_p;
    input.top_k = config_.model_params.top_k;
    input.thinking_enabled = thinking_enabled_for_request();
    if (input.thinking_enabled &&
        config_.thinking_config.mode != ThinkingConfig::Mode::Adaptive) {
        input.thinking_budget_tokens =
            config_.thinking_config.budget_tokens.value_or(10000);
    }
    input.native_computer_tool = native_computer_tool_;
    const auto env_dim = [](const char* key, int64_t fallback) {
        if (const char* v = std::getenv(key); v && *v) {
            try {
                long long n = std::stoll(v);
                if (n > 0) return static_cast<int64_t>(n);
            } catch (...) {}
        }
        return fallback;
    };
    input.computer_display_width =
        env_dim("LOOM_COMPUTER_DISPLAY_WIDTH", 1024);
    input.computer_display_height =
        env_dim("LOOM_COMPUTER_DISPLAY_HEIGHT", 768);
    input.computer_display_number =
        env_dim("LOOM_COMPUTER_DISPLAY_NUMBER", 0);

    // MCP verbatim schemas (snapshotted once per request).
    if (config_.mcp_input_schema_provider) {
        for (auto& [name, schema] : config_.mcp_input_schema_provider()) {
            input.tool_schemas.emplace_back(std::move(name),
                                            std::move(schema));
        }
    }

    std::unordered_set<std::string> snipped_message_ids;
    {
        std::lock_guard lock(conversation_mutex_);
        for (const auto& msg : conversation_) {
            const auto* sys = std::get_if<SystemMessage>(&msg);
            if (!sys || !sys->snip_metadata) continue;
            for (const auto& uuid : sys->snip_metadata->removed_uuids) {
                snipped_message_ids.insert(uuid);
            }
        }
        for (const auto& msg : conversation_) {
            const auto message_id = std::visit(
                [](const auto& value) { return value.id.value; }, msg);
            if (snipped_message_ids.contains(message_id)) continue;

            if (const auto* sys = std::get_if<SystemMessage>(&msg)) {
                if (sys->subtype == "compact_boundary") continue;
                if (input.system_prompt.empty()) {
                    for (const auto& block : sys->content) {
                        if (const auto* tb = std::get_if<TextBlock>(&block)) {
                            input.system_prompt += tb->text;
                        }
                    }
                }
                continue;  // system messages never enter the messages array
            }
            input.messages.push_back(msg);
        }
    }

    // Tool pruning: deny rules, dedup (built-ins win), enabled filter.
    std::unordered_set<std::string> seen_names;
    auto add_tool = [&](const ToolDefinition& tool) {
        cc::utils::tool_deny_rules::DenyToolView deny_view;
        deny_view.name = tool.name;
        if (tool.category && tool.category->starts_with("mcp:")) {
            deny_view.mcp_server = tool.category->substr(4);
            deny_view.mcp_tool = tool.name;
        }
        if (cc::utils::tool_deny_rules::is_tool_denied(
                config_.always_deny_rules, deny_view)) {
            return;
        }
        if (!seen_names.insert(tool.name).second) return;
        if (!is_tool_enabled_for_query(tool.name, options)) return;
        input.tools.push_back(tool);
    };
    for (const auto& tool : config_.tools) add_tool(tool);
    if (config_.dynamic_tools_provider) {
        for (const auto& tool : config_.dynamic_tools_provider()) {
            add_tool(tool);
        }
    }
    return input;
}

[[nodiscard]] std::string QueryEngine::build_request_body(
    const QueryOptions& options,
    bool stream) const {
    auto backend = make_wire_backend();
    auto prepared = backend->prepare(build_wire_input(options, stream));
    if (!prepared) {
        // Backends only fail on unusable configuration (e.g. an empty base
        // URL). Falling back to an empty object keeps the request path
        // from throwing; the HTTP layer reports the real error.
        return "{}";
    }
    return prepared->body;
}

} // namespace cc::core
