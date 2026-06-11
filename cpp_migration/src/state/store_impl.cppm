/// @file store_impl.cppm
/// @brief Concrete Redux-style AppState store (Phase 3-F).
///
/// Scope (kept intentionally small vs. the generic cc.state.store template):
///   * A single `AppStateData` POD aggregate with all fields that the REPL
///     shell needs for Phase 3 end-to-end scenarios.
///   * An `AppStateStore` object with `dispatch(Action)` / `subscribe(cb)` /
///     `snapshot()` / `register_reducer()` semantics.
///   * Six built-in default reducers cover the most common REPL mutations:
///     `SetActiveSession`, `UpdateDraft`, `SetModel`, `ToggleFlag`,
///     `UpdateSettings`, `AddRecentFile`.  The remaining five action kinds
///     (`CachePermission`, `AddSessionMetadata`, `RemoveSession`,
///     `Tick`) are declared and dispatched through `register_reducer()`;
///     callers plug in custom handlers or rely on the default no-op + bump
///     revision behaviour.
///   * Threading note: Phase 3 runs single-threaded on the REPL event loop
///     so there is no mutex.  Phase 4+ will add a std::shared_mutex
///     (reader-writer lock) without an API break — see comments on the
///     private fields below.
module;

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <cstring>

// JSON payloads are parsed with yyjson.  The same library is already a
// transitive dep of cc_state (see src/CMakeLists.txt cc_state -> yyjson).
#include <yyjson.h>

export module cc.state.store_impl;

export namespace cc::state::store_impl {

// ---------------------------------------------------------------------------
// Core POD types
// ---------------------------------------------------------------------------

enum class SessionStatus : std::uint8_t {
    Idle = 0,
    Streaming = 1,
    WaitingTool = 2,
    Paused = 3,
    Error = 4,
};

struct SessionMeta {
    std::string id;
    std::string title;
    std::time_t created_at = 0;
    std::time_t updated_at = 0;
    int         turn_count = 0;
};

struct ActiveSession {
    std::string   id;
    std::string   model_id;
    SessionStatus status = SessionStatus::Idle;
    std::string   branch;
    std::string   draft_text;
};

struct SettingsView {
    std::string theme_name = "default";
    bool        compact_mode = false;
    int         max_tokens = 4096;
    double      temperature = 1.0;
    std::string model_id = "claude-sonnet-4-6";
};

struct FeatureFlags {
    bool proactive        = false;
    bool bridge_mode      = false;
    bool kairos           = false;
    bool daemon           = false;
    bool voice_mode       = false;
    bool monitor_tool     = false;
    bool templates        = false;
    bool bg_sessions      = false;
    bool byoc_environment = false;
    bool self_hosted      = false;
};

struct RecentFile {
    std::string path;
    std::time_t last_accessed = 0;
};

/// Permission decision enum.  Kept as a plain int in the serialized form
/// for forward compatibility — callers compare against these constants.
inline constexpr int kPermAllowOnce    = 0;
inline constexpr int kPermAlwaysAllow  = 1;
inline constexpr int kPermAlwaysDeny   = 2;

struct PermissionEntry {
    std::string tool_pattern;
    std::string path_glob;
    int         decision = kPermAllowOnce;
};

struct AppStateData {
    std::string                              user_id;
    std::optional<ActiveSession>             active_session;
    std::vector<SessionMeta>                 recent_sessions;
    SettingsView                             settings;
    FeatureFlags                             flags;
    std::vector<RecentFile>                  recent_files;
    std::vector<PermissionEntry>             permissions_cache;
    std::uint64_t                            revision = 0;
};

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

enum class ActionType : std::uint8_t {
    SetActiveSession = 0,
    UpdateDraft,
    SetModel,
    ToggleFlag,
    AddRecentFile,
    UpdateSettings,
    CachePermission,
    AddSessionMetadata,
    RemoveSession,
    Tick,
};

struct Action {
    ActionType  type;
    std::string json_payload;  // JSON object; interpretation is reducer-specific.
};

using Reducer = std::function<AppStateData(const AppStateData&, const Action&)>;

// ---------------------------------------------------------------------------
// Helpers (visible for persistence_json tests too but kept internal here).
// ---------------------------------------------------------------------------

namespace detail {

/// Extract a string field from a yyjson object.  Returns `def` if missing
/// or the value is not actually a string.
[[nodiscard]] inline auto json_str(yyjson_val* obj,
                                   const char* key,
                                   std::string def = {})
    -> std::string {
    if (!obj) return def;
    auto* v = yyjson_obj_get(obj, key);
    if (!v || !yyjson_is_str(v)) return def;
    const char* s = yyjson_get_str(v);
    if (!s) return def;
    return std::string(s, std::strlen(s));
}

[[nodiscard]] inline auto json_bool(yyjson_val* obj,
                                    const char* key,
                                    bool def = false) -> bool {
    if (!obj) return def;
    auto* v = yyjson_obj_get(obj, key);
    if (!v) return def;
    if (yyjson_is_bool(v)) return yyjson_get_bool(v);
    if (yyjson_is_int(v)) return yyjson_get_int(v) != 0;
    return def;
}

[[nodiscard]] inline auto json_int(yyjson_val* obj,
                                   const char* key,
                                   int def = 0) -> int {
    if (!obj) return def;
    auto* v = yyjson_obj_get(obj, key);
    if (!v || !yyjson_is_int(v)) return def;
    return static_cast<int>(yyjson_get_int(v));
}

[[nodiscard]] inline auto json_uint(yyjson_val* obj,
                                    const char* key,
                                    std::uint64_t def = 0) -> std::uint64_t {
    if (!obj) return def;
    auto* v = yyjson_obj_get(obj, key);
    if (!v || !yyjson_is_uint(v) && !yyjson_is_int(v)) return def;
    return yyjson_is_uint(v) ? yyjson_get_uint(v)
                             : static_cast<std::uint64_t>(yyjson_get_sint(v));
}

[[nodiscard]] inline auto json_real(yyjson_val* obj,
                                    const char* key,
                                    double def = 0.0) -> double {
    if (!obj) return def;
    auto* v = yyjson_obj_get(obj, key);
    if (!v) return def;
    if (yyjson_is_real(v)) return yyjson_get_real(v);
    if (yyjson_is_int(v))  return static_cast<double>(yyjson_get_sint(v));
    return def;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Default reducers
// ---------------------------------------------------------------------------

namespace default_reducers {

/// SetActiveSession: payload = { id, model_id, status?, branch?, draft_text? }
inline auto SetActiveSession(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string id = detail::json_str(root, "id");
    if (id.empty()) { yyjson_doc_free(doc); ++out.revision; return out; }
    ActiveSession s{};
    s.id       = std::move(id);
    s.model_id = detail::json_str(root, "model_id", in.settings.model_id);
    int st     = detail::json_int(root, "status",
                                  static_cast<int>(SessionStatus::Idle));
    s.status   = static_cast<SessionStatus>(std::clamp(st, 0, 4));
    s.branch   = detail::json_str(root, "branch");
    s.draft_text = detail::json_str(root, "draft_text");
    out.active_session = std::move(s);
    yyjson_doc_free(doc);
    ++out.revision;
    return out;
}

/// UpdateDraft: payload = { draft_text } (applies to active_session if any).
inline auto UpdateDraft(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    if (!out.active_session.has_value()) { ++out.revision; return out; }
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    out.active_session->draft_text = detail::json_str(root, "draft_text");
    yyjson_doc_free(doc);
    ++out.revision;
    return out;
}

/// SetModel: payload = { model_id } → writes both active_session.model_id
/// and settings.model_id (so the setting persists across sessions).
inline auto SetModel(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string mid = detail::json_str(root, "model_id");
    yyjson_doc_free(doc);
    if (mid.empty()) { ++out.revision; return out; }
    out.settings.model_id = mid;
    if (out.active_session) out.active_session->model_id = mid;
    ++out.revision;
    return out;
}

/// ToggleFlag: payload = { name: "<flag>" }
/// Flips a boolean in the FeatureFlags struct by name match.
inline auto ToggleFlag(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string name = detail::json_str(root, "name");
    yyjson_doc_free(doc);
    auto flip = [&](bool& f) { f = !f; return true; };
    bool matched =
        (name == "proactive")        ? flip(out.flags.proactive) :
        (name == "bridge_mode")      ? flip(out.flags.bridge_mode) :
        (name == "kairos")           ? flip(out.flags.kairos) :
        (name == "daemon")           ? flip(out.flags.daemon) :
        (name == "voice_mode")       ? flip(out.flags.voice_mode) :
        (name == "monitor_tool")     ? flip(out.flags.monitor_tool) :
        (name == "templates")        ? flip(out.flags.templates) :
        (name == "bg_sessions")      ? flip(out.flags.bg_sessions) :
        (name == "byoc_environment") ? flip(out.flags.byoc_environment) :
        (name == "self_hosted")      ? flip(out.flags.self_hosted) : false;
    (void)matched;
    ++out.revision;
    return out;
}

/// UpdateSettings: payload = { theme_name?, compact_mode?, max_tokens?,
/// temperature?, model_id? }.  All fields are optional.
inline auto UpdateSettings(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (auto* v = yyjson_obj_get(root, "theme_name");
        v && yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        if (s) out.settings.theme_name.assign(s, std::strlen(s));
    }
    if (auto* v = yyjson_obj_get(root, "compact_mode");
        v && yyjson_is_bool(v)) out.settings.compact_mode = yyjson_get_bool(v);
    if (auto* v = yyjson_obj_get(root, "max_tokens");
        v && yyjson_is_int(v))  out.settings.max_tokens = static_cast<int>(yyjson_get_int(v));
    if (auto* v = yyjson_obj_get(root, "temperature");
        v && (yyjson_is_real(v) || yyjson_is_int(v))) {
        out.settings.temperature = yyjson_is_real(v)
            ? yyjson_get_real(v)
            : static_cast<double>(yyjson_get_sint(v));
    }
    if (auto* v = yyjson_obj_get(root, "model_id");
        v && yyjson_is_str(v)) {
        const char* s = yyjson_get_str(v);
        if (s) out.settings.model_id.assign(s, std::strlen(s));
    }
    yyjson_doc_free(doc);
    ++out.revision;
    return out;
}

/// AddRecentFile: payload = { path }.
///   * Bumps last_accessed to `time(nullptr)` if already present (unlinks
///     the old entry first).
///   * Truncates recent_files to the most-recent 50 entries to put a hard
///     cap on persistence size.
inline auto AddRecentFile(const AppStateData& in, const Action& a)
    -> AppStateData {
    AppStateData out = in;
    auto* doc = yyjson_read(a.json_payload.c_str(), a.json_payload.size(), 0);
    if (!doc) { ++out.revision; return out; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    std::string path = detail::json_str(root, "path");
    yyjson_doc_free(doc);
    if (path.empty()) { ++out.revision; return out; }

    // Remove any previous entry for this path.
    out.recent_files.erase(
        std::remove_if(out.recent_files.begin(), out.recent_files.end(),
                       [&](const RecentFile& r) { return r.path == path; }),
        out.recent_files.end());
    // New entry goes to the front (= most recent).
    RecentFile rf;
    rf.path = std::move(path);
    rf.last_accessed = std::time(nullptr);
    out.recent_files.insert(out.recent_files.begin(), std::move(rf));
    // Cap size at 50.
    constexpr size_t kMaxRecent = 50;
    if (out.recent_files.size() > kMaxRecent) {
        out.recent_files.resize(kMaxRecent);
    }
    ++out.revision;
    return out;
}

} // namespace default_reducers

// ---------------------------------------------------------------------------
// AppStateStore
// ---------------------------------------------------------------------------

class AppStateStore {
public:
    AppStateStore() = default;
    explicit AppStateStore(AppStateData initial) : state_(std::move(initial)) {
        RegisterBuiltins();
    }

    /// Register a reducer for a specific action type.  If multiple calls
    /// happen for the same `t`, the latest one wins (useful for tests).
    void register_reducer(ActionType t, Reducer r) {
        reducers_[static_cast<int>(t)] = std::move(r);
    }

    /// Subscribe to post-dispatch state snapshots.  Returns a token usable
    /// with unsubscribe().  The callback is invoked synchronously inside
    /// dispatch() after the reducer has run.
    ///
    /// Threading: Phase 3 is single-threaded (REPL event loop).  When Phase 4
    /// introduces multi-threaded dispatch (SSE reader + TUI input), guard this
    /// method and dispatch() with a std::shared_mutex.  subscribe/unsubscribe
    /// take a unique_lock; dispatch takes shared_lock for the reducer then
    /// unique_lock for notification.  A reader-writer lock is preferred over a
    /// plain mutex because snapshot() is a hot read path that should not block
    /// concurrent readers.
    [[nodiscard]] auto subscribe(std::function<void(const AppStateData&)> cb)
        -> std::uint64_t {
        const auto tok = next_token_++;
        subscribers_.emplace_back(tok, std::move(cb));
        return tok;
    }

    void unsubscribe(std::uint64_t token) {
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                           [&](const auto& p) { return p.first == token; }),
            subscribers_.end());
    }

    /// Apply `action` via its registered reducer (or a no-op revision bump
    /// when none is registered), then notify subscribers.
    void dispatch(Action a) {
        auto it = reducers_.find(static_cast<int>(a.type));
        if (it == reducers_.end()) {
            // Default: no-op, but still bump revision so subscribers can
            // detect an action-of-interest was fired.
            ++state_.revision;
        } else {
            state_ = it->second(state_, a);
        }
        // Notify all subscribers with the latest snapshot.
        const AppStateData& snap = state_;
        for (auto& [_, cb] : subscribers_) if (cb) cb(snap);
    }

    [[nodiscard]] auto snapshot() const noexcept -> const AppStateData& {
        return state_;
    }

private:
    void RegisterBuiltins() {
        using namespace default_reducers;
        reducers_[static_cast<int>(ActionType::SetActiveSession)] = SetActiveSession;
        reducers_[static_cast<int>(ActionType::UpdateDraft)]      = UpdateDraft;
        reducers_[static_cast<int>(ActionType::SetModel)]         = SetModel;
        reducers_[static_cast<int>(ActionType::ToggleFlag)]       = ToggleFlag;
        reducers_[static_cast<int>(ActionType::UpdateSettings)]   = UpdateSettings;
        reducers_[static_cast<int>(ActionType::AddRecentFile)]    = AddRecentFile;
    }

    AppStateData                                                     state_{};
    std::vector<std::pair<std::uint64_t,
                          std::function<void(const AppStateData&)>>> subscribers_;
    std::unordered_map<int, Reducer>                                reducers_;
    std::uint64_t                                                   next_token_ = 1;
    // Phase 4 threading: add `mutable std::shared_mutex mu_;` here.
    // - snapshot() acquires shared_lock (allows concurrent readers).
    // - dispatch() acquires unique_lock (serialises state mutations).
    // - subscribe/unsubscribe acquire unique_lock (modify subscriber list).
    // A std::shared_mutex (reader-writer lock) is chosen over a plain mutex
    // because snapshot() is called on every render frame and must not block
    // other readers.  The tradeoff is slightly higher overhead for writers,
    // which is acceptable given dispatch frequency is orders of magnitude
    // lower than render frequency.
};

} // namespace cc::state::store_impl
