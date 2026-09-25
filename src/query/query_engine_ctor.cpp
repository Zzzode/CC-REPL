// Implementation unit for cc.query.query_engine — the QueryEngine
// constructor and API-client setup. This is the ONLY implementation unit
// that imports cc.services.analytics (the local-only session_start event),
// so that dependency never enters the module interface BMI.
module;

#include <cstdlib>

module cc.query.query_engine;

import std;

import cc.query.wire_protocol;
import cc.services.analytics;

namespace cc::core {

QueryEngine::QueryEngine(QueryEngineConfig config, ToolRegistry& registry)
    : config_(std::move(config)), tool_registry_(&registry),
      session_start_(std::chrono::system_clock::now()) {
    // Initialize session ID
    if (config_.session_id_override &&
        !config_.session_id_override->empty()) {
        session_id_.value = *config_.session_id_override;
    } else {
        session_id_.value = generate_session_id();
    }

    // Set up budget
    if (config_.max_budget_usd) {
        budget_tracker_.max_budget_usd = *config_.max_budget_usd;
    }

    // Initialize API client config
    setup_api_client();

    // Initialize conversation with system prompt if available
    build_and_add_system_prompt();

    // Record the session start in the local-only event log. The engine is
    // the one place every entry point (REPL, server, headless) passes
    // through, so emitting here means the log is actually populated rather
    // than being a writer nobody calls -- which is how the previous
    // analytics module ended up dead.
    cc::services::analytics::local_analytics().log_event(
        "session_start",
        {{"wire_api", std::string(cc::query::wire::wire_api_name(wire_api_))}});
}

void QueryEngine::setup_api_client() {
    api_config_.base_url = config_.base_url.value_or("https://api.anthropic.com");
    api_config_.api_key = config_.api_key;
    api_config_.auth_token = config_.auth_token;
    api_config_.api_version = "2023-06-01";
    api_config_.timeout = std::chrono::milliseconds{120000};
    api_config_.max_retries = static_cast<int>(config_.retry_policy.max_retries);
    api_config_.base_retry_delay = config_.retry_policy.initial_delay;

    // Select the wire protocol. Unset keeps the historical behaviour
    // (Anthropic /v1/messages). See cc.query.wire_protocol for the seam.
    wire_api_ = cc::query::wire::WireApi::Anthropic;
    if (config_.wire_api && !config_.wire_api->empty()) {
        if (auto parsed = cc::query::wire::wire_api_from_string(*config_.wire_api)) {
            wire_api_ = *parsed;
        }
    } else {
        // LOOM_WIRE_API is the documented name; the CC_REPL_WIRE_API
        // spelling is honoured as a fallback so a config written before the
        // rename keeps working. Neither spelling is a vendor name, so both
        // are safe to accept.
        const char* env = std::getenv("LOOM_WIRE_API");
        if (!env || !*env) env = std::getenv("CC_REPL_WIRE_API");
        if (env && *env) {
            if (auto parsed = cc::query::wire::wire_api_from_string(env)) {
                wire_api_ = *parsed;
            }
        }
    }
    native_computer_tool_ =
        config_.native_computer_tool.value_or(wire_api_ == cc::query::wire::WireApi::Anthropic);
}

} // namespace cc::core
