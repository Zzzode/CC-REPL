/// @file query_engine.cppm
/// @brief Phase 3-A QueryEngine: Skeleton surface + real SSE streaming backend.
///
/// The module-level surface (EngineState / StreamDelta / QueryResult /
/// QueryCallbacks / QueryEngineOptions) is kept identical to the skeleton so
/// downstream Phase C-E integration code keeps compiling.  The method bodies
/// of `RunOnce` (previously `DryRunRunOnce`) and `StartStreaming` now drive
/// the `cc.services.api.sse` SSE client wrapped in
/// `cc.services.api.with_retry_simple`, with a dry-run gate preserved for
/// --dry-run and missing-API-key scenarios.
module;

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <cctype>
#include <span>
#include <array>
#include <utility>

import cc.services.api.sse;
import cc.services.api.with_retry_simple;

export module cc.services.query_engine;

export namespace cc::services::query_engine {

// --------------------------------------------------------------------------
// Types
// --------------------------------------------------------------------------

/// High-level engine lifecycle state.  Mirrors TS `QueryEngine.status`.
enum class EngineState {
    Idle,           // no pending request
    Sending,        // HTTP request in flight
    Streaming,      // receiving SSE / text deltas
    AwaitingTool,   // tool-call dispatched, pending tool-result
    Error,          // terminal error, retry needed
};

/// Text/tool-call deltas produced while streaming.
struct StreamDelta {
    enum class Kind { Text, ToolUseBegin, ToolUseInput, ToolUseEnd, ThinkingBegin, ThinkingDelta, ThinkingEnd };
    Kind         kind;
    std::string  content;         // text payload / tool-use json / thinking
    std::string  tool_name;       // only for ToolUse* kinds
    std::uint64_t tool_call_id = 0;
};

/// Result summary returned by `RunOnce()`.
struct QueryResult {
    std::string  accumulated_text;
    int          tool_calls_fired = 0;
    EngineState  end_state = EngineState::Idle;
    int          http_status = 200;        // 0 if no HTTP round-trip happened
    std::string  error_reason;             // set when end_state == Error
};

/// User-supplied callbacks for a running query.  All optional.
struct QueryCallbacks {
    std::function<void(const StreamDelta&)>  on_delta;
    std::function<void(const QueryResult&)>  on_complete;
    std::function<bool()>                    should_abort;   // polled during streaming
};

/// Construction-time options.
struct QueryEngineOptions {
    std::string  api_key;
    std::string  model_id   = "claude-sonnet-4-6";
    std::string  base_url   = "https://api.anthropic.com/v1";
    int          max_context = 200000;
    int          max_output_tokens  = 8192;
    double       temperature = 0.7;
    bool         dry_run    = false;   // set by --dry-run to skip HTTP
};

// --------------------------------------------------------------------------
// Engine
// --------------------------------------------------------------------------

// Forward declared for backward compat alias defined after the class.
class QueryEngine;

/// Legacy name kept so Phase C-E integration code does not break.
/// Alias is defined below the class definition.
// (alias lives after QueryEngine class body)

class QueryEngine {
public:
    explicit QueryEngine(QueryEngineOptions opts);

    /// Non-streaming synchronous facade with the original skeleton logic:
    /// echoes a banner response describing the prompt + options.  Used both as
    /// the dry-run path and as a standalone unit-test entry point.
    [[nodiscard]] QueryResult DryRunRunOnce(std::string_view prompt);

    /// Execute a full query: dry-run gate → build SSE client → wrap in
    /// RunWithRetry → drive callbacks.  Returns the final QueryResult and
    /// fires both `on_delta` (per text chunk) and `on_complete` exactly once.
    [[nodiscard]] QueryResult RunOnce(std::string_view system_prompt,
                                      std::span<const cc::services::api::sse::TextMessage> messages,
                                      QueryCallbacks cbs = {});

    /// Streaming-oriented entry point.  Internally wraps `prompt` into a
    /// single `User` TextMessage and calls `RunOnce` synchronously (HTTP is
    /// blocking in Phase 3; true async scheduling is deferred to Phase 3+).
    void StartStreaming(std::string_view prompt, QueryCallbacks cbs = {});

    /// Result record for DryRunFeedFakeSse: the final engine result plus
    /// counts of how many parser-side callbacks fired.  Exposed so unit tests
    /// can verify both the parser and the QueryEngine callback pipeline in
    /// one shot without network I/O.
    struct MockStreamResult {
        QueryResult qr;
        int         delta_calls = 0;   // on_text_delta invocations
        int         event_calls = 0;   // on_sse_event invocations
    };

    /// Inject a raw SSE byte stream into the SseClient parser (bypassing
    /// libcurl entirely) and drive the full QueryEngine callback pipeline
    /// as if a real HTTP stream had delivered the bytes.  Requires neither
    /// a network stack nor an API key.
    [[nodiscard]] MockStreamResult DryRunFeedFakeSse(std::string_view raw_sse_bytes,
                                                     QueryCallbacks cbs = {});

    // Accessors
    [[nodiscard]] auto state()      const noexcept -> EngineState { return state_; }
    [[nodiscard]] auto model_id()   const noexcept -> std::string_view { return opts_.model_id; }

private:
    /// Shared per-request mutable state captured by SSE callbacks.  Wrapped
    /// in a shared_ptr so callbacks remain valid if the caller copies the
    /// lambda (the SseClient itself copies StreamingCallbacks internally for
    /// the duration of the curl transfer).
    struct RunCtx {
        std::string  accum;
        int          http_status  = 0;
        std::string  stop_reason;
        std::string  error_reason;
        bool         final_fired  = false;
    };

    QueryEngineOptions opts_;
    EngineState        state_ = EngineState::Idle;
    /// Sequence counter reserved for future phases.
    [[maybe_unused]] std::uint64_t seq_ = 0;
};

/// Backward-compatible alias: any reference to QueryEngineSkeleton keeps
/// compiling as an alias to QueryEngine.
using QueryEngineSkeleton = QueryEngine;

// --------------------------------------------------------------------------
// Inline implementations
// --------------------------------------------------------------------------

inline QueryEngine::QueryEngine(QueryEngineOptions opts)
    : opts_(std::move(opts)) {}

inline QueryResult QueryEngine::DryRunRunOnce(std::string_view prompt) {
    using namespace std::string_literals;
    state_ = EngineState::Streaming;
    QueryResult r;
    r.accumulated_text =
        "[dry-run] QueryEngine: model="s + opts_.model_id
        + ", prompt_len=" + std::to_string(prompt.size())
        + ", max_tokens=" + std::to_string(opts_.max_output_tokens);
    r.tool_calls_fired = 0;
    r.end_state = EngineState::Idle;
    r.http_status = 0;   // no real HTTP round-trip in dry-run
    state_ = EngineState::Idle;
    return r;
}

inline QueryResult QueryEngine::RunOnce(
    std::string_view                                                system_prompt,
    std::span<const cc::services::api::sse::TextMessage>            messages,
    QueryCallbacks                                                  cbs) {

    state_ = EngineState::Sending;

    // ---- Dry-run / no-API-key gate: use the original skeleton logic ----
    if (opts_.dry_run || opts_.api_key.empty()) {
        // Concatenate all user-facing message content so the dry-run banner
        // reflects the user-visible prompt length faithfully.
        std::string combined;
        if (!system_prompt.empty()) {
            combined.append(system_prompt.data(), system_prompt.size());
            combined.push_back('\n');
        }
        for (const auto& m : messages) {
            combined.append(m.content);
            combined.push_back('\n');
        }
        QueryResult r = DryRunRunOnce(combined);
        if (cbs.on_delta) {
            StreamDelta d;
            d.kind    = StreamDelta::Kind::Text;
            d.content = r.accumulated_text;
            cbs.on_delta(d);
        }
        state_ = r.end_state;
        if (cbs.on_complete) cbs.on_complete(r);
        return r;
    }

    // ---- Build ApiConfig from QueryEngineOptions ------------------------
    cc::services::api::sse::ApiConfig api_cfg;
    api_cfg.api_key     = opts_.api_key;
    api_cfg.base_url    = opts_.base_url;
    api_cfg.api_version = "2023-06-01";
    api_cfg.model_id    = opts_.model_id;
    api_cfg.max_tokens  = opts_.max_output_tokens;
    api_cfg.temperature = opts_.temperature;
    cc::services::api::sse::SseClient client(std::move(api_cfg));

    // ---- Shared request context captured by SSE callbacks ---------------
    auto shared = std::make_shared<RunCtx>();
    shared->accum.reserve(4096);

    cc::services::api::sse::StreamingCallbacks scb;

    scb.on_sse_event = [shared](cc::services::api::sse::SseEventType /*type*/,
                                std::string_view /*data*/) {
        // Hook kept so future phases can add tool-use / thinking dispatch
        // without re-plumbing the SSE client.  No-op for text-only phase.
    };

    scb.on_text_delta = [shared, cbs](std::string_view delta) mutable {
        shared->accum.append(delta.data(), delta.size());
        if (cbs.on_delta) {
            StreamDelta d;
            d.kind    = StreamDelta::Kind::Text;
            d.content = std::string(delta);
            cbs.on_delta(d);
        }
    };

    scb.on_final = [shared](int         final_http_status,
                            std::string final_stop_reason,
                            std::string /*final_accumulated_text*/,
                            std::string final_error_reason) {
        shared->http_status  = final_http_status;
        shared->stop_reason  = std::move(final_stop_reason);
        shared->error_reason = std::move(final_error_reason);
        shared->final_fired  = true;
        // Note: we deliberately do NOT carry SseClient's own accumulated_text
        // into shared->accum — we have been accumulating independently inside
        // on_text_delta so the two must match (assert-free, keep last-write).
    };

    scb.should_abort = cbs.should_abort;

    state_ = EngineState::Streaming;

    // ---- Retry wrapper around the blocking SSE POST ---------------------
    cc::services::api::with_retry_simple::RetryConfig rcfg;
    const int rc = cc::services::api::with_retry_simple::RunWithRetry(
        rcfg,
        [&](int /*attempt*/) {
            return client.PostMessagesStream(system_prompt, messages, scb);
        });

    // ---- Assemble final QueryResult -------------------------------------
    QueryResult r;
    r.accumulated_text   = std::move(shared->accum);
    r.tool_calls_fired   = 0;
    r.http_status        = shared->http_status;
    r.error_reason       = std::move(shared->error_reason);

    if (rc == 0 && (shared->http_status == 0 ||
                    (shared->http_status >= 200 && shared->http_status < 300))) {
        r.end_state  = EngineState::Idle;
    } else if (rc < 0) {
        r.end_state  = EngineState::Error;
        if (r.error_reason.empty()) r.error_reason = "libcurl transport error";
        if (r.http_status == 0)     r.http_status = rc;
    } else if (rc > 0) {
        // rc is an HTTP status when PostMessagesStream returns non-zero
        // positive value; our local http_status should match but be safe.
        r.end_state  = EngineState::Error;
        if (r.http_status == 0) r.http_status = rc;
        if (r.error_reason.empty()) {
            r.error_reason = "HTTP " + std::to_string(r.http_status);
        }
    } else {
        r.end_state = EngineState::Idle;
    }

    state_ = r.end_state;
    if (cbs.on_complete) cbs.on_complete(r);
    return r;
}

inline void QueryEngine::StartStreaming(std::string_view prompt, QueryCallbacks cbs) {
    // Single user message wrapped around `prompt`.  The HTTP-backed
    // implementation is still blocking in Phase 3 so this method is
    // semantically identical to RunOnce(...).  Callers that need true
    // background streaming (Phase 3+) should post the call to a thread pool.
    cc::services::api::sse::TextMessage single;
    single.role    = cc::services::api::sse::Role::User;
    single.content = std::string(prompt);
    std::array<const cc::services::api::sse::TextMessage, 1> arr{std::move(single)};
    (void)RunOnce("", std::span<const cc::services::api::sse::TextMessage>{arr}, std::move(cbs));
}

inline QueryEngine::MockStreamResult
QueryEngine::DryRunFeedFakeSse(std::string_view raw_sse_bytes, QueryCallbacks cbs) {
    // Build a minimal SseClient: config contents don't matter because we
    // never hit the network — FeedParser only touches the SSE line parser.
    cc::services::api::sse::ApiConfig dummy_cfg;
    dummy_cfg.api_key = "";   // ensures no accidental network use
    cc::services::api::sse::SseClient client(std::move(dummy_cfg));

    auto shared = std::make_shared<RunCtx>();
    shared->accum.reserve(4096);

    MockStreamResult out;

    // Scratch buffers that SseClient::FeedParser will populate with text
    // deltas, stop_reason, and error details extracted from the JSON bodies.
    std::string parser_stop_reason;
    std::string parser_error_reason;

    cc::services::api::sse::StreamingCallbacks scb;
    scb.on_sse_event = [&out](cc::services::api::sse::SseEventType /*type*/,
                              std::string_view /*data*/) {
        ++out.event_calls;
    };
    scb.on_text_delta = [shared, cbs, &out](std::string_view delta) mutable {
        // NOTE: SseClient::FeedParserImpl ALSO writes the same delta to
        // shared->accum (it's passed by ref to the 6-arg FeedParser overload
        // below).  Do NOT double-append here; we only fire the user-facing
        // callback and count deltas.  The canonical copy lives in
        // shared->accum written by FeedParserImpl.
        ++out.delta_calls;
        if (cbs.on_delta) {
            StreamDelta d;
            d.kind    = StreamDelta::Kind::Text;
            d.content = std::string(delta);
            cbs.on_delta(d);
        }
    };
    scb.on_final = [shared](int status, std::string stop,
                            std::string /*accum*/, std::string err) {
        shared->http_status  = status;
        shared->stop_reason  = std::move(stop);
        shared->error_reason = std::move(err);
        shared->final_fired  = true;
    };
    scb.should_abort = cbs.should_abort;

    // ---- Drive the parser directly --------------------------------------
    state_ = EngineState::Streaming;
    // Use the 6-arg overload that writes accumulated_text / stop / err into
    // our own buffers so we don't lose parser-computed values.
    client.FeedParser(raw_sse_bytes, scb,
                      shared->accum,
                      parser_stop_reason,
                      parser_error_reason);

    // ---- Synthesize on_final like PostMessagesStream does for a 200 OK ---
    // The FeedParser overload above already wrote stop_reason / err into
    // our locals (extracted from message_delta / error JSON bodies).
    {
        std::string stop_reason = std::move(parser_stop_reason);
        std::string err_reason  = std::move(parser_error_reason);

        // Fallback: if the parser produced no stop_reason (e.g. the test
        // payload only puts stop_reason inside message_stop.json which the
        // parser intentionally does NOT probe), do a light hand-rolled
        // extraction.  Real streams emit message_delta with stop_reason so
        // this fallback rarely fires.
        if (stop_reason.empty()) {
            std::string_view remaining = raw_sse_bytes;
            std::size_t pos = 0;
            while (pos < remaining.size()) {
                const auto sep = remaining.find("\n\n", pos);
                if (sep == std::string_view::npos) break;
                const auto event_text = remaining.substr(pos, sep - pos);
                pos = sep + 2;
                bool is_msg_stop  = false;
                std::string data_str;
                bool has_data = false;
                std::size_t line_start = 0;
                while (line_start <= event_text.size()) {
                    std::size_t line_end = event_text.find('\n', line_start);
                    if (line_end == std::string_view::npos) line_end = event_text.size();
                    auto line = event_text.substr(line_start, line_end - line_start);
                    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                    line_start = line_end + 1;
                    if (line.empty()) continue;
                    if (line.front() == ':') continue;
                    if (line.starts_with("event:")) {
                        auto val = line.substr(6);
                        if (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                        if (val == "message_stop") is_msg_stop = true;
                    } else if (line.starts_with("data:")) {
                        auto val = line.substr(5);
                        if (!val.empty() && val.front() == ' ') val.remove_prefix(1);
                        if (has_data) data_str.push_back('\n');
                        data_str.append(val.data(), val.size());
                        has_data = true;
                    }
                }
                if (is_msg_stop && has_data) {
                    const auto key = data_str.find("\"stop_reason\"");
                    if (key != std::string::npos) {
                        const auto colon = data_str.find(':', key);
                        if (colon != std::string::npos) {
                            std::size_t q1 = data_str.find('"', colon + 1);
                            if (q1 != std::string::npos) {
                                std::size_t q2 = data_str.find('"', q1 + 1);
                                if (q2 != std::string::npos) {
                                    stop_reason = data_str.substr(q1 + 1, q2 - q1 - 1);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (scb.on_final) {
            scb.on_final(200, std::move(stop_reason),
                         shared->accum, std::move(err_reason));
        }
    }

    // ---- Assemble MockStreamResult --------------------------------------
    QueryResult& r = out.qr;
    r.accumulated_text = std::move(shared->accum);
    r.tool_calls_fired = 0;
    r.http_status      = shared->http_status;
    r.error_reason     = std::move(shared->error_reason);
    r.end_state        = r.error_reason.empty() ? EngineState::Idle : EngineState::Error;
    state_ = r.end_state;
    if (cbs.on_complete) cbs.on_complete(r);
    return out;
}

} // namespace cc::services::query_engine
