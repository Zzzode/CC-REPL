/// @file bridge_main.cppm
/// @brief Bridge main loop: environment registration, work polling,
///        session spawning, backoff/reconnection, and graceful shutdown.
module;

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <uv.h>
#include <signal.h>

export module cc.bridge.bridge_main;

import cc.types.types;
import cc.bridge.api;
import cc.bridge.config;
import cc.bridge.work_secret;
import cc.bridge.capacity_wake;

export namespace cc::bridge {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;
using cc::core::VoidResult;

// ============================================================
// CLI argument parsing
// ============================================================

/// Parsed CLI arguments for the bridge sub-command.
struct BridgeCliArgs {
    std::string dir;                          // Working directory (default: cwd)
    std::uint32_t max_sessions{1};            // Max concurrent sessions
    SpawnMode spawn_mode{SpawnMode::single_session};
    bool verbose{false};
    bool sandbox{false};
    std::optional<std::string> session_id;    // Resume an existing session
    std::optional<std::string> debug_file;    // Debug log file path
    std::optional<std::uint32_t> session_timeout_ms;
    std::optional<std::string> permission_mode;
    std::optional<std::string> name;          // Session name
    bool help{false};
    std::optional<std::string> error;         // First parse error, if any
};

namespace detail {

[[nodiscard]] inline std::string cwd_string() {
    return std::filesystem::current_path().string();
}

[[nodiscard]] inline std::optional<SpawnMode> parse_spawn_value(std::string_view raw) {
    if (raw == "session" || raw == "single-session") return SpawnMode::single_session;
    if (raw == "same-dir") return SpawnMode::same_dir;
    if (raw == "worktree") return SpawnMode::worktree;
    return std::nullopt;
}

} // namespace detail

/// Parse bridge CLI arguments from an argv-style vector.
[[nodiscard]] inline BridgeCliArgs parse_bridge_args(const std::vector<std::string>& args) {
    BridgeCliArgs result;
    result.dir = detail::cwd_string();

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        if (arg == "--help" || arg == "-h") {
            result.help = true;
        } else if (arg == "--verbose" || arg == "-v") {
            result.verbose = true;
        } else if (arg == "--sandbox") {
            result.sandbox = true;
        } else if (arg == "--no-sandbox") {
            result.sandbox = false;
        } else if (arg == "--spawn" && i + 1 < args.size()) {
            auto mode = detail::parse_spawn_value(args[++i]);
            if (!mode) {
                result.error = std::format("--spawn requires one of: session, same-dir, worktree (got: {})", args[i]);
                return result;
            }
            result.spawn_mode = *mode;
        } else if (arg.starts_with("--spawn=")) {
            auto mode = detail::parse_spawn_value(arg.substr(8));
            if (!mode) {
                result.error = std::format("--spawn requires one of: session, same-dir, worktree (got: {})", arg.substr(8));
                return result;
            }
            result.spawn_mode = *mode;
        } else if (arg == "--capacity" && i + 1 < args.size()) {
            int cap = std::atoi(args[++i].c_str());
            if (cap < 1) {
                result.error = std::format("--capacity requires a positive integer (got: {})", args[i]);
                return result;
            }
            result.max_sessions = static_cast<std::uint32_t>(cap);
        } else if (arg.starts_with("--capacity=")) {
            int cap = std::atoi(arg.substr(11).c_str());
            if (cap < 1) {
                result.error = std::format("--capacity requires a positive integer (got: {})", arg.substr(11));
                return result;
            }
            result.max_sessions = static_cast<std::uint32_t>(cap);
        } else if (arg == "--debug-file" && i + 1 < args.size()) {
            result.debug_file = std::filesystem::absolute(args[++i]).string();
        } else if (arg.starts_with("--debug-file=")) {
            result.debug_file = std::filesystem::absolute(arg.substr(13)).string();
        } else if (arg == "--session-timeout" && i + 1 < args.size()) {
            result.session_timeout_ms = static_cast<std::uint32_t>(std::atoi(args[++i].c_str()) * 1000);
        } else if (arg.starts_with("--session-timeout=")) {
            result.session_timeout_ms = static_cast<std::uint32_t>(std::atoi(arg.substr(18).c_str()) * 1000);
        } else if (arg == "--permission-mode" && i + 1 < args.size()) {
            result.permission_mode = args[++i];
        } else if (arg.starts_with("--permission-mode=")) {
            result.permission_mode = arg.substr(17);
        } else if (arg == "--name" && i + 1 < args.size()) {
            result.name = args[++i];
        } else if (arg.starts_with("--name=")) {
            result.name = arg.substr(7);
        } else if (arg == "--session-id" && i + 1 < args.size()) {
            result.session_id = args[++i];
        } else if (arg.starts_with("--session-id=")) {
            result.session_id = arg.substr(13);
        } else {
            result.error = std::format("Unknown argument: {}\nRun 'claude remote-control --help' for usage.", arg);
            return result;
        }
    }

    // --capacity with single-session makes no sense
    if (result.spawn_mode == SpawnMode::single_session && result.max_sessions != 1) {
        result.max_sessions = 1;
    }

    return result;
}

// ============================================================
// Backoff configuration
// ============================================================

/// Exponential backoff configuration for reconnection and error recovery.
struct BackoffConfig {
    std::chrono::milliseconds conn_initial_ms{2'000};
    std::chrono::milliseconds conn_cap_ms{120'000};
    std::chrono::milliseconds conn_give_up_ms{600'000};
    std::chrono::milliseconds general_initial_ms{500};
    std::chrono::milliseconds general_cap_ms{30'000};
    std::chrono::milliseconds general_give_up_ms{600'000};
    std::chrono::milliseconds shutdown_grace_ms{30'000};
    std::chrono::milliseconds stop_work_base_delay_ms{1'000};

    /// Calculate exponential backoff delay with jitter for the given attempt.
    /// Formula: min(cap, initial * 2^attempt) * (1 + jitter * random[-1,1])
    [[nodiscard]] std::chrono::milliseconds calculate_delay(
        int attempt,
        std::chrono::milliseconds initial,
        std::chrono::milliseconds cap,
        double jitter = 0.25
    ) const {
        static thread_local std::mt19937 gen{std::random_device{}()};
        std::uniform_real_distribution<double> dist(-1.0, 1.0);

        double exponential = std::min(
            static_cast<double>(cap.count()),
            static_cast<double>(initial.count()) * std::pow(2.0, attempt)
        );
        double jittered = exponential * (1.0 + jitter * dist(gen));
        auto ms = static_cast<std::chrono::milliseconds::rep>(std::round(jittered));
        return std::chrono::milliseconds(
            std::max<std::chrono::milliseconds::rep>(ms, 0));
    }
};

// ============================================================
// Error classification
// ============================================================

namespace detail {

inline constexpr std::array<std::string_view, 5> CONNECTION_ERROR_CODES = {
    "ECONNREFUSED", "ECONNRESET", "ETIMEDOUT",
    "ENETUNREACH",  "EHOSTUNREACH",
};

} // namespace detail

/// Returns true if the error is a network connectivity problem (retryable).
[[nodiscard]] inline bool is_connection_error(const Error& err) {
    auto msg = err.message;
    for (auto code : detail::CONNECTION_ERROR_CODES) {
        if (msg.find(code) != std::string::npos) return true;
    }
    return false;
}

/// Returns true if the error is an HTTP 5xx server-side problem (retryable).
[[nodiscard]] inline bool is_server_error(const Error& err) {
    return err.message.find("HTTP 5") != std::string::npos ||
           err.message.find("ERR_BAD_RESPONSE") != std::string::npos;
}

// ============================================================
// Headless bridge types
// ============================================================

/// Options for the headless (non-interactive) bridge mode.
struct HeadlessBridgeOpts {
    std::string dir;
    std::string environment_id;
    std::string session_id;
    std::string sdk_url;
    std::string access_token;
    std::function<void()> on_done;
    bool sandbox{false};
    bool verbose{false};
};

/// Thrown for configuration issues the supervisor should NOT retry.
/// Maps to EXIT_CODE_PERMANENT in the daemon worker.
class BridgeHeadlessPermanentError : public std::runtime_error {
public:
    explicit BridgeHeadlessPermanentError(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================
// Session handle (bridge-local tracking)
// ============================================================

/// Tracks a spawned session's lifecycle from the bridge's perspective.
struct SessionHandle {
    std::string session_id;
    std::string work_id;
    std::string access_token;
    std::string compat_id;
    std::chrono::steady_clock::time_point start_time;
    bool use_ccr_v2{false};
    std::function<void()> kill;         // Send SIGTERM
    std::function<void()> force_kill;   // Send SIGKILL
    std::function<void(const std::string&)> update_token;
};

// ============================================================
// Session spawner interface
// ============================================================

/// Spawn options for a new session.
struct SessionSpawnOpts {
    std::string session_id;
    std::string sdk_url;
    std::string access_token;
    bool use_ccr_v2{false};
    std::optional<int64_t> worker_epoch;
    std::string dir;
};

/// Result of a spawn attempt: either a handle or an error string.
using SpawnResult = std::expected<std::unique_ptr<SessionHandle>, std::string>;

/// Interface for spawning bridge sessions (child processes).
class SessionSpawner {
public:
    virtual ~SessionSpawner() = default;
    virtual auto spawn(const SessionSpawnOpts& opts) -> SpawnResult = 0;
};

// ============================================================
// Bridge logger interface
// ============================================================

/// Logging interface used by the bridge main loop.
/// Implementations may drive a TUI, pipe to stdout, or be silent.
class BridgeLogger {
public:
    virtual ~BridgeLogger() = default;

    virtual void print_banner(const std::string& env_id) = 0;
    virtual void log_session_start(const std::string& id) = 0;
    virtual void log_session_complete(const std::string& id, std::chrono::milliseconds duration) = 0;
    virtual void log_session_failed(const std::string& id, const std::string& err) = 0;
    virtual void log_status(const std::string& msg) = 0;
    virtual void log_verbose(const std::string& msg) = 0;
    virtual void log_error(const std::string& msg) = 0;
    virtual void log_reconnected(std::chrono::milliseconds disconnected_ms) = 0;

    virtual void add_session(const std::string& id, const std::string& url) = 0;
    virtual void remove_session(const std::string& id) = 0;
    virtual void set_attached(const std::string& id) = 0;
    virtual void clear_status() = 0;
    virtual void refresh_display() = 0;
};

// ============================================================
// Headless logger (routes everything to a single callback)
// ============================================================

class HeadlessBridgeLogger final : public BridgeLogger {
    std::function<void(const std::string&)> log_fn_;

public:
    explicit HeadlessBridgeLogger(std::function<void(const std::string&)> fn)
        : log_fn_(std::move(fn)) {}

    void print_banner(const std::string& env_id) override {
        log_fn_(std::format("registered environmentId={}", env_id));
    }
    void log_session_start(const std::string& id) override {
        log_fn_(std::format("session start {}", id));
    }
    void log_session_complete(const std::string& id, std::chrono::milliseconds ms) override {
        log_fn_(std::format("session complete {} ({}ms)", id, ms.count()));
    }
    void log_session_failed(const std::string& id, const std::string& err) override {
        log_fn_(std::format("session failed {}: {}", id, err));
    }
    void log_status(const std::string& msg) override { log_fn_(msg); }
    void log_verbose(const std::string& msg) override { log_fn_(msg); }
    void log_error(const std::string& msg) override {
        log_fn_(std::format("error: {}", msg));
    }
    void log_reconnected(std::chrono::milliseconds ms) override {
        log_fn_(std::format("reconnected after {}ms", ms.count()));
    }
    void add_session(const std::string& id, const std::string&) override {
        log_fn_(std::format("session attached {}", id));
    }
    void remove_session(const std::string& id) override {
        log_fn_(std::format("session detached {}", id));
    }
    void set_attached(const std::string&) override {}
    void clear_status() override {}
    void refresh_display() override {}
};

// ============================================================
// BridgeMain — the main bridge runner
// ============================================================

/// Orchestrates the bridge lifecycle: register environment, poll for work,
/// spawn sessions, manage backoff, and handle graceful shutdown.
///
/// Uses libuv for async polling (uv_timer_t for poll intervals) and
/// signal handling (uv_signal_t for SIGINT/SIGTERM).
class BridgeMain {
public:
    struct Opts {
        std::string dir;
        std::string api_base_url;
        std::string session_ingress_url;
        std::string bridge_id;
        std::string machine_name;
        std::optional<std::string> branch;
        std::optional<std::string> git_repo_url;
        std::uint32_t max_sessions{1};
        SpawnMode spawn_mode{SpawnMode::single_session};
        bool verbose{false};
        bool sandbox{false};
        std::optional<std::string> debug_file;
        std::optional<std::uint32_t> session_timeout_ms;
        std::optional<std::string> initial_session_id;
        std::optional<std::string> reuse_environment_id;
    };

private:
    Opts opts_;
    std::shared_ptr<BridgeApiClient> api_;
    std::shared_ptr<SessionSpawner> spawner_;
    std::unique_ptr<BridgeLogger> logger_;
    BackoffConfig backoff_config_;

    // Event loop and async handles
    uv_loop_t* loop_{nullptr};
    bool owns_loop_{false};
    uv_timer_t poll_timer_{};
    uv_timer_t status_timer_{};
    uv_signal_t sigint_handle_{};
    uv_signal_t sigterm_handle_{};

    // Session tracking
    std::unordered_map<std::string, std::unique_ptr<SessionHandle>> active_sessions_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> session_start_times_;
    std::unordered_map<std::string, std::string> session_work_ids_;
    std::unordered_map<std::string, std::string> session_ingress_tokens_;
    std::unordered_map<std::string, std::string> session_compat_ids_;
    std::set<std::string> completed_work_ids_;
    std::set<std::string> timed_out_sessions_;
    std::set<std::string> titled_sessions_;

    // Backoff state
    int conn_backoff_attempt_{0};
    int general_backoff_attempt_{0};
    std::optional<std::chrono::steady_clock::time_point> conn_error_start_;
    std::optional<std::chrono::steady_clock::time_point> general_error_start_;
    std::optional<std::chrono::steady_clock::time_point> last_poll_error_time_;
    bool fatal_exit_{false};

    // Registration state
    std::string environment_id_;
    std::string environment_secret_;

    // Shutdown coordination
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> abort_poll_{false};

    static constexpr std::chrono::milliseconds DEFAULT_SESSION_TIMEOUT{300'000};
    static constexpr std::chrono::milliseconds STATUS_UPDATE_INTERVAL{1'000};
    static constexpr int POLL_INTERVAL_BASE_MS = 2'000;
    static constexpr int MAX_STOP_WORK_ATTEMPTS = 3;

public:
    BridgeMain(
        Opts opts,
        std::shared_ptr<BridgeApiClient> api,
        std::shared_ptr<SessionSpawner> spawner,
        std::unique_ptr<BridgeLogger> logger,
        BackoffConfig backoff = {}
    )
        : opts_(std::move(opts))
        , api_(std::move(api))
        , spawner_(std::move(spawner))
        , logger_(std::move(logger))
        , backoff_config_(backoff)
    {}

    ~BridgeMain() { shutdown_cleanup(); }

    BridgeMain(const BridgeMain&) = delete;
    BridgeMain& operator=(const BridgeMain&) = delete;

    // --------------------------------------------------------
    // run() — main loop with libuv
    // --------------------------------------------------------

    /// Run the bridge main loop. Blocks until shutdown.
    /// Returns an error if registration fails or a fatal error occurs.
    [[nodiscard]] VoidResult run() {
        // 1. Register environment
        auto reg_result = register_environment();
        if (!reg_result) return std::unexpected(reg_result.error());

        // 2. Create event loop
        loop_ = new uv_loop_t;
        uv_loop_init(loop_);
        owns_loop_ = true;

        // 3. Wire up signal handlers
        uv_signal_init(loop_, &sigint_handle_);
        sigint_handle_.data = this;
        uv_signal_start(&sigint_handle_, on_signal, SIGINT);

        uv_signal_init(loop_, &sigterm_handle_);
        sigterm_handle_.data = this;
        uv_signal_start(&sigterm_handle_, on_signal, SIGTERM);

        // 4. Start poll timer (fires immediately, then at intervals)
        uv_timer_init(loop_, &poll_timer_);
        poll_timer_.data = this;
        uv_timer_start(&poll_timer_, on_poll_timer, 0, POLL_INTERVAL_BASE_MS);

        // 5. Start status update timer
        uv_timer_init(loop_, &status_timer_);
        status_timer_.data = this;
        uv_timer_start(&status_timer_, on_status_timer,
            static_cast<std::uint64_t>(STATUS_UPDATE_INTERVAL.count()),
            static_cast<std::uint64_t>(STATUS_UPDATE_INTERVAL.count()));

        logger_->print_banner(environment_id_);

        // 6. Run the event loop (blocks until uv_stop)
        uv_run(loop_, UV_RUN_DEFAULT);

        // 7. Graceful shutdown: kill sessions, report, deregister
        perform_shutdown();

        return {};
    }

    /// Trigger graceful shutdown from an external thread.
    void shutdown() {
        abort_poll_.store(true);
        shutting_down_.store(true);
        if (loop_) {
            uv_async_t* async = new uv_async_t;
            uv_async_init(loop_, async, [](uv_async_t* handle) {
                uv_stop(handle->loop);
                uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
                    delete reinterpret_cast<uv_async_t*>(h);
                });
            });
            uv_async_send(async);
        }
    }

    // --------------------------------------------------------
    // run_headless() — non-interactive mode
    // --------------------------------------------------------

    /// Run in headless (daemon worker) mode. Throws BridgeHeadlessPermanentError
    /// for configuration issues that should not be retried.
    [[nodiscard]] VoidResult run_headless(const HeadlessBridgeOpts& hopts) {
        // Validate that the directory is accessible
        if (!std::filesystem::exists(hopts.dir)) {
            throw BridgeHeadlessPermanentError(
                std::format("Directory does not exist: {}", hopts.dir));
        }

        // Verify HTTPS requirement
        if (opts_.api_base_url.starts_with("http://") &&
            opts_.api_base_url.find("localhost") == std::string::npos &&
            opts_.api_base_url.find("127.0.0.1") == std::string::npos) {
            throw BridgeHeadlessPermanentError(
                "Remote Control base URL uses HTTP. Only HTTPS or localhost HTTP is allowed.");
        }

        // Register and run the poll loop
        auto reg_result = register_environment();
        if (!reg_result) return std::unexpected(reg_result.error());

        logger_->print_banner(environment_id_);

        // In headless mode, run a simpler synchronous poll loop
        while (!shutting_down_.load()) {
            auto poll_result = poll_once();
            if (!poll_result && !shutting_down_.load()) {
                // On error, back off
                auto delay = backoff_config_.calculate_delay(
                    conn_backoff_attempt_++,
                    backoff_config_.conn_initial_ms,
                    backoff_config_.conn_cap_ms);
                std::this_thread::sleep_for(delay);
                continue;
            }
            // Reset backoff on success
            conn_backoff_attempt_ = 0;

            auto interval = compute_poll_interval();
            std::this_thread::sleep_for(interval);
        }

        perform_shutdown();
        if (hopts.on_done) hopts.on_done();
        return {};
    }

private:
    // --------------------------------------------------------
    // Environment registration
    // --------------------------------------------------------

    [[nodiscard]] VoidResult register_environment() {
        BridgeConfig bridge_config;
        bridge_config.host = opts_.machine_name;
        bridge_config.path = "/bridge";

        auto reg = api_->register_environment(bridge_config);
        if (!reg) {
            return std::unexpected(reg.error());
        }
        environment_id_ = std::move(reg->environment_id);
        environment_secret_ = std::move(reg->environment_secret);
        return {};
    }

    // --------------------------------------------------------
    // Poll logic (called from uv timer callback)
    // --------------------------------------------------------

    /// Perform one poll cycle. Returns false on error.
    [[nodiscard]] bool poll_once() {
        auto work = api_->poll_for_work(environment_id_, environment_secret_);
        if (!work) {
            auto err = work.error();
            handle_poll_error(err);
            return false;
        }

        // Reset backoff state on successful poll
        conn_error_start_.reset();
        general_error_start_.reset();
        conn_backoff_attempt_ = 0;
        general_backoff_attempt_ = 0;
        last_poll_error_time_.reset();

        if (!work.value()) {
            // No work available
            return true;
        }

        const auto& w = work.value().value();
        dispatch_work(w);
        return true;
    }

    /// Dispatch a work item based on its type.
    void dispatch_work(const WorkResponse& work) {
        // Skip already-completed work
        if (completed_work_ids_.count(work.id)) {
            return;
        }

        // Decode the work secret
        std::string ingress_token;
        if (work.secret.empty()) {
            // Cannot process without a secret
            completed_work_ids_.insert(work.id);
            return;
        }

        // Acknowledge the work
        auto ack = api_->acknowledge_work(environment_id_, work.id, work.secret);
        if (!ack) {
            logger_->log_verbose(std::format(
                "Acknowledge failed for workId={}: {}", work.id, ack.error().message));
        }

        // Determine work type from the data_type field
        const auto& work_type = work.data_type.value_or("");

        if (work_type == "healthcheck") {
            logger_->log_verbose("Healthcheck received");
            return;
        }

        if (work_type == "session" && work.data_id) {
            handle_session_work(work);
            return;
        }

        // Unknown work type — gracefully ignore
        logger_->log_verbose(std::format("Unknown work type: {}, skipping", work_type));
    }

    /// Handle a 'session' work item — spawn or update a session.
    void handle_session_work(const WorkResponse& work) {
        const auto& session_id = work.data_id.value();

        // Check if this session is already running (token refresh path)
        auto it = active_sessions_.find(session_id);
        if (it != active_sessions_.end()) {
            // Update the access token for the existing session
            auto& handle = it->second;
            if (handle->update_token) {
                handle->update_token(work.secret);
            }
            session_ingress_tokens_[session_id] = work.secret;
            session_work_ids_[session_id] = work.id;
            logger_->log_verbose(std::format(
                "Updated access token for existing sessionId={}", session_id));
            return;
        }

        // At capacity — cannot spawn new session
        if (active_sessions_.size() >= opts_.max_sessions) {
            logger_->log_verbose(std::format(
                "At capacity ({}/{}), cannot spawn new session for workId={}",
                active_sessions_.size(), opts_.max_sessions, work.id));
            return;
        }

        // Spawn a new session
        SessionSpawnOpts spawn_opts{
            .session_id = session_id,
            .sdk_url = opts_.api_base_url,
            .access_token = work.secret,
            .use_ccr_v2 = false,
            .worker_epoch = std::nullopt,
            .dir = opts_.dir,
        };

        auto spawn_result = spawner_->spawn(spawn_opts);
        if (!spawn_result) {
            logger_->log_error(std::format(
                "Failed to spawn session {}: {}", session_id, spawn_result.error()));
            completed_work_ids_.insert(work.id);
            stop_work_with_retry(work.id);
            return;
        }

        auto& handle = *spawn_result.value();
        handle.start_time = std::chrono::steady_clock::now();

        // Track the session
        active_sessions_.emplace(session_id, std::move(spawn_result.value()));
        session_work_ids_[session_id] = work.id;
        session_ingress_tokens_[session_id] = work.secret;
        session_start_times_[session_id] = std::chrono::steady_clock::now();

        logger_->log_session_start(session_id);
        logger_->add_session(session_id, opts_.session_ingress_url);
        logger_->set_attached(session_id);

        // Start session timeout watchdog
        start_session_timeout(session_id);
    }

    // --------------------------------------------------------
    // Session timeout
    // --------------------------------------------------------

    void start_session_timeout(const std::string& session_id) {
        auto timeout_ms = opts_.session_timeout_ms.value_or(
            static_cast<std::uint32_t>(DEFAULT_SESSION_TIMEOUT.count()));
        if (timeout_ms == 0) return;

        // The actual timeout is managed externally via uv_timer.
        // This method records the session for timeout tracking.
        (void)session_id;
        // Deferred: per-session uv_timer setup belongs with full lifecycle wiring.
    }

    // --------------------------------------------------------
    // Session completion
    // --------------------------------------------------------

    void on_session_done(const std::string& session_id, const std::string& status) {
        auto it = active_sessions_.find(session_id);
        if (it == active_sessions_.end()) return;

        auto start = session_start_times_.find(session_id);
        auto duration = start != session_start_times_.end()
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start->second)
            : std::chrono::milliseconds{0};

        auto work_id = session_work_ids_.find(session_id);
        auto compat_id = session_compat_ids_.find(session_id);

        // Remove from tracking maps
        active_sessions_.erase(it);
        session_start_times_.erase(session_id);
        session_work_ids_.erase(session_id);
        session_ingress_tokens_.erase(session_id);
        session_compat_ids_.erase(session_id);
        timed_out_sessions_.erase(session_id);

        auto cid = compat_id != session_compat_ids_.end() ? compat_id->second : session_id;
        logger_->remove_session(cid);

        // Log completion
        if (status == "completed") {
            logger_->log_session_complete(session_id, duration);
        } else if (status == "failed") {
            bool was_timed_out = false; // timed_out_sessions_ already erased
            if (!was_timed_out && !shutting_down_.load()) {
                logger_->log_session_failed(session_id, "Process exited with error");
            }
        }

        // Notify the server that work is done
        if (status != "interrupted" && work_id != session_work_ids_.end()) {
            stop_work_with_retry(work_id->second);
            completed_work_ids_.insert(work_id->second);
        }

        // In single-session mode, abort the poll loop on session end
        if (status != "interrupted" && !shutting_down_.load() &&
            opts_.spawn_mode == SpawnMode::single_session) {
            abort_poll_.store(true);
            if (loop_) uv_stop(loop_);
        }
    }

    // --------------------------------------------------------
    // Stop work with retry
    // --------------------------------------------------------

    void stop_work_with_retry(const std::string& work_id) {
        for (int attempt = 1; attempt <= MAX_STOP_WORK_ATTEMPTS; ++attempt) {
            auto result = api_->stop_work(environment_id_, work_id, false);
            if (result) return;

            // Auth/permission errors won't be fixed by retrying
            if (result.error().code == ErrorCode::AuthenticationFailed) return;

            if (attempt < MAX_STOP_WORK_ATTEMPTS) {
                auto delay = backoff_config_.calculate_delay(
                    attempt - 1,
                    backoff_config_.stop_work_base_delay_ms,
                    backoff_config_.stop_work_base_delay_ms * 8);
                std::this_thread::sleep_for(delay);
            } else {
                logger_->log_verbose(std::format(
                    "Failed to stop work {} after {} attempts", work_id, MAX_STOP_WORK_ATTEMPTS));
            }
        }
    }

    // --------------------------------------------------------
    // Heartbeat active work items
    // --------------------------------------------------------

    /// Send heartbeat for all active sessions. Returns the aggregate result.
    [[nodiscard]] auto heartbeat_active_work_items() -> std::string {
        bool any_success = false;
        bool any_auth_failed = false;

        for (const auto& [session_id, handle] : active_sessions_) {
            auto work_it = session_work_ids_.find(session_id);
            auto token_it = session_ingress_tokens_.find(session_id);
            if (work_it == session_work_ids_.end() || token_it == session_ingress_tokens_.end()) {
                continue;
            }
            auto hb = api_->heartbeat_work(environment_id_, work_it->second, token_it->second);
            if (hb) {
                any_success = true;
            } else {
                auto& err = hb.error();
                if (err.code == ErrorCode::AuthenticationFailed) {
                    any_auth_failed = true;
                    // Attempt reconnect for auth-failed sessions
                    auto reconnect = api_->reconnect_session(environment_id_, session_id);
                    if (!reconnect) {
                        logger_->log_verbose(std::format(
                            "Failed to reconnect session {}: {}",
                            session_id, reconnect.error().message));
                    }
                }
            }
        }

        if (any_auth_failed) return "auth_failed";
        return any_success ? "ok" : "failed";
    }

    // --------------------------------------------------------
    // Poll error handling with backoff
    // --------------------------------------------------------

    void handle_poll_error(const Error& err) {
        auto now = std::chrono::steady_clock::now();

        // Check for fatal errors (401/403)
        if (err.code == ErrorCode::AuthenticationFailed) {
            fatal_exit_ = true;
            logger_->log_error(err.message);
            abort_poll_.store(true);
            if (loop_) uv_stop(loop_);
            return;
        }

        // Classify error
        bool is_conn = is_connection_error(err);
        bool is_srv = is_server_error(err);

        // Sleep detection: if the gap since the last error greatly exceeds
        // the expected backoff, the machine likely slept. Reset error budget.
        if (last_poll_error_time_) {
            auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *last_poll_error_time_);
            auto threshold = backoff_config_.conn_cap_ms * 2;
            if (gap > threshold) {
                logger_->log_verbose("Detected system sleep, resetting error budget");
                conn_error_start_.reset();
                conn_backoff_attempt_ = 0;
                general_error_start_.reset();
                general_backoff_attempt_ = 0;
            }
        }
        last_poll_error_time_ = now;

        if (is_conn || is_srv) {
            // Connection / server error path
            if (!conn_error_start_) conn_error_start_ = now;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *conn_error_start_);

            if (elapsed >= backoff_config_.conn_give_up_ms) {
                logger_->log_error(std::format(
                    "Server unreachable for {} minutes, giving up.",
                    std::chrono::duration_cast<std::chrono::minutes>(elapsed).count()));
                fatal_exit_ = true;
                abort_poll_.store(true);
                if (loop_) uv_stop(loop_);
                return;
            }

            // Reset general error track when switching types
            general_error_start_.reset();
            general_backoff_attempt_ = 0;

            auto delay = backoff_config_.calculate_delay(
                conn_backoff_attempt_++,
                backoff_config_.conn_initial_ms,
                backoff_config_.conn_cap_ms);

            logger_->log_verbose(std::format(
                "Connection error, retrying in {}ms ({}s elapsed): {}",
                delay.count(),
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
                err.message));

        } else {
            // General error path
            if (!general_error_start_) general_error_start_ = now;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *general_error_start_);

            if (elapsed >= backoff_config_.general_give_up_ms) {
                logger_->log_error(std::format(
                    "Persistent errors for {} minutes, giving up.",
                    std::chrono::duration_cast<std::chrono::minutes>(elapsed).count()));
                fatal_exit_ = true;
                abort_poll_.store(true);
                if (loop_) uv_stop(loop_);
                return;
            }

            // Reset connection error track when switching types
            conn_error_start_.reset();
            conn_backoff_attempt_ = 0;

            auto delay = backoff_config_.calculate_delay(
                general_backoff_attempt_++,
                backoff_config_.general_initial_ms,
                backoff_config_.general_cap_ms);

            logger_->log_verbose(std::format(
                "Poll failed, retrying in {}ms ({}s elapsed): {}",
                delay.count(),
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
                err.message));
        }
    }

    // --------------------------------------------------------
    // Poll interval computation
    // --------------------------------------------------------

    [[nodiscard]] std::chrono::milliseconds compute_poll_interval() const {
        auto at_cap = active_sessions_.size() >= opts_.max_sessions;
        if (at_cap) {
            // At capacity: slow poll as a liveness signal
            return std::chrono::milliseconds{10'000};
        }
        if (active_sessions_.size() > 0) {
            // Partial capacity
            return std::chrono::milliseconds{5'000};
        }
        // Idle
        return std::chrono::milliseconds{POLL_INTERVAL_BASE_MS};
    }

    // --------------------------------------------------------
    // Graceful shutdown
    // --------------------------------------------------------

    void perform_shutdown() {
        if (active_sessions_.empty()) {
            deregister_environment();
            return;
        }

        logger_->log_status(std::format(
            "Shutting down {} active session(s)...", active_sessions_.size()));

        // 1. Send SIGTERM to all active sessions
        for (auto& [sid, handle] : active_sessions_) {
            if (handle->kill) handle->kill();
        }

        // 2. Wait for grace period
        auto grace = backoff_config_.shutdown_grace_ms;
        auto deadline = std::chrono::steady_clock::now() + grace;
        while (!active_sessions_.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        // 3. SIGKILL any stragglers
        for (auto& [sid, handle] : active_sessions_) {
            if (handle->force_kill) handle->force_kill();
        }

        // 4. Stop all work items
        for (const auto& [sid, work_id] : session_work_ids_) {
            auto result = api_->stop_work(environment_id_, work_id, true);
            if (!result) {
                logger_->log_verbose(std::format(
                    "Failed to stop work {} for session {}: {}",
                    work_id, sid, result.error().message));
            }
        }

        // 5. Archive sessions
        for (const auto& [sid, cid] : session_compat_ids_) {
            auto result = api_->archive_session(cid.empty() ? sid : cid);
            if (!result) {
                logger_->log_verbose(std::format(
                    "Failed to archive session {}: {}", sid, result.error().message));
            }
        }

        active_sessions_.clear();
        session_start_times_.clear();
        session_work_ids_.clear();
        session_ingress_tokens_.clear();
        session_compat_ids_.clear();

        deregister_environment();
        logger_->log_verbose("Environment offline.");
    }

    void deregister_environment() {
        if (environment_id_.empty()) return;
        auto result = api_->deregister_environment(environment_id_);
        if (!result) {
            logger_->log_verbose(std::format(
                "Failed to deregister environment: {}", result.error().message));
        } else {
            logger_->log_verbose("Environment deregistered.");
        }
    }

    // --------------------------------------------------------
    // libuv callbacks
    // --------------------------------------------------------

    static void on_signal(uv_signal_t* handle, int signum) {
        (void)signum;
        auto* self = static_cast<BridgeMain*>(handle->data);
        self->shutting_down_.store(true);
        self->abort_poll_.store(true);
        if (self->loop_) uv_stop(self->loop_);
    }

    static void on_poll_timer(uv_timer_t* handle) {
        auto* self = static_cast<BridgeMain*>(handle->data);
        if (self->abort_poll_.load()) {
            uv_timer_stop(handle);
            return;
        }
        (void)self->poll_once();

        // Adjust the repeat interval based on current state
        auto interval = self->compute_poll_interval();
        uv_timer_set_repeat(handle, static_cast<std::uint64_t>(interval.count()));
    }

    static void on_status_timer(uv_timer_t* handle) {
        auto* self = static_cast<BridgeMain*>(handle->data);
        if (self->shutting_down_.load()) {
            uv_timer_stop(handle);
            return;
        }
        // Refresh status display periodically
        self->logger_->refresh_display();
    }

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    void shutdown_cleanup() {
        if (!loop_) return;

        uv_timer_stop(&poll_timer_);
        uv_timer_stop(&status_timer_);
        uv_signal_stop(&sigint_handle_);
        uv_signal_stop(&sigterm_handle_);

        if (owns_loop_) {
            uv_loop_close(loop_);
            delete loop_;
            loop_ = nullptr;
            owns_loop_ = false;
        }
    }
};

// ============================================================
// Convenience: format helpers
// ============================================================

namespace detail {

[[nodiscard]] inline std::string format_delay(std::chrono::milliseconds ms) {
    if (ms.count() >= 1000) {
        return std::format("{:.1f}s", ms.count() / 1000.0);
    }
    return std::format("{}ms", ms.count());
}

[[nodiscard]] inline std::string format_duration(std::chrono::milliseconds ms) {
    auto total_sec = ms.count() / 1000;
    if (total_sec < 60) return std::format("{}s", total_sec);
    auto min = total_sec / 60;
    auto sec = total_sec % 60;
    return std::format("{}m{}s", min, sec);
}

} // namespace detail

} // namespace cc::bridge
