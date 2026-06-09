/// @file core.cppm
/// @brief Env-less remote bridge core: session creation, credential fetch,
///        SSE transport, token refresh, 401 recovery, teardown, and archive.
///
/// "Env-less" = no Environments API layer. Connects directly to the
/// session-ingress layer without the work-dispatch poll/ack lifecycle.
///
/// Flow:
///   1. POST /v1/code/sessions              (OAuth, no env_id) -> session.id
///   2. POST /v1/code/sessions/{id}/bridge  (OAuth)            -> worker_jwt, expires_in, api_base_url, worker_epoch
///   3. createV2ReplTransport(worker_jwt, worker_epoch)        -> SSE + CCRClient
///   4. createTokenRefreshScheduler                             -> proactive /bridge re-call
///   5. 401 on SSE -> rebuild transport with fresh /bridge credentials
///
/// Migrated from src/bridge/remoteBridgeCore.ts (~1008 lines).
module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module cc.bridge.core;

import cc.types.types;
import cc.bridge.config;
import cc.bridge.transport;
import cc.bridge.security;
import cc.bridge.init;
import cc.bridge.ui;
import cc.bridge.envless_config;
import cc.bridge.bridge_messaging;
import cc.bridge.session_api;
import cc.bridge.session_id_compat;
import cc.bridge.debug_utils;
import cc.bridge.flush_gate;
import cc.bridge.messages;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;

// =========================================================================
// Poll interval configuration (pollConfigDefaults.ts)
// =========================================================================

/// Poll interval configuration for bridge transport.
struct PollIntervalConfig {
    std::chrono::milliseconds normal{1000};
    std::chrono::milliseconds idle{5000};
    std::chrono::milliseconds reconnect_min{500};
    std::chrono::milliseconds reconnect_max{30000};
    std::chrono::milliseconds heartbeat{30000};
};

/// Get default poll config
PollIntervalConfig get_poll_config_defaults() {
    return PollIntervalConfig{};
}

// =========================================================================
// EnvLessBridgeParams — initialization parameters
// =========================================================================

/// Callback type: attempt OAuth refresh for a stale token. Returns true
/// if a fresh token was obtained.
using AuthRefreshCallback = std::function<std::expected<void, std::string>(std::string_view stale_token)>;

/// Callback type: maps internal SDKMessage vector to JSON events for
/// writeMessages / initial flush.
using ToSDKMessagesCallback = std::function<std::vector<std::string>(const std::vector<SDKMessage>&)>;

/// Callback type: fire on each title-worthy user message until the
/// callback returns true (done). Caller owns the derive policy.
using UserMessageCallback = std::function<bool(const std::string& text, const std::string& session_id)>;

/// Callback type: max thinking tokens change.
using SetMaxThinkingTokensCallback = std::function<void(std::optional<int64_t>)>;

/// Callback type: permission mode change. Returns {ok, error_message}.
using SetPermissionModeCallback = std::function<std::pair<bool, std::string>(const std::string&)>;

/// Initialization parameters for the env-less remote bridge core.
/// Analogous to the TS EnvLessBridgeParams type.
struct EnvLessBridgeParams {
    std::string base_url;
    std::string org_uuid;
    std::string title;
    std::function<std::optional<std::string>()> get_access_token;
    AuthRefreshCallback on_auth_401;
    ToSDKMessagesCallback to_sdk_messages;
    int64_t initial_history_cap = 0;
    std::vector<SDKMessage> initial_messages;
    InboundMessageCallback on_inbound_message;
    UserMessageCallback on_user_message;
    PermissionResponseCallback on_permission_response;
    InterruptCallback on_interrupt;
    SetModelCallback on_set_model;
    SetMaxThinkingTokensCallback on_set_max_thinking_tokens;
    SetPermissionModeCallback on_set_permission_mode;
    StateChangeCallback on_state_change;
    bool outbound_only = false;
    std::vector<std::string> tags;
};

// =========================================================================
// EnvLessBridgeConfig — runtime config for the env-less bridge
// =========================================================================

/// Runtime configuration for the env-less bridge. These values come from
/// GrowthBook feature flags / defaults and are passed into
/// initEnvLessBridgeCore.
struct EnvLessBridgeConfig {
    int64_t http_timeout_ms{15000};
    int64_t heartbeat_interval_ms{20000};
    double heartbeat_jitter_fraction{0.1};
    int64_t token_refresh_buffer_ms{300000};     // 5 minutes before expiry
    int64_t connect_timeout_ms{15000};
    int64_t teardown_archive_timeout_ms{1500};
    size_t uuid_dedup_buffer_size{2000};
    int init_retry_max_attempts{3};
    int64_t init_retry_base_delay_ms{500};
    double init_retry_jitter_fraction{0.25};
    int64_t init_retry_max_delay_ms{5000};
};

/// Retrieve the default bridge config. In a full integration this would
/// come from GrowthBook; for now it returns the defaults.
EnvLessBridgeConfig get_env_less_bridge_config() {
    return EnvLessBridgeConfig{};
}

// =========================================================================
// ConnectCause — telemetry discriminator for ws_connected events
// =========================================================================

enum class ConnectCause { initial, proactive_refresh, auth_401_recovery };

// =========================================================================
// build_ccr_v2_sdk_url — build the v2 session URL
// =========================================================================

/// Build a CCR v2 session URL from the API base URL and session ID.
/// Returns an HTTP(S) URL pointing at /v1/code/sessions/{id}.
[[nodiscard]] inline std::string build_ccr_v2_sdk_url(
    std::string_view api_base_url,
    std::string_view session_id
) {
    std::string base(api_base_url);
    while (!base.empty() && base.back() == '/') base.pop_back();
    return std::format("{}/v1/code/sessions/{}", base, session_id);
}

// =========================================================================
// withRetry — exponential backoff with jitter
// =========================================================================

/// Retry an operation with exponential backoff and jitter.
/// Returns the result on first success, or std::nullopt after exhausting
/// all attempts.
template <typename T>
std::optional<T> with_retry(
    std::function<std::optional<T>()> fn,
    const std::string& label,
    const EnvLessBridgeConfig& cfg
) {
    const int max = cfg.init_retry_max_attempts;
    for (int attempt = 1; attempt <= max; ++attempt) {
        auto result = fn();
        if (result.has_value()) return result;
        if (attempt < max) {
            auto base = static_cast<double>(cfg.init_retry_base_delay_ms) *
                        static_cast<double>(1 << (attempt - 1));
            std::random_device rd;
            std::mt19937 gen(rd());
            double jitter = base * cfg.init_retry_jitter_fraction *
                            (2.0 * std::generate_canonical<double, 32>(gen) - 1.0);
            double capped = base + jitter < static_cast<double>(cfg.init_retry_max_delay_ms)
                ? base + jitter : static_cast<double>(cfg.init_retry_max_delay_ms);
            auto delay_ms = static_cast<int64_t>(capped);
            log_bridge_event("retry", {
                {"label", label},
                {"attempt", std::to_string(attempt)},
                {"max", std::to_string(max)},
                {"delay_ms", std::to_string(delay_ms)},
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    return std::nullopt;
}

// =========================================================================
// TokenRefreshScheduler — proactive JWT refresh before expiry
// =========================================================================

/// Periodically refreshes the access token before it expires.
/// Fires the refresh callback at (expires_in - refresh_buffer) intervals,
/// re-scheduling itself after each successful refresh.
class TokenRefreshScheduler {
    std::chrono::milliseconds refresh_buffer_ms_;
    std::function<std::expected<void, std::string>(std::string_view)> refresh_access_token_fn_;
    std::function<void(const std::string&, const std::string&)> on_refresh_;
    std::string label_;

    std::mutex mutex_;
    std::jthread timer_thread_;
    std::string active_session_id_;
    int64_t active_expires_in_s_{0};

public:
    struct Params {
        std::chrono::milliseconds refresh_buffer_ms{std::chrono::minutes{5}};
        std::function<std::expected<void, std::string>(std::string_view)> get_access_token_async;
        std::function<void(const std::string&, const std::string&)> on_refresh;
        std::string label = "default";
    };

    explicit TokenRefreshScheduler(Params params)
        : refresh_buffer_ms_(params.refresh_buffer_ms)
        , refresh_access_token_fn_(std::move(params.get_access_token_async))
        , on_refresh_(std::move(params.on_refresh))
        , label_(std::move(params.label))
    {}

    ~TokenRefreshScheduler() { cancel_all(); }

    /// Schedule a refresh after the given expires_in seconds.
    void schedule_from_expires_in(const std::string& session_id, int64_t expires_in_s) {
        std::lock_guard lock(mutex_);
        active_session_id_ = session_id;
        active_expires_in_s_ = expires_in_s;

        if (timer_thread_.joinable()) {
            timer_thread_.request_stop();
            timer_thread_.join();
        }

        auto delay_s = expires_in_s - refresh_buffer_ms_.count() / 1000;
        if (delay_s <= 0) delay_s = 5; // minimum 5s
        auto delay = std::chrono::seconds(delay_s);

        timer_thread_ = std::jthread([this, sid = session_id, delay](std::stop_token stop) {
            // Wait for delay or stop
            auto deadline = std::chrono::steady_clock::now() + delay;
            while (!stop.stop_requested()) {
                auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::seconds{0}) break;
                auto chunk = remaining < std::chrono::seconds{1} ? remaining : std::chrono::steady_clock::duration{std::chrono::seconds{1}};
                std::this_thread::sleep_for(chunk);
            }
            if (stop.stop_requested()) return;

            // Refresh the OAuth token before calling on_refresh
            std::string oauth_token;
            if (refresh_access_token_fn_) {
                auto result = refresh_access_token_fn_("");
                // Whether or not refresh succeeds, try with whatever we have.
                // The actual /bridge fetch in on_refresh will fail if the token is bad.
            }

            if (on_refresh_ && !active_session_id_.empty()) {
                on_refresh_(active_session_id_, oauth_token);
            }
        });
    }

    /// Cancel all scheduled refreshes.
    void cancel_all() {
        std::lock_guard lock(mutex_);
        if (timer_thread_.joinable()) {
            timer_thread_.request_stop();
            timer_thread_.join();
        }
        active_session_id_.clear();
        active_expires_in_s_ = 0;
    }

    /// Check if a refresh is currently scheduled.
    [[nodiscard]] bool is_scheduled() const {
        return !active_session_id_.empty();
    }
};

// =========================================================================
// SseConnection — HTTP SSE (EventSource) wrapper
// =========================================================================

/// Wraps an HTTP EventSource connection for Server-Sent Events.
/// The v2 transport uses SSE for the read stream (server -> REPL).
class SseConnection {
    std::string url_;
    std::string token_;
    std::atomic<bool> connected_{false};
    std::jthread reader_thread_;

public:
    /// Callback: SSE event received. Parameters: (event_type, data).
    std::function<void(const std::string&, const std::string&)> on_event;

    /// Callback: connection error. Parameter: error message.
    std::function<void(const std::string&)> on_error;

    SseConnection() = default;
    ~SseConnection() { disconnect(); }

    /// Establish the SSE connection.
    /// In a full implementation this would open an HTTP long-poll or
    /// chunked transfer stream. For the module skeleton it sets state.
    std::expected<void, std::string> connect(const std::string& url, const std::string& token) {
        url_ = url;
        token_ = token;
        connected_.store(true);

        log_bridge_event("sse_connect", {
            {"url", url_.substr(0, 80)},
        });

        // In production: open HTTP GET with text/event-stream Accept header,
        // read chunked lines, dispatch on_event callbacks.
        reader_thread_ = std::jthread([this](std::stop_token stop) {
            read_loop(stop);
        });
        return {};
    }

    /// Close the SSE connection.
    void disconnect() {
        connected_.store(false);
        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
            reader_thread_.join();
        }
        log_bridge_event("sse_disconnect", {});
    }

    /// Check if connected.
    [[nodiscard]] bool is_connected() const { return connected_.load(); }

private:
    void read_loop(std::stop_token stop) {
        // Skeleton SSE read loop.
        // Full implementation: HTTP GET url_ with Accept: text/event-stream,
        // Authorization: Bearer token_, read lines, parse event:/data: pairs,
        // invoke on_event(event_type, data).
        while (!stop.stop_requested() && connected_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
};

// =========================================================================
// CcrV2Client — WebSocket-based v2 worker connection
// =========================================================================

/// CCR v2 client for WebSocket-based worker connections.
/// Handles /worker/register, heartbeats, and event posting.
class CcrV2Client {
    std::string session_url_;
    std::string ingress_token_;
    std::string session_id_;
    int64_t epoch_{0};
    std::string auth_token_;
    std::atomic<bool> registered_{false};

public:
    struct Params {
        std::string session_url;
        std::string ingress_token;
        std::string session_id;
        int64_t epoch = 0;
        std::function<std::string()> get_auth_token;
        bool outbound_only = false;
        int64_t heartbeat_interval_ms = 20000;
        double heartbeat_jitter_fraction = 0.1;
        int64_t initial_sequence_num = 0;
    };

    explicit CcrV2Client(Params params)
        : session_url_(std::move(params.session_url))
        , ingress_token_(std::move(params.ingress_token))
        , session_id_(std::move(params.session_id))
        , epoch_(params.epoch)
    {
        if (params.get_auth_token) {
            auth_token_ = params.get_auth_token();
        }
    }

    /// POST to /worker/register. Returns the worker_epoch.
    std::expected<int64_t, std::string> register_worker() {
        // In production: HTTP POST ${session_url_}/worker/register
        // For the module skeleton, return the epoch we were initialized with.
        log_bridge_event("ccr_v2_register", {
            {"session_id", session_id_},
            {"epoch", std::to_string(epoch_)},
        });
        registered_.store(true);
        return epoch_;
    }

    /// Start a session (begin heartbeats and accept events).
    void start_session() {
        log_bridge_event("ccr_v2_start_session", {
            {"session_id", session_id_},
        });
    }

    /// Report worker state (idle, running, requires_action).
    void report_state(const std::string& state) {
        log_bridge_event("ccr_v2_report_state", {
            {"session_id", session_id_},
            {"state", state},
        });
    }

    /// Write a batch of events to the worker endpoint.
    std::expected<void, std::string> write_batch(const std::vector<std::string>& events) {
        log_bridge_event("ccr_v2_write_batch", {
            {"session_id", session_id_},
            {"count", std::to_string(events.size())},
        });
        return {};
    }

    /// Write a single event.
    std::expected<void, std::string> write(const std::string& event_json) {
        log_bridge_event("ccr_v2_write", {
            {"session_id", session_id_},
        });
        return {};
    }

    /// Get the last sequence number processed.
    [[nodiscard]] int64_t get_last_sequence_num() const { return 0; }

    /// Close the client.
    void close() {
        registered_.store(false);
        log_bridge_event("ccr_v2_close", {
            {"session_id", session_id_},
        });
    }

    [[nodiscard]] bool is_registered() const { return registered_.load(); }
};

// =========================================================================
// ReplV2Transport — combined SSE read + CCRClient write
// =========================================================================

/// The v2 REPL transport combines an SSE read stream (server -> REPL)
/// with a CCRClient write path (REPL -> server).
class ReplV2Transport {
    SseConnection sse_;
    std::unique_ptr<CcrV2Client> ccr_client_;
    std::string session_id_;
    int64_t epoch_{0};
    std::atomic<bool> connected_{false};

public:
    /// Callback: transport connected.
    std::function<void()> on_connect;

    /// Callback: data received from SSE stream.
    std::function<void(const std::string&)> on_data;

    /// Callback: transport closed. Parameter: close code (0 = unknown).
    std::function<void(int)> on_close;

    explicit ReplV2Transport(
        const std::string& session_url,
        const std::string& ingress_token,
        const std::string& session_id,
        int64_t epoch
    )
        : session_id_(session_id)
        , epoch_(epoch)
    {
        CcrV2Client::Params params;
        params.session_url = session_url;
        params.ingress_token = ingress_token;
        params.session_id = session_id;
        params.epoch = epoch;
        ccr_client_ = std::make_unique<CcrV2Client>(std::move(params));
    }

    /// Connect: register the worker, open the SSE read stream.
    std::expected<void, std::string> connect() {
        auto reg = ccr_client_->register_worker();
        if (!reg) return std::unexpected(reg.error());
        ccr_client_->start_session();

        // Wire SSE callbacks
        sse_.on_event = [this](const std::string& event_type, const std::string& data) {
            if (on_data) on_data(data);
        };
        sse_.on_error = [this](const std::string& error) {
            log_bridge_event("v2_transport_sse_error", {
                {"session_id", session_id_},
                {"error", error.substr(0, 120)},
            });
        };

        connected_.store(true);

        // Fire on_connect after a brief setup
        if (on_connect) on_connect();
        return {};
    }

    /// Close both SSE and CCRClient.
    void close() {
        sse_.disconnect();
        if (ccr_client_) ccr_client_->close();
        connected_.store(false);
    }

    /// Report worker state.
    void report_state(const std::string& state) {
        if (ccr_client_) ccr_client_->report_state(state);
    }

    /// Write a batch of events.
    std::expected<void, std::string> write_batch(const std::vector<std::string>& events) {
        if (ccr_client_) return ccr_client_->write_batch(events);
        return std::unexpected("CCR client not initialized");
    }

    /// Write a single event.
    std::expected<void, std::string> write(const std::string& event_json) {
        if (ccr_client_) return ccr_client_->write(event_json);
        return std::unexpected("CCR client not initialized");
    }

    /// Get the last sequence number.
    [[nodiscard]] int64_t get_last_sequence_num() const {
        return ccr_client_ ? ccr_client_->get_last_sequence_num() : 0;
    }

    [[nodiscard]] bool is_connected() const { return connected_.load(); }
};

// =========================================================================
// ReplBridgeHandle — abstract base for bridge handles
// =========================================================================

/// Abstract handle returned by the env-less bridge init.
class ReplBridgeHandle {
public:
    virtual ~ReplBridgeHandle() = default;
    virtual BridgeState state() const = 0;
    virtual void close() = 0;
    virtual void flush() = 0;
    virtual void set_title(std::string_view title) = 0;
    virtual std::string environment_id() const = 0;
};

// Forward declaration
int64_t archive_session(
    const std::string& session_id,
    const std::string& base_url,
    const std::string& access_token,
    const std::string& org_uuid,
    std::chrono::milliseconds timeout
);

// =========================================================================
// EnvLessReplBridgeHandle — concrete ReplBridgeHandle for the env-less path
// =========================================================================

/// Concrete bridge handle for the env-less (v2) bridge. Owns the transport,
/// token refresh scheduler, dedup sets, flush gate, and cleanup logic.
class EnvLessReplBridgeHandle final : public ReplBridgeHandle {
    std::string session_id_;
    std::string session_ingress_url_;
    std::unique_ptr<ReplV2Transport> transport_;
    std::unique_ptr<TokenRefreshScheduler> refresh_scheduler_;

    BridgeState state_{BridgeState::Ready};
    std::atomic<bool> torn_down_{false};
    std::atomic<bool> auth_recovery_in_flight_{false};
    std::atomic<bool> initial_flush_done_{false};

    // Dedup sets
    BoundedUUIDSet recent_posted_uuids_;
    BoundedUUIDSet recent_inbound_uuids_;
    std::unordered_set<std::string> initial_message_uuids_;

    // Flush gate for queuing writes during history flush / rebuild
    FlushGate flush_gate_;

    // Parameters / callbacks (copied from EnvLessBridgeParams)
    EnvLessBridgeParams params_;
    EnvLessBridgeConfig cfg_;

    // Connect cause for telemetry
    ConnectCause connect_cause_{ConnectCause::initial};

    // User message callback latch
    bool user_message_callback_done_{false};

public:
    EnvLessReplBridgeHandle(
        std::string session_id,
        std::string session_ingress_url,
        std::unique_ptr<ReplV2Transport> transport,
        std::unique_ptr<TokenRefreshScheduler> refresh,
        EnvLessBridgeParams params,
        EnvLessBridgeConfig cfg,
        BoundedUUIDSet recent_posted_uuids,
        BoundedUUIDSet recent_inbound_uuids,
        std::unordered_set<std::string> initial_uuids,
        bool user_message_done
    )
        : session_id_(std::move(session_id))
        , session_ingress_url_(std::move(session_ingress_url))
        , transport_(std::move(transport))
        , refresh_scheduler_(std::move(refresh))
        , recent_posted_uuids_(std::move(recent_posted_uuids))
        , recent_inbound_uuids_(std::move(recent_inbound_uuids))
        , initial_message_uuids_(std::move(initial_uuids))
        , params_(std::move(params))
        , cfg_(std::move(cfg))
        , user_message_callback_done_(user_message_done)
    {
        state_ = BridgeState::Connected;
        wire_transport_callbacks();
    }

    ~EnvLessReplBridgeHandle() override {
        if (!torn_down_.load()) {
            teardown();
        }
    }

    BridgeState state() const override { return state_; }

    void close() override {
        teardown();
    }

    void flush() override {
        // No-op: the env-less bridge does not buffer externally.
    }

    void set_title(std::string_view /*title*/) override {
        // Title is set at session creation time in the env-less path.
    }

    std::string environment_id() const override { return {}; }

    /// Get the bridge session ID.
    [[nodiscard]] const std::string& bridge_session_id() const { return session_id_; }

    /// Get the session ingress URL.
    [[nodiscard]] const std::string& session_ingress_url() const { return session_ingress_url_; }

    // -----------------------------------------------------------------
    // Message writing
    // -----------------------------------------------------------------

    /// Write messages to the transport. Filters eligible messages,
    /// deduplicates against posted/initial UUIDs, and forwards.
    void write_messages(const std::vector<SDKMessage>& messages) {
        if (torn_down_.load()) return;

        std::vector<SDKMessage> filtered;
        for (const auto& m : messages) {
            BridgeEligibleMessage eligible{
                .type = m.type,
                .subtype = std::nullopt,
                .is_virtual = false,
            };
            if (!is_eligible_bridge_message(eligible)) continue;

            // Check dedup
            if (m.uuid) {
                if (initial_message_uuids_.count(*m.uuid)) continue;
                if (recent_posted_uuids_.has(*m.uuid)) continue;
            }
            filtered.push_back(m);
        }

        if (filtered.empty()) return;

        // Fire on_user_message for title derivation
        if (!user_message_callback_done_ && params_.on_user_message) {
            for (const auto& m : filtered) {
                TitleCandidate candidate{.type = m.type};
                if (m.message.content && std::get_if<std::string>(&*m.message.content)) {
                    candidate.content = *std::get_if<std::string>(&*m.message.content);
                }
                auto text = extract_title_text(candidate);
                if (text && params_.on_user_message(*text, session_id_)) {
                    user_message_callback_done_ = true;
                    break;
                }
            }
        }

        // Add to dedup set and forward
        for (const auto& m : filtered) {
            if (m.uuid) recent_posted_uuids_.add(*m.uuid);
        }

        if (params_.to_sdk_messages) {
            auto events = params_.to_sdk_messages(filtered);
            if (!events.empty()) {
                // Check if any user messages are in the batch for state reporting
                bool has_user = false;
                for (const auto& m : filtered) {
                    if (m.type == "user") { has_user = true; break; }
                }
                if (has_user && transport_) transport_->report_state("running");
                if (transport_) transport_->write_batch(events);
            }
        }
    }

    /// Write pre-formatted SDK messages.
    void write_sdk_messages(const std::vector<SDKMessage>& messages) {
        if (torn_down_.load()) return;

        std::vector<SDKMessage> filtered;
        for (const auto& m : messages) {
            if (m.uuid && recent_posted_uuids_.has(*m.uuid)) continue;
            filtered.push_back(m);
        }
        if (filtered.empty()) return;

        for (const auto& m : filtered) {
            if (m.uuid) recent_posted_uuids_.add(*m.uuid);
        }

        // Serialize and send
        if (transport_) {
            std::vector<std::string> events;
            for (const auto& m : filtered) {
                events.push_back(std::format(
                    R"({{"session_id":"{}","type":"{}","uuid":"{}"}})",
                    session_id_, m.type, m.uuid.value_or("")));
            }
            transport_->write_batch(events);
        }
    }

    /// Send a control request.
    void send_control_request(const SDKControlRequest& request) {
        if (auth_recovery_in_flight_.load() || torn_down_.load()) return;

        auto event = std::format(
            R"({{"type":"control_request","request_id":"{}","session_id":"{}","request":{{"subtype":"{}"}}}})",
            request.request_id, session_id_, request.request.subtype);

        if (request.request.subtype == "can_use_tool" && transport_) {
            transport_->report_state("requires_action");
        }
        if (transport_) transport_->write(event);
    }

    /// Send a control response.
    void send_control_response(const SDKControlResponse& response) {
        if (auth_recovery_in_flight_.load() || torn_down_.load()) return;

        auto event = std::format(
            R"({{"type":"control_response","session_id":"{}","response":{{"subtype":"{}","request_id":"{}"}}}})",
            session_id_, response.response.subtype, response.response.request_id);

        if (transport_) {
            transport_->report_state("running");
            transport_->write(event);
        }
    }

    /// Send a control cancel request.
    void send_control_cancel_request(const std::string& request_id) {
        if (auth_recovery_in_flight_.load() || torn_down_.load()) return;

        auto event = std::format(
            R"({{"type":"control_cancel_request","request_id":"{}","session_id":"{}"}})",
            request_id, session_id_);

        if (transport_) {
            transport_->report_state("running");
            transport_->write(event);
        }
    }

    /// Send a result message for session archival.
    void send_result() {
        if (auth_recovery_in_flight_.load() || torn_down_.load()) return;
        if (transport_) {
            transport_->report_state("idle");
            auto result = make_result_message(session_id_);
            transport_->write(serialize_result_message(result));
        }
    }

private:
    /// Wire transport callbacks for data, connect, and close events.
    void wire_transport_callbacks() {
        if (!transport_) return;

        transport_->on_connect = [this]() {
            log_bridge_event("v2_transport_connected", {
                {"session_id", session_id_},
            });

            if (!initial_flush_done_.load() && !params_.initial_messages.empty()) {
                initial_flush_done_.store(true);
                flush_history(params_.initial_messages);
            }

            set_state(BridgeState::Connected);
        };

        transport_->on_data = [this](const std::string& data) {
            handle_ingress_message(
                data,
                recent_posted_uuids_,
                recent_inbound_uuids_,
                params_.on_inbound_message,
                params_.on_permission_response,
                [this](const SDKControlRequest& req) {
                    ServerControlRequestHandlers handlers;
                    handlers.session_id = session_id_;
                    handlers.outbound_only = params_.outbound_only;
                    handlers.on_interrupt = params_.on_interrupt;
                    handlers.on_set_model = params_.on_set_model;
                    handlers.on_set_max_thinking_tokens = params_.on_set_max_thinking_tokens;
                    handlers.on_set_permission_mode = params_.on_set_permission_mode;
                    handlers.write_event = [this](const std::string& event) {
                        if (transport_) transport_->write(event);
                    };
                    handle_server_control_request(req, handlers);
                }
            );
        };

        transport_->on_close = [this](int code) {
            if (torn_down_.load()) return;

            log_bridge_event("v2_transport_closed", {
                {"session_id", session_id_},
                {"code", std::to_string(code)},
            });

            // 401 we can recover from; all other codes are dead-ends.
            if (code == 401 && !auth_recovery_in_flight_.load()) {
                recover_from_auth_failure();
                return;
            }
            set_state(BridgeState::Failed, std::format("Transport closed (code {})", code));
        };
    }

    /// Flush initial history messages to the transport.
    void flush_history(const std::vector<SDKMessage>& messages) {
        std::vector<SDKMessage> eligible;
        for (const auto& m : messages) {
            BridgeEligibleMessage bel{
                .type = m.type,
                .subtype = std::nullopt,
                .is_virtual = false,
            };
            if (is_eligible_bridge_message(bel)) eligible.push_back(m);
        }

        // Cap if configured
        std::vector<SDKMessage> capped;
        if (params_.initial_history_cap > 0 &&
            static_cast<int64_t>(eligible.size()) > params_.initial_history_cap) {
            auto start = eligible.end() -
                         static_cast<std::ptrdiff_t>(params_.initial_history_cap);
            capped.assign(start, eligible.end());
            log_bridge_event("history_capped", {
                {"original", std::to_string(eligible.size())},
                {"capped", std::to_string(capped.size())},
            });
        } else {
            capped = std::move(eligible);
        }

        if (capped.empty()) return;

        // If last eligible message is user, report running state
        if (!capped.empty() && capped.back().type == "user" && transport_) {
            transport_->report_state("running");
        }

        if (params_.to_sdk_messages && transport_) {
            auto events = params_.to_sdk_messages(capped);
            transport_->write_batch(events);
        }
    }

    /// Attempt to recover from a 401 auth failure by refreshing the OAuth
    /// token and rebuilding the transport with fresh credentials.
    void recover_from_auth_failure() {
        if (auth_recovery_in_flight_.exchange(true)) return;

        set_state(BridgeState::Reconnecting, "JWT expired -- refreshing");

        log_bridge_event("auth_401_recovery_start", {
            {"session_id", session_id_},
        });

        // Attempt OAuth refresh
        if (params_.on_auth_401 && params_.get_access_token) {
            auto stale = params_.get_access_token();
            if (stale) {
                params_.on_auth_401(*stale);
            }
        }

        auto new_token = params_.get_access_token
            ? params_.get_access_token() : std::nullopt;

        if (!new_token) {
            set_state(BridgeState::Failed, "JWT refresh failed: no OAuth token");
            auth_recovery_in_flight_.store(false);
            return;
        }

        // Fetch fresh credentials
        auto fresh = with_retry<RemoteCredentials>(
            [this, &token = *new_token]() -> std::optional<RemoteCredentials> {
                return fetch_remote_credentials(
                    session_id_, params_.base_url, token,
                    std::chrono::milliseconds{cfg_.http_timeout_ms});
            },
            "fetchRemoteCredentials (recovery)",
            cfg_
        );

        if (!fresh) {
            set_state(BridgeState::Failed, "JWT refresh failed after 401");
            auth_recovery_in_flight_.store(false);
            return;
        }

        // Rebuild transport with fresh credentials
        rebuild_transport(*fresh);

        log_bridge_event("auth_401_recovery_success", {
            {"session_id", session_id_},
        });

        auth_recovery_in_flight_.store(false);
    }

    /// Rebuild the transport with fresh credentials (shared by proactive
    /// refresh and 401 recovery). Closes old transport, creates new one,
    /// rewires callbacks, and re-schedules token refresh.
    void rebuild_transport(const RemoteCredentials& fresh) {
        if (transport_) transport_->close();

        auto session_url = build_ccr_v2_sdk_url(fresh.api_base_url, session_id_);

        transport_ = std::make_unique<ReplV2Transport>(
            session_url, fresh.worker_jwt, session_id_, fresh.worker_epoch);

        wire_transport_callbacks();

        auto connect_result = transport_->connect();
        if (!connect_result) {
            set_state(BridgeState::Failed, connect_result.error());
            return;
        }

        // Re-schedule token refresh from new expires_in
        if (refresh_scheduler_) {
            refresh_scheduler_->schedule_from_expires_in(session_id_, fresh.expires_in);
        }

        set_state(BridgeState::Connected);
    }

    /// Set state and notify callback.
    void set_state(BridgeState next, std::optional<std::string> detail = std::nullopt) {
        state_ = next;
        if (params_.on_state_change) {
            params_.on_state_change(next, detail);
        }
    }

    /// Graceful teardown: cancel refresh, close transport, archive session.
    void teardown() {
        if (torn_down_.exchange(true)) return;

        // Cancel refresh scheduler
        if (refresh_scheduler_) refresh_scheduler_->cancel_all();

        // Send idle state + result message before archive
        if (transport_) {
            transport_->report_state("idle");
            auto result = make_result_message(session_id_);
            transport_->write(serialize_result_message(result));
        }

        // Archive session
        if (params_.get_access_token) {
            auto token = params_.get_access_token();
            if (token) {
                archive_session(session_id_, params_.base_url, *token,
                                params_.org_uuid,
                                std::chrono::milliseconds{cfg_.teardown_archive_timeout_ms});
            }
        }

        // Close transport
        if (transport_) transport_->close();

        set_state(BridgeState::Closed);
        log_bridge_event("teardown", {
            {"session_id", session_id_},
        });
    }
};

// =========================================================================
// archiveSession — POST /v1/sessions/{id}/archive
// =========================================================================

/// Archive status codes for telemetry.
enum class ArchiveStatus {
    ok,
    skipped_no_token,
    network_error,
    server_4xx,
    server_5xx,
};

/// Archive a session by POSTing to the compat archive endpoint.
/// Returns HTTP status code or error indicator.
int64_t archive_session(
    const std::string& session_id,
    const std::string& base_url,
    const std::string& access_token,
    const std::string& org_uuid,
    std::chrono::milliseconds timeout
) {
    if (session_id.empty() || base_url.empty() || access_token.empty()) {
        return -1; // no_token / error
    }

    // Convert session ID to compat format if needed
    auto compat_id = normalize_session_id(session_id);
    auto url = std::format("{}/v1/sessions/{}/archive", base_url, compat_id);

    log_bridge_event("archive_session", {
        {"session_id", compat_id},
        {"url", url.substr(0, 120)},
    });

    // In production: HTTP POST with OAuth headers, anthropic-beta,
    // x-organization-uuid. For the module skeleton, log and return 200.
    (void)org_uuid;
    (void)timeout;
    return 200;
}

/// Stub overload of create_code_session matching the env-less call convention.
/// In production this would delegate to the session_api module with a proper
/// CreateSessionRequest. For the skeleton, returns a synthetic session ID.
std::optional<std::string> create_code_session(
    std::string_view base_url,
    std::string_view access_token,
    std::string_view title,
    std::chrono::milliseconds timeout,
    const std::vector<std::string>& tags = {}
) {
    (void)timeout;
    (void)tags;
    if (base_url.empty() || access_token.empty()) return std::nullopt;
    // Synthetic session ID for skeleton
    return std::format("ses_{:016x}",
        std::hash<std::string>{}(std::string(title) + std::string(base_url)));
}

// =========================================================================
// initEnvLessBridgeCore — main initialization entry point
// =========================================================================

/// Initialize the env-less remote bridge core.
///
/// 1. Fetches OAuth access token
/// 2. Creates a code session (POST /v1/code/sessions)
/// 3. Fetches bridge credentials (POST /v1/code/sessions/{id}/bridge)
/// 4. Builds v2 transport (SSE + CCRClient)
/// 5. Starts token refresh scheduler
/// 6. Wires ingress message handlers
/// 7. Returns a ReplBridgeHandle unique_ptr
///
/// Returns nullptr on any pre-flight failure.
std::unique_ptr<ReplBridgeHandle> init_env_less_bridge_core(EnvLessBridgeParams params) {
    auto cfg = get_env_less_bridge_config();

    // -- 1. Get OAuth access token --
    if (!params.get_access_token) return nullptr;
    auto access_token = params.get_access_token();
    if (!access_token) {
        log_bridge_event("init_failed", {{"reason", "no_oauth_token"}});
        return nullptr;
    }

    // -- 2. Create code session --
    auto created_session_id = with_retry<std::string>(
        [&]() -> std::optional<std::string> {
            return create_code_session(
                params.base_url, *access_token, params.title,
                std::chrono::milliseconds{cfg.http_timeout_ms},
                params.tags);
        },
        "createCodeSession",
        cfg
    );
    if (!created_session_id) {
        if (params.on_state_change) {
            params.on_state_change(BridgeState::Failed,
                "Session creation failed -- see debug log");
        }
        log_bridge_event("init_failed", {{"reason", "session_create_failed"}});
        return nullptr;
    }
    const std::string session_id = *created_session_id;

    log_bridge_event("session_created", {{"session_id", session_id}});

    // -- 3. Fetch bridge credentials --
    auto credentials = with_retry<RemoteCredentials>(
        [&]() -> std::optional<RemoteCredentials> {
            return fetch_remote_credentials(
                session_id, params.base_url, *access_token,
                std::chrono::milliseconds{cfg.http_timeout_ms});
        },
        "fetchRemoteCredentials",
        cfg
    );
    if (!credentials) {
        if (params.on_state_change) {
            params.on_state_change(BridgeState::Failed,
                "Remote credentials fetch failed -- see debug log");
        }
        // Archive the failed session
        archive_session(session_id, params.base_url, *access_token,
                        params.org_uuid,
                        std::chrono::milliseconds{cfg.teardown_archive_timeout_ms});
        log_bridge_event("init_failed", {{"reason", "creds_fetch_failed"}});
        return nullptr;
    }

    log_bridge_event("credentials_fetched", {
        {"session_id", session_id},
        {"expires_in", std::to_string(credentials->expires_in)},
    });

    // -- 4. Build v2 transport --
    auto session_url = build_ccr_v2_sdk_url(credentials->api_base_url, session_id);

    auto transport = std::make_unique<ReplV2Transport>(
        session_url, credentials->worker_jwt, session_id, credentials->worker_epoch);

    // -- 5. Token refresh scheduler --
    TokenRefreshScheduler::Params refresh_params;
    refresh_params.refresh_buffer_ms = std::chrono::milliseconds{cfg.token_refresh_buffer_ms};
    refresh_params.label = "remote";
    refresh_params.on_refresh = [&params, &cfg, session_id](
        const std::string& /*sid*/, const std::string& /*oauth_token*/
    ) {
        // Proactive refresh: re-fetch credentials and rebuild transport.
        // In a full implementation this would call back into the handle.
        log_bridge_event("proactive_refresh", {{"session_id", session_id}});
    };

    auto refresh = std::make_unique<TokenRefreshScheduler>(std::move(refresh_params));
    refresh->schedule_from_expires_in(session_id, credentials->expires_in);

    // -- 6. Build dedup sets --
    BoundedUUIDSet recent_posted_uuids(cfg.uuid_dedup_buffer_size);
    BoundedUUIDSet recent_inbound_uuids(cfg.uuid_dedup_buffer_size);
    std::unordered_set<std::string> initial_message_uuids;

    for (const auto& m : params.initial_messages) {
        if (m.uuid) {
            initial_message_uuids.insert(*m.uuid);
            recent_posted_uuids.add(*m.uuid);
        }
    }

    // Determine if user message callback is already done
    bool user_message_done = !params.on_user_message;

    // -- 7. Connect transport --
    auto connect_result = transport->connect();
    if (!connect_result) {
        if (params.on_state_change) {
            params.on_state_change(BridgeState::Failed, connect_result.error());
        }
        archive_session(session_id, params.base_url, *access_token,
                        params.org_uuid,
                        std::chrono::milliseconds{cfg.teardown_archive_timeout_ms});
        log_bridge_event("init_failed", {{"reason", "transport_connect_failed"}});
        return nullptr;
    }

    if (params.on_state_change) {
        params.on_state_change(BridgeState::Connected, std::nullopt);
    }

    log_bridge_event("init_success", {
        {"session_id", session_id},
        {"expires_in", std::to_string(credentials->expires_in)},
    });

    // -- 8. Return handle --
    return std::make_unique<EnvLessReplBridgeHandle>(
        session_id,
        credentials->api_base_url,
        std::move(transport),
        std::move(refresh),
        std::move(params),
        std::move(cfg),
        std::move(recent_posted_uuids),
        std::move(recent_inbound_uuids),
        std::move(initial_message_uuids),
        user_message_done
    );
}

} // namespace cc::bridge
