// app_store_bridge.cpp — impl unit for the engine/app-store CommandContext
// bridge free functions, kept OUT of app.cppm so the interface BMI need not
// import cc.state.store / cc.state.app_state.
//
// Contains: compact_runtime_messages, compact_runtime_apply,
//           app_store_dispatch, app_store_get_state,
//           command_context_for_engine.
module;


module cc.ui.app.app;

import std;

import cc.types.types;
import cc.types.command;
import cc.commands.command;
import cc.query.query_engine;
import cc.state.store;
import cc.state.app_state;

namespace cc::ui {

[[nodiscard]] std::vector<Message> compact_runtime_messages(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    return engine ? engine->get_conversation() : std::vector<Message>{};
}

[[nodiscard]] VoidResult compact_runtime_apply(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    if (!engine) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            "No active query engine is available for compaction"));
    }
    auto compacted = engine->compact_conversation();
    if (!compacted) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            compacted.error().format()));
    }
    return VoidResult{};
}

// ============================================================
// AppStore bridge for CommandContext
// ============================================================

/// dispatch_fn implementation: casts void* back to AppStore*, int back to
/// ActionType, and dispatches.  Payload types are inferred from the action
/// type (the common bool / optional-string / enum cases); anything
/// unrecognised falls back to a payload-less dispatch.
void app_store_dispatch(void* store_ptr, int action_type_int,
                        const void* payload) {
    using cc::state::ActionType;
    auto* store = static_cast<cc::state::AppStore*>(store_ptr);
    if (!store) return;
    const auto at = static_cast<ActionType>(action_type_int);

    using cc::state::Action;
    switch (at) {
        // ── Bool-payload actions ──────────────────────────────────
        case ActionType::SetLoading:
        case ActionType::SetStreaming:
        case ActionType::SetVerbose:
        case ActionType::SetBriefOnly:
        case ActionType::SetFastMode:
        case ActionType::ToggleCompactMode:
        case ActionType::ToggleThinking:
            if (payload) {
                store->dispatch(Action{at, *static_cast<const bool*>(payload)});
            } else {
                store->dispatch(Action{at});
            }
            break;

        // ── String-payload actions ────────────────────────────────
        // Reducer expects std::string directly (not optional).
        case ActionType::SetError:
        case ActionType::GrantPermission:
        case ActionType::RevokePermission:
        case ActionType::SetWorkingDirectory:
        case ActionType::SetOutputStyle:
        case ActionType::AddNotification:
        case ActionType::DismissNotification:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const std::string*>(payload)});
            } else {
                store->dispatch(Action{at});
            }
            break;

        // ── Optional-string-payload actions ───────────────────────
        // Reducer expects std::optional<std::string>; caller passes a
        // std::string* which we wrap.
        case ActionType::SetStatusLineText:
        case ActionType::SetSpinnerTip:
        case ActionType::SetSlashCommand:
        case ActionType::SetMainLoopModel:
        case ActionType::SetAdvisorModel:
        case ActionType::SetEffortValue:
            if (payload) {
                store->dispatch(Action{at,
                    std::optional<std::string>{*static_cast<const std::string*>(payload)}});
            } else {
                store->dispatch(Action{at, std::optional<std::string>{}});
            }
            break;

        // ── ExpandedView enum payload ─────────────────────────────
        case ActionType::SetExpandedView:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const cc::state::ExpandedView*>(payload)});
            } else {
                store->dispatch(Action{at, cc::state::ExpandedView::None});
            }
            break;

        // ── PermissionMode enum payload ──────────────────────────
        case ActionType::SetPermissionMode:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const cc::state::PermissionMode*>(payload)});
            } else {
                store->dispatch(Action{at, cc::state::PermissionMode::Default});
            }
            break;

        // ── Payload-less actions ──────────────────────────────────
        case ActionType::ClearMessages:
        case ActionType::ResetSession:
        case ActionType::ClearError:
        case ActionType::SaveState:
        case ActionType::LoadState:
        case ActionType::ClearSavedState:
        default:
            store->dispatch(Action{at});
            break;
    }
}

/// get_state_fn implementation: returns a thread-local snapshot of AppState
/// so the returned pointer stays valid until the next call on this thread.
const void* app_store_get_state(void* store_ptr) {
    auto* store = static_cast<cc::state::AppStore*>(store_ptr);
    if (!store) return nullptr;
    thread_local static cc::state::AppState snapshot;
    snapshot = store->get_state();
    return &snapshot;
}

[[nodiscard]] CommandContext command_context_for_engine(
    core::QueryEngine* engine,
    void* app_store,
    std::string cwd) {
    if (cwd.empty() && engine) cwd = engine->working_directory();
    return CommandContext{
        .args = {},
        .raw_input = {},
        .cwd = std::move(cwd),
        .runtime_state = engine,
        .compact_message_provider = compact_runtime_messages,
        .compact_applier = compact_runtime_apply,
        .app_store = app_store,
        .dispatch_fn = app_store ? app_store_dispatch : nullptr,
        .get_state_fn = app_store ? app_store_get_state : nullptr,
    };
}


// Type-erased AppStore factory (keeps cc.state.* out of the :impl partition).
[[nodiscard]] std::shared_ptr<void> create_typed_app_store() {
    // Adopt the unique_ptr's raw pointer: shared_ptr<void> type-erases the
    // deleter at this construction site, so :impl need never name AppStore.
    return std::shared_ptr<void>(cc::state::create_app_store().release());
}

BridgeState AppAdapter::bridge_state() const {
    BridgeState b{false, false, false, false, false};
    if (auto* raw = app_store_raw()) {
        const auto st = static_cast<cc::state::AppStore*>(raw)->get_state();
        b.enabled        = st.repl_bridge_enabled;
        b.explicit_remote = st.repl_bridge_explicit;
        b.connected      = st.repl_bridge_connected;
        b.session_active = st.repl_bridge_session_active;
        b.reconnecting   = st.repl_bridge_reconnecting;
    }
    return b;
}


}  // namespace cc::ui
