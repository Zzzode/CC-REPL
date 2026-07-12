// ============================================================================
/// @file manager.cppm
/// @brief LSP Manager - Singleton facade managing LSP server lifecycle.
///        Tracks initialization state, server capabilities, and dispatches
///        events for state changes.
///
/// TS REF: src/services/lsp/manager.ts (289 lines)
///   - Initialization state machine (not-started/pending/success/failed)
///   - Singleton LSPServerManager instance management
///   - Async initialization with generation counter for stale-prevention
///   - Re-initialization support (for plugin refresh, issue #15521)
///   - Graceful shutdown with state cleanup
///   - isLspConnected() health check across all servers
///
/// Added beyond TS:
///   - Per-server capability tracking (from initialize response)
///   - Event dispatch system (state changes, server lifecycle)
// ============================================================================
module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.services.lsp.manager;

import cc.services.lsp.LSPServerManager;
import cc.services.lsp.LSPServerInstance;
import cc.services.lsp.types;
import cc.utils.error;
import cc.utils.json;

export namespace cc::services::lsp {

using cc::utils::Result;
namespace fs = std::filesystem;

// ============================================================================
// Initialization State
// ============================================================================

/// Initialization state of the LSP server manager.
/// TS REF: src/services/lsp/manager.ts:14-15 (InitializationState type)
enum class InitializationState : uint8_t {
    NotStarted,  ///< Not yet initialized
    Pending,     ///< Initialization in progress
    Success,     ///< Initialized successfully
    Failed,      ///< Initialization failed
};

/// Convert InitializationState to canonical string name.
[[nodiscard]] inline std::string_view initialization_state_to_string(
    InitializationState state
) {
    switch (state) {
        case InitializationState::NotStarted: return "not-started";
        case InitializationState::Pending:    return "pending";
        case InitializationState::Success:    return "success";
        case InitializationState::Failed:     return "failed";
    }
    return "not-started";
}

// ============================================================================
// Initialization Status
// ============================================================================

/// Status report from get_initialization_status().
/// TS REF: src/services/lsp/manager.ts:76-94 (getInitializationStatus return type)
struct InitializationStatus {
    InitializationState state{InitializationState::NotStarted};
    std::optional<std::string> error;  ///< Present only when state == Failed
};

// ============================================================================
// Server Capabilities (high-level tracking)
// ============================================================================

/// Tracked capabilities for a single LSP server.
/// Derived from the initialize response's ServerCapabilities.
/// TS REF: src/services/lsp/LSPServerInstance.ts (capabilities field)
struct ServerCapabilities {
    bool hover{false};
    bool definition{false};
    bool references{false};
    bool completion{false};
    bool document_symbol{false};
    bool code_action{false};
    bool formatting{false};
    bool rename{false};
    bool diagnostics{false};
    bool implementation{false};
    bool call_hierarchy{false};
    bool type_hierarchy{false};
    bool semantic_tokens{false};
    bool code_lens{false};
    bool document_link{false};
    bool color_provider{false};
    bool folding_range{false};
    bool selection_range{false};
    bool declaration{false};
    bool type_definition{false};
    bool signature_help{false};
    bool document_highlight{false};
    bool inlay_hint{false};
    bool inline_value{false};
    bool inline_completion{false};
    bool execute_command{false};
    bool workspace_symbol{false};

    /// Server info string (name + version) from initialize response.
    std::string server_info;
};

// ============================================================================
// LSP Event Types
// ============================================================================

/// Types of events dispatched by the LSP manager.
enum class LspEventType : uint8_t {
    InitializationStarted,    ///< Async initialization began
    InitializationSucceeded,  ///< Initialization completed successfully
    InitializationFailed,     ///< Initialization failed
    ServerStarted,            ///< A language server process started
    ServerReady,              ///< A server completed initialize handshake
    ServerStopped,            ///< A server was stopped
    ServerError,              ///< A server encountered an error
    Shutdown,                 ///< Manager shutdown completed
    ReinitializationStarted,  ///< Re-initialization triggered
};

/// An event dispatched by the LSP manager.
struct LspEvent {
    LspEventType type;
    std::string server_name;       ///< Server name (empty for manager-level events)
    std::string message;           ///< Human-readable description
    int64_t timestamp_ms{0};       ///< When the event occurred (epoch ms)
};

/// Callback type for LSP event subscriptions.
using LspEventCallback = std::function<void(const LspEvent&)>;

// ============================================================================
// LspManager - Singleton facade
// ============================================================================

/// Singleton facade managing the LSP server manager lifecycle.
///
/// Wraps LSPServerManager with initialization state tracking, async
/// initialization, capability tracking, and event dispatch.
///
/// TS REF: src/services/lsp/manager.ts (entire module)
///   - lspManagerInstance singleton
///   - initializationState / initializationError / initializationGeneration
///   - initializeLspServerManager / reinitializeLspServerManager
///   - shutdownLspServerManager / waitForInitialization
///   - getLspServerManager / getInitializationStatus / isLspConnected
class LspManager {
public:
    /// Get the singleton instance.
    /// TS REF: src/services/lsp/manager.ts:20-21 (lspManagerInstance)
    static LspManager& instance() {
        static LspManager inst;
        return inst;
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// Initialize the LSP server manager singleton.
    ///
    /// Synchronously creates the manager instance, then starts async
    /// initialization (loading LSP configs) in the background without
    /// blocking startup. Safe to call multiple times - idempotent.
    /// If initialization previously failed, calling again will retry.
    ///
    /// TS REF: src/services/lsp/manager.ts:145-208 (initializeLspServerManager)
    void initialize() {
        // --bare / SIMPLE: no LSP. LSP is for editor integration (diagnostics,
        // hover, go-to-def in the REPL). Scripted -p calls have no use for it.
        // TS REF: src/services/lsp/manager.ts:148-150 (isBareMode gate)
        if (const char* simple = std::getenv("CLAUDE_CODE_SIMPLE");
            simple && simple[0] == '1') {
            return;
        }

        std::lock_guard lock(mutex_);

        // Skip if already initialized or currently initializing
        // TS REF: src/services/lsp/manager.ts:154-159
        if (manager_ != nullptr && state_ != InitializationState::Failed) {
            return;
        }

        // Reset state for retry if previous initialization failed
        // TS REF: src/services/lsp/manager.ts:162-165
        if (state_ == InitializationState::Failed) {
            manager_.reset();
            error_.reset();
        }

        // Create the manager instance and mark as pending
        // TS REF: src/services/lsp/manager.ts:168-170
        manager_ = create_lsp_server_manager();
        state_ = InitializationState::Pending;

        // Increment generation to invalidate any pending initializations
        // TS REF: src/services/lsp/manager.ts:173
        const uint64_t current_generation = ++generation_;

        emit_event(LspEvent{
            .type = LspEventType::InitializationStarted,
            .server_name = "",
            .message = "LSP server manager initialization started",
            .timestamp_ms = current_time_ms(),
        });

        // Create promise for wait_for_initialization()
        auto promise = std::make_shared<std::promise<void>>();
        init_future_ = promise->get_future().share();

        // Start initialization asynchronously without blocking
        // TS REF: src/services/lsp/manager.ts:180-207
        std::thread([this, current_generation, promise]() {
            auto* mgr = manager_.get();
            if (!mgr) {
                promise->set_value();
                return;
            }

            auto result = mgr->initialize();
            if (!result) {
                // Only update state if this is still the current initialization
                // TS REF: src/services/lsp/manager.ts:196-206
                LspEvent fail_event;
                bool should_emit = false;
                {
                    std::lock_guard lock(mutex_);
                    if (current_generation == generation_) {
                        state_ = InitializationState::Failed;
                        error_ = result.error().message();
                        manager_.reset();  // Clear the instance since it's not usable

                        should_emit = true;
                        fail_event = LspEvent{
                            .type = LspEventType::InitializationFailed,
                            .server_name = "",
                            .message = "LSP initialization failed: " + error_.value_or("unknown error"),
                            .timestamp_ms = current_time_ms(),
                        };
                    }
                }
                if (should_emit) {
                    emit_event(fail_event);
                }
                promise->set_value();
                return;
            }

            // Success - only update if still current generation
            // TS REF: src/services/lsp/manager.ts:184-192
            LspEvent success_event;
            bool should_emit = false;
            {
                std::lock_guard lock(mutex_);
                if (current_generation == generation_) {
                    state_ = InitializationState::Success;

                    // Capture capabilities from all registered servers
                    refresh_capabilities_locked();

                    should_emit = true;
                    success_event = LspEvent{
                        .type = LspEventType::InitializationSucceeded,
                        .server_name = "",
                        .message = "LSP server manager initialized successfully",
                        .timestamp_ms = current_time_ms(),
                    };
                }
            }
            if (should_emit) {
                emit_event(success_event);
            }
            promise->set_value();
        }).detach();
    }

    /// Force re-initialization of the LSP server manager.
    ///
    /// Called after plugin caches are cleared, so newly-loaded plugin LSP
    /// servers are picked up. Fixes plugin refresh leaving stale server list.
    ///
    /// TS REF: src/services/lsp/manager.ts:226-253 (reinitializeLspServerManager)
    void reinitialize() {
        // TS REF: src/services/lsp/manager.ts:227-231
        if (state_ == InitializationState::NotStarted) {
            // initialize() was never called. Don't start it now.
            return;
        }

        emit_event_locked(LspEvent{
            .type = LspEventType::ReinitializationStarted,
            .server_name = "",
            .message = "LSP server manager re-initialization triggered",
            .timestamp_ms = current_time_ms(),
        });

        // Best-effort shutdown of any running servers on the old instance so
        // /reload-plugins doesn't leak child processes. Fire-and-forget.
        // TS REF: src/services/lsp/manager.ts:238-244
        if (manager_) {
            (void)manager_->shutdown();
        }

        // Force the idempotence check in initialize() to fall through.
        // Generation counter handles invalidating any in-flight init.
        // TS REF: src/services/lsp/manager.ts:248-250
        {
            std::lock_guard lock(mutex_);
            manager_.reset();
            state_ = InitializationState::NotStarted;
            error_.reset();
            server_capabilities_.clear();
        }

        initialize();
    }

    /// Shutdown the LSP server manager and clean up resources.
    ///
    /// Stops all running LSP servers and clears internal state.
    /// Safe to call when not initialized (no-op).
    ///
    /// TS REF: src/services/lsp/manager.ts:267-289 (shutdownLspServerManager)
    /// @returns Future that resolves when shutdown completes
    std::future<void> shutdown() {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();

        std::unique_lock lock(mutex_);
        if (manager_ == nullptr) {
            promise->set_value();
            return future;
        }

        // Take ownership of the manager for async shutdown
        auto mgr = std::move(manager_);
        state_ = InitializationState::NotStarted;
        error_.reset();
        init_future_ = {};
        ++generation_;  // Invalidate any pending initializations
        server_capabilities_.clear();

        lock.unlock();

        // Async shutdown
        std::thread([this, mgr = std::move(mgr), promise]() {
            if (mgr) {
                (void)mgr->shutdown();
            }

            std::lock_guard lock(mutex_);
            emit_event_locked(LspEvent{
                .type = LspEventType::Shutdown,
                .server_name = "",
                .message = "LSP server manager shut down",
                .timestamp_ms = current_time_ms(),
            });
            promise->set_value();
        }).detach();

        return future;
    }

    /// Wait for LSP server manager initialization to complete.
    ///
    /// Returns immediately if initialization has already completed (success
    /// or failure). If initialization is pending, waits for it to complete.
    /// If initialization hasn't started, returns immediately.
    ///
    /// TS REF: src/services/lsp/manager.ts:121-133 (waitForInitialization)
    std::shared_future<void> wait_for_initialization() {
        std::lock_guard lock(mutex_);

        // If already initialized or failed, return immediately
        // TS REF: src/services/lsp/manager.ts:123-125
        if (state_ == InitializationState::Success ||
            state_ == InitializationState::Failed) {
            std::promise<void> p;
            p.set_value();
            return p.get_future();
        }

        // If pending and we have a future, return a copy
        // TS REF: src/services/lsp/manager.ts:128-130
        if (state_ == InitializationState::Pending && init_future_.valid()) {
            return init_future_;
        }

        // Not started - return immediately
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    // ------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------

    /// Get the singleton LSP server manager instance.
    /// Returns nullptr if not yet initialized, initialization failed,
    /// or still pending.
    ///
    /// TS REF: src/services/lsp/manager.ts:63-69 (getLspServerManager)
    LSPServerManager* get_manager() {
        std::lock_guard lock(mutex_);
        // Don't return a broken instance if initialization failed
        if (state_ == InitializationState::Failed) return nullptr;
        return manager_.get();
    }

    /// Get the current initialization status.
    /// TS REF: src/services/lsp/manager.ts:76-94 (getInitializationStatus)
    InitializationStatus get_status() {
        std::lock_guard lock(mutex_);
        InitializationStatus status;
        status.state = state_;
        if (state_ == InitializationState::Failed) {
            status.error = error_;
        }
        return status;
    }

    /// Check whether at least one language server is connected and healthy.
    /// Backs LSPTool.isEnabled().
    ///
    /// TS REF: src/services/lsp/manager.ts:100-110 (isLspConnected)
    bool is_connected() {
        std::lock_guard lock(mutex_);
        if (state_ == InitializationState::Failed) return false;
        if (!manager_) return false;

        const auto& servers = manager_->get_all_servers();
        if (servers.empty()) return false;

        for (const auto& [name, server] : servers) {
            if (server && server->state != ServerInstanceState::Error &&
                server->state != ServerInstanceState::Stopping) {
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // Capability tracking
    // ------------------------------------------------------------------

    /// Get tracked capabilities for a specific server.
    /// Returns nullopt if server is not known.
    std::optional<ServerCapabilities> get_server_capabilities(
        std::string_view server_name
    ) const {
        std::lock_guard lock(mutex_);
        auto it = server_capabilities_.find(std::string(server_name));
        if (it == server_capabilities_.end()) return std::nullopt;
        return it->second;
    }

    /// Get all tracked server capabilities (copy).
    std::unordered_map<std::string, ServerCapabilities>
    get_all_capabilities() const {
        std::lock_guard lock(mutex_);
        return server_capabilities_;
    }

    /// Refresh capabilities from the current manager's server instances.
    /// Called after successful initialization.
    void refresh_capabilities() {
        std::lock_guard lock(mutex_);
        refresh_capabilities_locked();
    }

    // ------------------------------------------------------------------
    // Event dispatch
    // ------------------------------------------------------------------

    /// Dispatch an event to all registered callbacks.
    /// Thread-safe: copies callbacks under lock, then dispatches without lock.
    void emit_event(const LspEvent& event) {
        std::vector<LspEventCallback> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks.reserve(event_callbacks_.size());
            for (const auto& [token, cb] : event_callbacks_) {
                if (cb) callbacks.push_back(cb);
            }
        }
        // Dispatch outside the lock to avoid deadlocks
        for (const auto& cb : callbacks) {
            cb(event);
        }
    }

    /// Register a callback for LSP events.
    /// Returns a token that can be used to unregister.
    uint64_t on_event(LspEventCallback callback) {
        std::lock_guard lock(mutex_);
        uint64_t token = ++next_event_token_;
        event_callbacks_.emplace_back(token, std::move(callback));
        return token;
    }

    /// Unregister an event callback by its token.
    void off_event(uint64_t token) {
        std::lock_guard lock(mutex_);
        std::erase_if(event_callbacks_, [token](const auto& entry) {
            return entry.first == token;
        });
    }

    // ------------------------------------------------------------------
    // Test support
    // ------------------------------------------------------------------

    /// Test-only sync reset. Clears module-scope singleton state so
    /// initialize() early-returns on 'not-started' in downstream tests.
    ///
    /// TS REF: src/services/lsp/manager.ts:48-53 (_resetLspManagerForTesting)
    void _reset_for_testing() {
        std::lock_guard lock(mutex_);
        state_ = InitializationState::NotStarted;
        error_.reset();
        init_future_ = {};
        ++generation_;
        manager_.reset();
        server_capabilities_.clear();
    }

private:
    LspManager() = default;
    ~LspManager() = default;
    LspManager(const LspManager&) = delete;
    LspManager& operator=(const LspManager&) = delete;

    // ------------------------------------------------------------------
    // Internal helpers (caller must hold mutex_)
    // ------------------------------------------------------------------

    void refresh_capabilities_locked() {
        if (!manager_) return;

        const auto& servers = manager_->get_all_servers();
        for (const auto& [name, instance] : servers) {
            if (!instance) continue;

            ServerCapabilities caps;
            caps.server_info = instance->server_info;

            // A server that successfully initialized supports at least
            // textDocumentSync (diagnostics) and basic request handling.
            // TS REF: LSPServerInstance.ts capabilities parsed from init response
            caps.diagnostics = true;

            // Mark as ready if state is Ready
            if (instance->state == ServerInstanceState::Ready) {
                caps.hover = true;
                caps.definition = true;
                caps.references = true;
                caps.document_symbol = true;
            }

            server_capabilities_[name] = std::move(caps);
        }
    }

    void emit_event_locked(const LspEvent& event) {
        // Copy callbacks to avoid holding lock during dispatch
        auto callbacks = event_callbacks_;
        mutex_.unlock();

        for (const auto& [token, cb] : callbacks) {
            if (cb) cb(event);
        }

        mutex_.lock();
    }

    static int64_t current_time_ms() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    mutable std::mutex mutex_;
    std::unique_ptr<LSPServerManager> manager_;
    InitializationState state_{InitializationState::NotStarted};
    std::optional<std::string> error_;
    uint64_t generation_{0};  ///< Prevents stale init promises from updating state
    std::shared_future<void> init_future_;

    /// Per-server capability tracking.
    std::unordered_map<std::string, ServerCapabilities> server_capabilities_;

    /// Event callback registry: token -> callback.
    std::vector<std::pair<uint64_t, LspEventCallback>> event_callbacks_;
    uint64_t next_event_token_{0};
};

// ============================================================================
// Free functions: TS-compatible API
// ============================================================================

/// Get the singleton LSP server manager instance.
/// Returns nullptr if not yet initialized, initialization failed, or still pending.
/// TS REF: src/services/lsp/manager.ts:63-69
inline LSPServerManager* getLspServerManager() {
    return LspManager::instance().get_manager();
}

/// Get the current initialization status of the LSP server manager.
/// TS REF: src/services/lsp/manager.ts:76-94
inline InitializationStatus getInitializationStatus() {
    return LspManager::instance().get_status();
}

/// Check whether at least one language server is connected and healthy.
/// TS REF: src/services/lsp/manager.ts:100-110
inline bool isLspConnected() {
    return LspManager::instance().is_connected();
}

/// Wait for LSP server manager initialization to complete.
/// TS REF: src/services/lsp/manager.ts:121-133
inline std::shared_future<void> waitForInitialization() {
    return LspManager::instance().wait_for_initialization();
}

/// Initialize the LSP server manager singleton.
/// Safe to call multiple times - idempotent.
/// TS REF: src/services/lsp/manager.ts:145-208
inline void initializeLspServerManager() {
    LspManager::instance().initialize();
}

/// Force re-initialization after plugin refresh.
/// TS REF: src/services/lsp/manager.ts:226-253
inline void reinitializeLspServerManager() {
    LspManager::instance().reinitialize();
}

/// Shutdown the LSP server manager and clean up resources.
/// TS REF: src/services/lsp/manager.ts:267-289
inline std::future<void> shutdownLspServerManager() {
    return LspManager::instance().shutdown();
}

/// Test-only sync reset.
/// TS REF: src/services/lsp/manager.ts:48-53
inline void _resetLspManagerForTesting() {
    LspManager::instance()._reset_for_testing();
}

/// Get tracked capabilities for a specific server.
inline std::optional<ServerCapabilities> getServerCapabilities(
    std::string_view server_name
) {
    return LspManager::instance().get_server_capabilities(server_name);
}

/// Register a callback for LSP lifecycle events.
inline uint64_t onLspEvent(LspEventCallback callback) {
    return LspManager::instance().on_event(std::move(callback));
}

/// Unregister an event callback.
inline void offLspEvent(uint64_t token) {
    LspManager::instance().off_event(token);
}

} // namespace cc::services::lsp