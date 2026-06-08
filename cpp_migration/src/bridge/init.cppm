/// @file init.cppm
/// @brief REPL bridge initialization and core — env-based poll/dispatch loop,
///        session management, reconnection with exponential backoff,
///        transport message handling, and flush gate management.
///
/// Condensed C++ migration of src/bridge/replBridge.ts (~2406 lines).
/// Key abstractions: BridgeCoreParams (DI), BridgeCoreHandle (extended handle),
/// EnvBasedReplBridgeHandle (concrete poll-loop implementation).
module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

export module cc.bridge.init;

import cc.types.types;
import cc.bridge.bridge_messaging;
import cc.bridge.bridge_enabled;
import cc.bridge.config;
import cc.bridge.api;
import cc.bridge.transport;
import cc.bridge.ui;
import cc.bridge.messages;
import cc.bridge.work_secret;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;

// ===========================================================================
// Bridge state & callback types
// ===========================================================================

enum class BridgeState { Ready, Connected, Reconnecting, Failed, Closed };

using InboundMessageCallback  = std::function<void(const SDKMessage&)>;
using PermissionResponseCallback = std::function<void(const SDKControlResponse&)>;
using InterruptCallback       = std::function<void()>;
using SetModelCallback        = std::function<void(const std::string&)>;
using StateChangeCallback     = std::function<void(BridgeState, const std::optional<std::string>&)>;

// ===========================================================================
// BridgeCoreParams — explicit-param input to initBridgeCore
// ===========================================================================

struct BridgeCoreParams {
    std::string dir;
    std::string machine_name;
    std::string branch;
    std::optional<std::string> git_repo_url;
    std::string title;
    std::string base_url;
    std::string session_ingress_url;
    std::string worker_type;
    std::function<std::optional<std::string>()> get_access_token;

    std::function<std::optional<std::string>(
        const std::string& environment_id, const std::string& title,
        const std::optional<std::string>& git_repo_url, const std::string& branch)> create_session;
    std::function<void(const std::string& session_id)> archive_session;
    std::function<std::string()> get_current_title;
    std::function<std::vector<SDKMessage>(const std::vector<SDKMessage>&)> to_sdk_messages;

    InboundMessageCallback    on_inbound_message;
    PermissionResponseCallback on_permission_response;
    InterruptCallback         on_interrupt;
    SetModelCallback          on_set_model;
    StateChangeCallback       on_state_change;
    std::function<PollConfig()> get_poll_config;

    std::vector<SDKMessage> initial_messages;
    int initial_history_cap{200};
    bool perpetual{false};
    int initial_sse_sequence_num{0};

    std::shared_ptr<BridgeApiClient> api_client;
    std::shared_ptr<BridgeLogger> logger;
};

// ===========================================================================
// BridgeCoreHandle — extended handle with outbound writes
// ===========================================================================

class BridgeCoreHandle {
public:
    virtual ~BridgeCoreHandle() = default;

    virtual auto state() const -> BridgeState = 0;
    virtual auto environment_id() const -> const std::string& = 0;
    virtual auto bridge_session_id() const -> const std::string& = 0;
    virtual auto session_ingress_url() const -> const std::string& = 0;
    virtual auto get_sse_sequence_num() const -> int = 0;

    virtual void write_messages(const std::vector<SDKMessage>& messages) = 0;
    virtual void write_sdk_messages(const std::vector<SDKMessage>& messages) = 0;
    virtual void send_control_response(const SDKControlResponse& response) = 0;
    virtual void send_control_cancel_request(const std::string& request_id) = 0;
    virtual void send_result() = 0;

    virtual void close() = 0;
    virtual void flush() = 0;
    virtual void set_title(std::string_view title) = 0;
};

// ===========================================================================
// Constants
// ===========================================================================

inline constexpr int POLL_ERROR_INITIAL_MS = 2'000;
inline constexpr int POLL_ERROR_MAX_MS = 60'000;
inline constexpr int POLL_ERROR_GIVE_UP_MS = 15 * 60 * 1'000;
inline constexpr int MAX_ENV_RECREATIONS = 3;

// ===========================================================================
// EnvBasedReplBridgeHandle
// ===========================================================================

class EnvBasedReplBridgeHandle final : public BridgeCoreHandle {
    BridgeCoreParams params_;
    BridgeConfig bridge_config_;
    std::string environment_id_, environment_secret_, current_session_id_, title_;

    std::atomic<bool> running_{false}, teardown_started_{false};
    std::thread poll_thread_, timer_thread_;
    std::atomic<bool> poll_stop_{false}, timer_stop_{false};

    std::unique_ptr<BridgeTransport> transport_;
    std::atomic<int> last_transport_seq_{0};
    int v2_gen_{0};

    std::string current_work_id_, current_ingress_token_;
    BoundedUUIDSet posted_uuids_{2000}, inbound_uuids_{2000};
    std::unordered_set<std::string> initial_msg_uuids_;
    FlushGate flush_gate_;
    std::vector<SDKMessage> flush_pending_;

    std::atomic<int> env_recreations_{0}, consecutive_errors_{0};
    std::mutex reconnect_mtx_, wake_mtx_;
    std::condition_variable wake_cv_;
    std::atomic<bool> wake_req_{false}, reconnect_in_progress_{false};
    std::chrono::steady_clock::time_point first_err_{}, last_err_{};
    std::atomic<BridgeState> state_{BridgeState::Ready};

    // -- helpers -------------------------------------------------------------

    void set_state(BridgeState s, std::optional<std::string> reason = std::nullopt) {
        state_.store(s, std::memory_order_relaxed);
        if (params_.on_state_change) params_.on_state_change(s, std::move(reason));
    }
    void log_d(const std::string& m) const { if (params_.logger) params_.logger->debug(m); }
    void log_w(const std::string& m) const { if (params_.logger) params_.logger->warn(m); }

    void wake() { wake_req_.store(true); wake_cv_.notify_one(); }
    bool at_capacity() const { return !!transport_; }

    // -- environment & session -----------------------------------------------

    bool register_env() {
        auto r = params_.api_client->register_environment(bridge_config_);
        if (!r) { set_state(BridgeState::Failed, "Environment registration failed"); return false; }
        environment_id_ = r->environment_id;
        environment_secret_ = r->environment_secret;
        log_d(std::format("[bridge:repl] Environment registered: {}", environment_id_));
        return true;
    }

    bool create_session() {
        if (!params_.create_session) return false;
        auto sid = params_.create_session(environment_id_, title_,
                                          bridge_config_.git_repo_url, bridge_config_.branch);
        if (!sid) {
            params_.api_client->deregister_environment(environment_id_);
            set_state(BridgeState::Failed, "Session creation failed");
            return false;
        }
        current_session_id_ = *sid;
        log_d(std::format("[bridge:repl] Session created: {}", current_session_id_));
        return true;
    }

    // -- flush gate ----------------------------------------------------------

    void drop_flush() {
        flush_gate_.open();
        flush_pending_.clear();
    }
    void end_flush() {
        flush_gate_.open();
        if (flush_pending_.empty() || !transport_) return;
        for (auto& m : flush_pending_) if (m.uuid) posted_uuids_.add(*m.uuid);
        log_d(std::format("[bridge:repl] Drained {} pending message(s)", flush_pending_.size()));
        flush_pending_.clear();
    }

    // -- transport wiring ----------------------------------------------------

    void on_work(const std::string& sid, const std::string& token,
                 const std::string& wid, bool use_v2) {
        if (sid != current_session_id_) return;
        current_work_id_ = wid; current_ingress_token_ = token; v2_gen_++;
        if (transport_) { transport_->disconnect(); transport_.reset(); }
        flush_gate_.open();

        std::unique_ptr<BridgeTransport> t;
        if (use_v2) {
            t = std::make_unique<HttpPollingTransport>();
        } else {
            t = std::make_unique<WebSocketTransport>();
        }

        t->on_message([this](BridgeMessage m) { handle_ingress(m); });
        t->on_state_change([this](TransportState, TransportState ns) {
            if (ns == TransportState::connected) handle_connected();
        });
        t->on_error([this](TransportError e) {
            log_d(std::format("[bridge:repl] Transport error: {}", e.message));
            if (!e.retryable) handle_close(1002);
        });

        transport_ = std::move(t);
        auto url = use_v2
            ? std::format("{}/v1/code/sessions/{}", params_.base_url, sid)
            : std::format("{}/v1/sessions/{}/ingress", params_.session_ingress_url, sid);
        if (!transport_->connect(url, token)) { transport_.reset(); wake(); }
    }

    void handle_connected() {
        log_d("[bridge:repl] Transport connected");
        teardown_started_.store(false);
        if (!initial_msg_uuids_.empty()) {
            auto msgs = params_.initial_messages;
            initial_msg_uuids_.clear();
            if (params_.initial_history_cap > 0 &&
                static_cast<int>(msgs.size()) > params_.initial_history_cap)
                msgs.erase(msgs.begin(), msgs.end() - params_.initial_history_cap);
            for (auto& m : msgs) if (m.uuid) posted_uuids_.add(*m.uuid);
            log_d(std::format("[bridge:repl] Flushed {} initial message(s)", msgs.size()));
        }
        end_flush();
        set_state(BridgeState::Connected);
    }

    void handle_ingress(const BridgeMessage& msg) {
        if (!msg.id.empty() && inbound_uuids_.has(msg.id)) return;
        if (!msg.id.empty()) inbound_uuids_.add(msg.id);
        if (params_.on_inbound_message) {
            SDKMessage s; s.type = msg.type; s.uuid = msg.id;
            params_.on_inbound_message(s);
        }
    }

    void handle_close(int code) {
        if (transport_) transport_.reset();
        wake(); drop_flush();
        if (code == 1000) { set_state(BridgeState::Failed, "session ended"); running_ = false; return; }
        set_state(BridgeState::Reconnecting,
                  std::format("Remote Control connection lost (code {})", code));
        attempt_reconnect();
    }

    // -- reconnection --------------------------------------------------------

    bool attempt_reconnect() {
        std::lock_guard lock(reconnect_mtx_);
        if (reconnect_in_progress_.exchange(true)) return false;
        auto n = env_recreations_.fetch_add(1) + 1;
        if (n > MAX_ENV_RECREATIONS) { reconnect_in_progress_ = false; return false; }

        if (transport_) { transport_->disconnect(); transport_.reset(); }
        wake(); drop_flush();
        if (!current_work_id_.empty()) {
            params_.api_client->stop_work(environment_id_, current_work_id_, false);
            current_work_id_.clear(); current_ingress_token_.clear();
        }

        // Strategy 1: reconnect-in-place
        bridge_config_.reuse_environment_id = environment_id_;
        auto reg = params_.api_client->register_environment(bridge_config_);
        bridge_config_.reuse_environment_id.reset();
        if (reg) {
            environment_id_ = reg->environment_id;
            environment_secret_ = reg->environment_secret;
            if (params_.api_client->reconnect_session(environment_id_, current_session_id_)) {
                env_recreations_.store(0);
                reconnect_in_progress_ = false;
                return true;
            }
        }

        // Strategy 2: fresh session
        if (params_.archive_session) params_.archive_session(current_session_id_);
        auto t = params_.get_current_title ? params_.get_current_title() : title_;
        auto sid = params_.create_session(environment_id_, t,
                                          bridge_config_.git_repo_url, bridge_config_.branch);
        if (!sid) { reconnect_in_progress_ = false; return false; }
        current_session_id_ = *sid;
        last_transport_seq_.store(0);
        inbound_uuids_.clear();
        env_recreations_.store(0);
        reconnect_in_progress_ = false;
        return true;
    }

    // -- poll loop -----------------------------------------------------------

    void poll_loop() {
        while (!poll_stop_.load(std::memory_order_acquire) && running_.load()) {
            auto pc = params_.get_poll_config ? params_.get_poll_config() : PollConfig{};
            try {
                auto work = params_.api_client->poll_for_work(environment_id_, environment_secret_);
                if (!work) { poll_error(work.error()); continue; }
                if (consecutive_errors_.load() > 0) {
                    consecutive_errors_.store(0); first_err_ = {};
                }
                if (!*work) { at_capacity() ? cap_sleep(pc)
                                            : sleep_wake(pc.interval); continue; }
                dispatch_work(**work);
            } catch (const BridgeFatalError& e) {
                if (poll_stop_.load(std::memory_order_acquire)) break;
                if (e.status_code == 404) {
                    set_state(BridgeState::Reconnecting, "environment lost");
                    if (!attempt_reconnect()) { set_state(BridgeState::Failed, e.what()); break; }
                    set_state(BridgeState::Ready); continue;
                }
                set_state(BridgeState::Failed, e.what()); break;
            } catch (const std::exception& e) {
                if (poll_stop_.load(std::memory_order_acquire)) break;
                poll_error(Error::make(ErrorCode::ConnectionFailed, e.what()));
            }
        }
    }

    void dispatch_work(const WorkResponse& w) {
        auto secret = decode_work_secret(w.secret);
        if (!secret) { params_.api_client->stop_work(environment_id_, w.id, false); return; }
        params_.api_client->acknowledge_work(environment_id_, w.id, secret->session_ingress_token);
        auto dt = w.data_type.value_or("");
        if (dt == "healthcheck") { log_d("[bridge:repl] Healthcheck"); return; }
        if (dt == "session") {
            auto sid = w.data_id.value_or("");
            if (!is_safe_bridge_id(sid)) return;
            on_work(sid, secret->session_ingress_token, w.id,
                    secret->use_code_sessions.value_or(false));
        }
    }

    void poll_error(const Error& err) {
        auto now = std::chrono::steady_clock::now();
        if (last_err_ != std::chrono::steady_clock::time_point{}) {
            auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_err_).count();
            if (gap > POLL_ERROR_MAX_MS * 2) { consecutive_errors_.store(0); first_err_ = {}; }
        }
        last_err_ = now;
        int n = consecutive_errors_.fetch_add(1) + 1;
        if (first_err_ == std::chrono::steady_clock::time_point{}) first_err_ = now;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_err_).count();
        if (n == 1) set_state(BridgeState::Reconnecting, err.message);
        if (elapsed >= POLL_ERROR_GIVE_UP_MS) {
            set_state(BridgeState::Failed, "connection to server lost");
            running_.store(false); return;
        }
        int backoff = std::min(POLL_ERROR_INITIAL_MS * (1 << (n - 1)), POLL_ERROR_MAX_MS);
        if (!current_work_id_.empty())
            params_.api_client->heartbeat_work(environment_id_, current_work_id_, current_ingress_token_);
        sleep_wake(std::chrono::milliseconds(backoff));
    }

    void cap_sleep(const PollConfig& pc) { sleep_wake(pc.interval); }
    void sleep_wake(std::chrono::milliseconds dur) {
        auto deadline = std::chrono::steady_clock::now() + dur;
        while (!poll_stop_.load(std::memory_order_acquire) && running_.load()) {
            std::unique_lock lock(wake_mtx_);
            auto rem = deadline - std::chrono::steady_clock::now();
            if (rem <= std::chrono::milliseconds::zero()) break;
            wake_cv_.wait_for(lock, rem, [this] { return wake_req_.exchange(false); });
            if (wake_req_.exchange(false)) break;
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    void keepalive_loop() {
        while (!timer_stop_.load(std::memory_order_acquire) && running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(120));
            if (timer_stop_.load(std::memory_order_acquire) || !running_.load()) break;
            if (transport_) {
                BridgeMessage k; k.id = "ka"; k.type = "keep_alive"; k.method = "keep_alive";
                transport_->send(std::move(k));
            }
        }
    }

    // -- teardown ------------------------------------------------------------

    void do_teardown() {
        if (teardown_started_.exchange(true)) return;
        running_.store(false);
        if (poll_thread_.joinable()) { poll_stop_.store(true); wake(); }
        if (timer_thread_.joinable()) timer_stop_.store(true);
        if (params_.perpetual) { transport_.reset(); drop_flush(); return; }
        auto t = std::move(transport_); transport_.reset(); drop_flush();
        if (t) {
            BridgeMessage r; r.id = "result"; r.type = "result"; r.method = "result";
            r.payload = std::format(R"({{"session_id":"{}"}})", current_session_id_);
            t->send(std::move(r));
        }
        if (!current_work_id_.empty())
            params_.api_client->stop_work(environment_id_, current_work_id_, true);
        if (params_.archive_session) params_.archive_session(current_session_id_);
        t.reset();
        params_.api_client->deregister_environment(environment_id_);
    }

public:
    EnvBasedReplBridgeHandle(BridgeCoreParams params, BridgeConfig cfg)
        : params_(std::move(params)), bridge_config_(std::move(cfg)), title_(params_.title)
    {
        for (auto& m : params_.initial_messages)
            if (m.uuid) { initial_msg_uuids_.insert(*m.uuid); posted_uuids_.add(*m.uuid); }
        if (!register_env() || !create_session()) { state_.store(BridgeState::Failed); return; }
        running_.store(true);
        poll_thread_ = std::thread([this]() { poll_loop(); });
        timer_thread_ = std::thread([this]() { keepalive_loop(); });
        set_state(BridgeState::Ready);
    }
    ~EnvBasedReplBridgeHandle() override {
        do_teardown();
        if (poll_thread_.joinable()) poll_thread_.join();
        if (timer_thread_.joinable()) timer_thread_.join();
    }

    auto state() const -> BridgeState override { return state_.load(); }
    auto environment_id() const -> const std::string& override { return environment_id_; }
    auto bridge_session_id() const -> const std::string& override { return current_session_id_; }
    auto session_ingress_url() const -> const std::string& override { return params_.session_ingress_url; }
    auto get_sse_sequence_num() const -> int override { return last_transport_seq_.load(); }

    void write_messages(const std::vector<SDKMessage>& msgs) override {
        std::vector<SDKMessage> f;
        for (auto& m : msgs) {
            if (m.uuid && (initial_msg_uuids_.count(*m.uuid) || posted_uuids_.has(*m.uuid))) continue;
            f.push_back(m);
        }
        if (f.empty()) return;
        if (!flush_gate_.is_open()) {
            for (auto& m : f) flush_pending_.push_back(m); return;
        }
        if (!transport_) { log_w(std::format("Dropping {} msg(s) — no transport", f.size())); return; }
        for (auto& m : f) { if (m.uuid) posted_uuids_.add(*m.uuid);
            BridgeMessage b; b.id = m.uuid.value_or(""); b.type = m.type; b.method = "event";
            transport_->send(std::move(b));
        }
    }
    void write_sdk_messages(const std::vector<SDKMessage>& msgs) override {
        std::vector<SDKMessage> f;
        for (auto& m : msgs) { if (m.uuid && posted_uuids_.has(*m.uuid)) continue; f.push_back(m); }
        if (f.empty() || !transport_) return;
        for (auto& m : f) { if (m.uuid) posted_uuids_.add(*m.uuid);
            BridgeMessage b; b.id = m.uuid.value_or(""); b.type = m.type; b.method = "event";
            transport_->send(std::move(b));
        }
    }
    void send_control_response(const SDKControlResponse& r) override {
        if (!transport_) return;
        BridgeMessage b; b.id = r.response.request_id; b.type = "control_response";
        b.method = "control_response"; transport_->send(std::move(b));
    }
    void send_control_cancel_request(const std::string& rid) override {
        if (!transport_) return;
        BridgeMessage b; b.id = rid; b.type = "control_cancel_request";
        b.method = "control_cancel_request"; transport_->send(std::move(b));
    }
    void send_result() override {
        if (!transport_) return;
        BridgeMessage b; b.id = "result"; b.type = "result"; b.method = "result";
        b.payload = std::format(R"({{"session_id":"{}"}})", current_session_id_);
        transport_->send(std::move(b));
    }
    void close() override { do_teardown(); }
    void flush() override { if (state_.load() == BridgeState::Ready && transport_) set_state(BridgeState::Connected); }
    void set_title(std::string_view t) override { title_ = t; }
};

// ===========================================================================
// initBridgeCore
// ===========================================================================

auto initBridgeCore(const BridgeCoreParams& params)
    -> Result<std::unique_ptr<BridgeCoreHandle>>
{
    if (params.base_url.empty())
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Bridge base URL cannot be empty"));
    if (!params.api_client)
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Bridge API client is required"));
    if (!params.create_session)
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Session creation callback is required"));
    if (params.title.empty())
        return std::unexpected(Error::make(ErrorCode::InvalidInput, "Bridge title cannot be empty"));

    BridgeConfig cfg;
    cfg.dir = params.dir; cfg.machine_name = params.machine_name;
    cfg.branch = params.branch; cfg.git_repo_url = params.git_repo_url;
    cfg.max_sessions = 1; cfg.spawn_mode = SpawnMode::single_session;
    cfg.worker_type = params.worker_type; cfg.api_base_url = params.base_url;
    cfg.session_ingress_url = params.session_ingress_url;
    cfg.bridge_id = std::format("bridge_{}", std::hash<std::string>{}(
        std::format("{}{}{}", params.base_url, params.title, std::chrono::steady_clock::now().time_since_epoch().count())));
    cfg.environment_id = std::format("env_{}", std::hash<std::string>{}(
        std::format("{}{}{}", params.base_url, params.dir, std::chrono::steady_clock::now().time_since_epoch().count())));

    auto h = std::make_unique<EnvBasedReplBridgeHandle>(params, std::move(cfg));
    if (h->state() == BridgeState::Failed)
        return std::unexpected(Error::make(ErrorCode::ConnectionFailed, "Bridge initialization failed"));
    return h;
}

// ===========================================================================
// initReplBridge
// ===========================================================================

auto initReplBridge(const BridgeCoreParams& params)
    -> Result<std::unique_ptr<BridgeCoreHandle>>
{
    return initBridgeCore(params);
}

} // namespace cc::bridge
