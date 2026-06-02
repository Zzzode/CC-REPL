/// @file store.cppm
/// @brief Redux-like state management store for the Claude Code REPL.
/// Provides a generic Store class template with actions, reducers,
/// middleware pipeline, subscriptions, and async thunk support.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <optional>
#include <expected>
#include <format>
#include <concepts>
#include <variant>
#include <any>
#include <queue>
#include <future>
#include <chrono>

export module cc.state.store;

import cc.types.types;
import cc.state.app_state;
import cc.state.selectors;
import cc.state.persistence;
import cc.state.on_change;

export namespace cc::state {

// ============================================================
// Action System
// ============================================================

/// Extended action type identifiers
enum class ActionType : std::uint16_t {
    // Message actions
    AddMessage,
    ClearMessages,
    UpdateLastMessage,
    
    // Loading/streaming
    SetLoading,
    SetStreaming,
    
    // Error handling
    SetError,
    ClearError,
    
    // Token usage
    UpdateUsage,
    
    // Model management
    SwitchModel,
    SetMainLoopModel,
    
    // Permission actions
    GrantPermission,
    RevokePermission,
    SetPermissionMode,
    
    // Tool management
    EnableTool,
    DisableTool,
    
    // UI state
    ToggleCompactMode,
    ToggleThinking,
    SetSlashCommand,
    AddNotification,
    DismissNotification,
    SetStatusLineText,
    SetExpandedView,
    SetFooterSelection,
    SetSpinnerTip,
    SetBriefOnly,
    SetShowTeammatePreview,
    SetSelectedAgentIndex,
    SetCoordinatorTaskIndex,
    SetViewSelectionMode,
    
    // Session lifecycle
    ResetSession,
    UpdateActivity,
    SetWorkingDirectory,
    
    // Settings
    UpdateSettings,
    SetVerbose,
    SetFastMode,
    SetThinkingEnabled,
    SetPromptSuggestionEnabled,
    
    // Bridge
    SetBridgeEnabled,
    SetBridgeExplicit,
    SetBridgeOutboundOnly,
    SetBridgeConnected,
    SetBridgeSessionActive,
    SetBridgeReconnecting,
    SetBridgeConnectUrl,
    SetBridgeSessionUrl,
    SetBridgeEnvironmentId,
    SetBridgeSessionId,
    SetBridgeError,
    SetBridgeInitialName,
    SetShowRemoteCallout,
    
    // Remote
    SetRemoteSessionUrl,
    SetRemoteConnectionStatus,
    SetRemoteBackgroundTaskCount,
    
    // Tasks
    AddTask,
    RemoveTask,
    UpdateTask,
    SetForegroundedTaskId,
    SetViewingAgentTaskId,
    
    // Agent
    RegisterAgentName,
    SetAgent,
    SetKairosEnabled,
    
    // Companion
    SetCompanionReaction,
    SetCompanionPetTime,
    
    // MCP
    UpdateMcpState,
    IncrementMcpReconnectKey,
    
    // Plugins
    UpdatePluginsState,
    SetPluginsNeedRefresh,
    
    // Speculation
    SetSpeculationState,
    SetSpeculationTimeSaved,
    
    // Skill improvement
    SetSkillSuggestion,
    
    // Auth
    IncrementAuthVersion,
    
    // Initial message
    SetInitialMessage,
    ClearInitialMessage,
    
    // Effort
    SetEffortValue,
    
    // Overlays
    AddActiveOverlay,
    RemoveActiveOverlay,
    ClearActiveOverlays,
    
    // Advisor
    SetAdvisorModel,
    
    // Ultraplan
    SetUltraplanLaunching,
    SetUltraplanSessionUrl,
    SetUltraplanPendingChoice,
    SetUltraplanLaunchPending,
    SetUltraplanMode,
    
    // Worker sandbox
    AddSandboxPermissionRequest,
    RemoveSandboxPermissionRequest,
    SetSelectedSandboxPermissionIndex,
    SetPendingWorkerRequest,
    SetPendingSandboxRequest,
    
    // Prompt suggestion
    SetPromptSuggestion,
    ClearPromptSuggestion,
    
    // Inbox
    AddInboxMessage,
    RemoveInboxMessage,
    ClearInboxMessages,
    
    // Persistence
    SaveState,
    LoadState,
    ClearSavedState,
    
    // Batch actions
    Batch,
};

/// Extended action carrying a type tag and optional payload
struct Action {
    ActionType type;
    std::any payload;
    
    /// Construct action with no payload
    explicit Action(ActionType t) : type(t) {}
    
    /// Construct action with typed payload
    template <typename T>
    Action(ActionType t, T&& data) : type(t), payload(std::forward<T>(data)) {}
    
    /// Extract payload as a specific type (returns nullopt if wrong type)
    template <typename T>
    [[nodiscard]] std::optional<T> get_payload() const {
        if (auto* p = std::any_cast<T>(&payload)) {
            return *p;
        }
        return std::nullopt;
    }
    
    /// Check if payload holds a specific type
    template <typename T>
    [[nodiscard]] bool has_payload() const {
        return payload.type() == typeid(T);
    }
};

// ============================================================
// Reducer
// ============================================================

/// Reducer concept: takes state + action, produces new state
template <typename F, typename State>
concept Reducer = std::invocable<F, const State&, const Action&> &&
                  std::same_as<std::invoke_result_t<F, const State&, const Action&>, State>;

/// Extended reducer for AppState
[[nodiscard]] inline AppState app_reducer(const AppState& state, const Action& action) {
    AppState next = state;
    
    switch (action.type) {
        // ========================================
        // Message actions
        // ========================================
        case ActionType::AddMessage: {
            if (auto msg = action.get_payload<cc::core::Message>()) {
                return with_message(state, std::move(*msg));
            }
            break;
        }
        case ActionType::ClearMessages: {
            next.messages.clear();
            break;
        }
        
        // ========================================
        // Loading/streaming
        // ========================================
        case ActionType::SetLoading: {
            if (auto val = action.get_payload<bool>()) {
                return with_loading(state, *val);
            }
            break;
        }
        case ActionType::SetStreaming: {
            if (auto val = action.get_payload<bool>()) {
                next.is_streaming = *val;
            }
            break;
        }
        
        // ========================================
        // Error handling
        // ========================================
        case ActionType::SetError: {
            if (auto msg = action.get_payload<std::string>()) {
                return with_error(state, std::move(*msg));
            }
            break;
        }
        case ActionType::ClearError: {
            return clear_error(state);
        }
        
        // ========================================
        // Token usage
        // ========================================
        case ActionType::UpdateUsage: {
            if (auto usage = action.get_payload<cc::core::TokenUsage>()) {
                return with_usage(state, *usage);
            }
            break;
        }
        
        // ========================================
        // Model management
        // ========================================
        case ActionType::SwitchModel: {
            if (auto model = action.get_payload<ModelConfig>()) {
                return with_model(state, std::move(*model));
            }
            break;
        }
        case ActionType::SetMainLoopModel: {
            if (auto model = action.get_payload<std::optional<std::string>>()) {
                next.main_loop_model = std::move(*model);
            }
            break;
        }
        
        // ========================================
        // Permission actions
        // ========================================
        case ActionType::SetPermissionMode: {
            if (auto mode = action.get_payload<PermissionMode>()) {
                return with_permission_mode(state, *mode);
            }
            break;
        }
        
        // ========================================
        // UI state
        // ========================================
        case ActionType::ToggleCompactMode: {
            next.compact_mode = !next.compact_mode;
            break;
        }
        case ActionType::ToggleThinking: {
            next.show_thinking = !next.show_thinking;
            break;
        }
        case ActionType::SetVerbose: {
            if (auto val = action.get_payload<bool>()) {
                return with_verbose(state, *val);
            }
            break;
        }
        case ActionType::SetExpandedView: {
            if (auto view = action.get_payload<ExpandedView>()) {
                return with_expanded_view(state, *view);
            }
            break;
        }
        case ActionType::SetFastMode: {
            if (auto val = action.get_payload<bool>()) {
                next.fast_mode = *val;
            }
            break;
        }
        
        // ========================================
        // Session lifecycle
        // ========================================
        case ActionType::ResetSession: {
            next = get_default_app_state();
            // Keep some configuration from previous state
            next.current_model = state.current_model;
            next.available_models = state.available_models;
            next.tool_permission_context = state.tool_permission_context;
            next.working_directory = state.working_directory;
            next.settings = state.settings;
            break;
        }
        case ActionType::UpdateActivity: {
            next.last_activity = std::chrono::system_clock::now();
            break;
        }
        case ActionType::SetWorkingDirectory: {
            if (auto dir = action.get_payload<std::string>()) {
                next.working_directory = std::move(*dir);
            }
            break;
        }
        
        // ========================================
        // Batch actions
        // ========================================
        case ActionType::Batch: {
            if (auto actions = action.get_payload<std::vector<Action>>()) {
                AppState batch_state = state;
                for (const auto& batch_action : *actions) {
                    batch_state = app_reducer(batch_state, batch_action);
                }
                return batch_state;
            }
            break;
        }
        
        default:
            break;
    }
    
    return next;
}

// ============================================================
// Middleware
// ============================================================

/// Middleware type: wraps dispatch to intercept/transform actions
using DispatchFn = std::function<void(const Action&)>;
using Middleware = std::function<DispatchFn(DispatchFn next)>;

/// Logging middleware: prints dispatched actions (for debug)
[[nodiscard]] inline Middleware logging_middleware() {
    return [](DispatchFn next) -> DispatchFn {
        return [next = std::move(next)](const Action& action) {
            // In production, log to file or structured logger
            next(action);
        };
    };
}

/// Persistence middleware: saves state on specific actions
[[nodiscard]] inline Middleware persistence_middleware(
    std::shared_ptr<persistence::StatePersistence> persistence
) {
    return [persistence = std::move(persistence)](DispatchFn next) -> DispatchFn {
        return [next = std::move(next), persistence](const Action& action) {
            next(action);
            // Save state on certain actions if auto-save is enabled
            if (action.type == ActionType::AddMessage ||
                action.type == ActionType::SetVerbose ||
                action.type == ActionType::ToggleCompactMode) {
                // Actual save happens in the store after state is updated
            }
        };
    };
}

// ============================================================
// Thunk (Async Action Support)
// ============================================================

/// Thunk type: an async action that receives dispatch + getState
using GetStateFn = std::function<AppState()>;
using Thunk = std::function<void(DispatchFn dispatch, GetStateFn get_state)>;

// ============================================================
// Store Class Template
// ============================================================

/// Enhanced Redux-like state store with middleware pipeline and subscriptions
template <typename State, Reducer<State> ReducerFn>
class Store {
    State state_;
    ReducerFn reducer_;
    mutable std::shared_mutex mutex_;
    
    // Subscription management
    std::unordered_map<SubscriptionId, StateObserver> subscribers_;
    SubscriptionId next_sub_id_{1};
    
    // Middleware chain
    std::vector<Middleware> middlewares_;
    DispatchFn dispatch_chain_;
    
    // State change callback integration
    std::shared_ptr<on_change::StateChangeRegistry> change_registry_;
    
    // Persistence integration
    std::shared_ptr<persistence::StatePersistence> persistence_;
    std::chrono::steady_clock::time_point last_persist_time_;
    static constexpr auto AUTO_PERSIST_INTERVAL = std::chrono::seconds(5);

public:
    /// Construct store with initial state and reducer
    explicit Store(
        State initial,
        ReducerFn reducer,
        std::shared_ptr<on_change::StateChangeRegistry> registry = nullptr,
        std::shared_ptr<persistence::StatePersistence> persistence = nullptr
    ) : state_(std::move(initial)),
        reducer_(std::move(reducer)),
        change_registry_(std::move(registry)),
        persistence_(std::move(persistence)) {
        rebuild_dispatch_chain();
    }

    /// Get current state snapshot (thread-safe)
    [[nodiscard]] State get_state() const {
        std::shared_lock lock(mutex_);
        return state_;
    }

    /// Dispatch a synchronous action through the middleware chain
    void dispatch(const Action& action) {
        dispatch_chain_(action);
    }

    /// Dispatch a thunk (async action)
    void dispatch_thunk(Thunk thunk) {
        auto dispatch_fn = [this](const Action& a) { dispatch(a); };
        auto get_state_fn = [this]() { return get_state(); };
        thunk(dispatch_fn, get_state_fn);
    }

    /// Add middleware (must call before first dispatch for correct ordering)
    void add_middleware(Middleware mw) {
        std::unique_lock lock(mutex_);
        middlewares_.push_back(std::move(mw));
        rebuild_dispatch_chain();
    }

    /// Subscribe to state changes
    [[nodiscard]] SubscriptionId subscribe(StateObserver observer) {
        std::unique_lock lock(mutex_);
        auto id = next_sub_id_++;
        subscribers_.emplace(id, std::move(observer));
        return id;
    }

    /// Unsubscribe from state changes
    void unsubscribe(SubscriptionId id) {
        std::unique_lock lock(mutex_);
        subscribers_.erase(id);
    }

    /// Set the state change registry
    void set_change_registry(std::shared_ptr<on_change::StateChangeRegistry> registry) {
        std::unique_lock lock(mutex_);
        change_registry_ = std::move(registry);
    }

    /// Set the persistence manager
    void set_persistence(std::shared_ptr<persistence::StatePersistence> persistence) {
        std::unique_lock lock(mutex_);
        persistence_ = std::move(persistence);
    }

    /// Manually save the current state
    [[nodiscard]] cc::core::VoidResult save_state() {
        if (!persistence_) {
            return {}; // No persistence configured
        }
        auto saved = persistence_->save_state(get_state());
        if (!saved) {
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::InternalError,
                saved.error().format()));
        }
        return {};
    }

    /// Load state from persistence
    [[nodiscard]] std::expected<bool, cc::core::Error> load_state() {
        if (!persistence_) {
            return false; // No persistence configured
        }
        auto loaded = persistence_->load_state();
        if (!loaded) {
            return std::unexpected(cc::core::Error::make(
                cc::core::ErrorCode::InternalError,
                loaded.error().format()));
        }
        // Update state with loaded value
        State prev;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = std::move(*loaded);
        }
        notify(prev, state_);
        return true;
    }

private:
    /// Core dispatch: apply reducer and notify subscribers
    void core_dispatch(const Action& action) {
        State prev;
        State next;
        {
            std::unique_lock lock(mutex_);
            prev = state_;
            state_ = reducer_(state_, action);
            next = state_;
        }
        notify(prev, next);
    }

    /// Rebuild middleware pipeline after adding new middleware
    void rebuild_dispatch_chain() {
        // Start from core dispatch
        DispatchFn chain = [this](const Action& a) { core_dispatch(a); };
        // Apply middlewares in reverse order (outermost first)
        for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
            chain = (*it)(std::move(chain));
        }
        dispatch_chain_ = std::move(chain);
    }

    /// Notify all subscribers and change registry
    void notify(const State& prev, const State& next) {
        // Notify subscribers
        std::vector<StateObserver> observers;
        {
            std::shared_lock lock(mutex_);
            observers.reserve(subscribers_.size());
            for (const auto& [_, ob] : subscribers_) {
                observers.push_back(ob);
            }
        }
        for (const auto& ob : observers) {
            ob(prev, next);
        }
        
        // Notify change registry
        if (change_registry_) {
            change_registry_->run_callbacks(prev, next);
        }
        
        // Auto-persist if needed
        if (persistence_ && persistence_->is_auto_save_enabled()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_persist_time_ >= AUTO_PERSIST_INTERVAL) {
                auto result = persistence_->save_state(next);
                if (result) {
                    last_persist_time_ = now;
                }
            }
        }
    }
};

// ============================================================
// Convenience Type Alias and Factory
// ============================================================

/// The application store type with default reducer
using AppStore = Store<AppState, decltype(&app_reducer)>;

/// Factory function to create the application store with full setup
[[nodiscard]] inline std::unique_ptr<AppStore> create_app_store(
    AppState initial = get_default_app_state()
) {
    // Create change registry and set up default handlers
    auto registry = std::make_shared<on_change::StateChangeRegistry>();
    on_change::setup_default_handlers(*registry);
    
    // Create persistence manager
    auto persistence = std::make_shared<persistence::StatePersistence>(
        persistence::get_default_state_file_path()
    );
    
    // Create store with all integrations
    auto store = std::make_unique<AppStore>(
        std::move(initial),
        &app_reducer,
        registry,
        persistence
    );
    
    // Add logging middleware (optional, can be conditional)
    store->add_middleware(logging_middleware());
    
    return store;
}

} // namespace cc::state
