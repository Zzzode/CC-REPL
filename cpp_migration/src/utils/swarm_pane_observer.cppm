// C++23 leader-side pane observer module.
//
// Net-new C++ feature (no TS counterpart): a leader-owned background poller
// that periodically runs 'tmux capture-pane' against every spawned teammate
// pane and keeps a mutex-guarded snapshot map the swarm UI can render without
// focus-switching to the pane.
module;

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.utils.swarm_pane_observer;

import cc.utils.swarm_backends;

export namespace cc::utils::pane_observer {

namespace sw = cc::utils::swarm_backends;

// ── Event-driven change subscription ──────────────────────────────────────
// The poller jthread is allowed (background tmux capture), but FTXUI must not
// render at a constant rate: after a capture pass bumps any snapshot revision
// the observer invokes these callbacks, and the UI layer responds by flagging
// a dirty atomic + posting one FTXUI event (the only wake path).
namespace pane_observer_detail {

struct ChangeSubscriptionState {
    std::mutex mtx;
    std::uint64_t next_token = 1;
    // Per-callback alive guard so notify() never invokes a callback whose
    // subscriber (e.g. AppAdapter) has been destroyed, even if an in-flight
    // notify copied the lambda before unsubscribe().
    struct Entry {
        std::weak_ptr<void> alive;
        std::function<void()> cb;
    };
    std::unordered_map<std::uint64_t, Entry> subs;
};

inline ChangeSubscriptionState& change_state() {
    static ChangeSubscriptionState state;
    return state;
}

/// Invoke every still-alive change callback. Callbacks are gathered under the
/// lock and invoked unlocked (so a callback that re-subscribes cannot
/// deadlock), each gated on locking its weak alive guard.
inline void notify_changed() {
    std::vector<std::pair<std::shared_ptr<void>, std::function<void()>>> live;
    {
        auto& st = change_state();
        std::lock_guard lock(st.mtx);
        live.reserve(st.subs.size());
        for (auto& [_, e] : st.subs) {
            if (auto guard = e.alive.lock()) {
                live.emplace_back(std::move(guard), e.cb);
            }
        }
    }
    for (auto& [guard, cb] : live) {
        if (guard && cb) cb();  // guard held for the duration of cb()
    }
}

}  // namespace pane_observer_detail

using PaneChangeCallback = std::function<void()>;

/// Subscribe with an explicit lifetime guard. Hold the returned shared_ptr for
/// as long as the callback's captures (typically `this`) stay valid; releasing
/// it (or unsubscribe_changed) stops future calls safely even mid-notify.
inline std::pair<std::uint64_t, std::shared_ptr<void>>
subscribe_changed_with_guard(PaneChangeCallback cb) {
    auto& st = pane_observer_detail::change_state();
    std::lock_guard lock(st.mtx);
    const std::uint64_t token = st.next_token++;
    auto alive = std::make_shared<int>(1);
    st.subs.emplace(
        token,
        pane_observer_detail::ChangeSubscriptionState::Entry{
            .alive = alive, .cb = std::move(cb)});
    return {token, std::shared_ptr<void>(std::move(alive))};
}

/// Backward-compatible subscribe (caller must keep the callback's captures
/// valid until unsubscribe_changed returns; prefer the _with_guard variant).
inline std::uint64_t subscribe_changed(PaneChangeCallback cb) {
    return subscribe_changed_with_guard(std::move(cb)).first;
}

/// Remove a change subscription. No-op when the token is unknown.
inline void unsubscribe_changed(std::uint64_t token) {
    auto& st = pane_observer_detail::change_state();
    std::lock_guard lock(st.mtx);
    st.subs.erase(token);
}

/// Liveness of a spawned teammate pane as seen by the leader.
enum class PaneRunState : std::uint8_t {
    Running,    ///< Last capture succeeded.
    Done,       ///< Capture failed kFailureThreshold times (pane exited/killed).
};

/// Thread-safe, UI-facing per-agent observation record (the UI reads this).
struct PaneSnapshot {
    std::string agent_id;
    std::vector<std::string> lines;                 ///< Captured tail, split on '\n', trailing blanks trimmed.
    std::string raw;                                ///< Last distinct captured text (dedupe key).
    PaneRunState state = PaneRunState::Running;
    std::uint64_t revision = 0;                     ///< Bumped only when `raw` changes.
    std::chrono::steady_clock::time_point observed_at{};
    int consecutive_failures = 0;
};

/// Background poller that periodically captures every tracked pane via a
/// PaneBackend, dedupes unchanged screens, and exposes a mutex-guarded
/// snapshot map. The jthread is a background shell/file poller only — it is
/// NOT an FTXUI render ticker; the UI pulls get_pane_snapshot() on demand.
class PaneObserver {
public:
    static constexpr int kDefaultTailLines = 200;
    static constexpr std::chrono::milliseconds kDefaultInterval{1500};
    static constexpr int kFailureThreshold = 2;

    explicit PaneObserver(std::shared_ptr<sw::PaneBackend> backend,
                          std::chrono::milliseconds interval = kDefaultInterval,
                          int tail_lines = kDefaultTailLines,
                          bool auto_start = true)
        : backend_(std::move(backend)), interval_(interval), tail_lines_(tail_lines) {
        if (auto_start) start();
    }

    ~PaneObserver() { stop(); }

    PaneObserver(const PaneObserver&) = delete;
    PaneObserver& operator=(const PaneObserver&) = delete;

    /// Begin watching a spawned teammate pane. Re-tracking replaces the pane.
    void track(std::string agent_id, sw::PaneId pane_id, bool inside_tmux = false) {
        {
            std::lock_guard lock(mutex_);
            Entry e;
            e.tracked = {std::move(pane_id), inside_tmux};
            e.snapshot.agent_id = agent_id;
            entries_.insert_or_assign(std::move(agent_id), std::move(e));
        }
        cv_.notify_all();
    }

    /// Stop watching an agent (e.g. team deleted). Its snapshot is dropped.
    void untrack(std::string_view agent_id) {
        {
            std::lock_guard lock(mutex_);
            entries_.erase(std::string(agent_id));
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::vector<std::string> tracked_agent_ids() const {
        std::lock_guard lock(mutex_);
        std::vector<std::string> ids;
        ids.reserve(entries_.size());
        for (const auto& [id, _] : entries_) ids.push_back(id);
        return ids;
    }

    /// Stable UI API: deep-copy snapshot of every tracked agent.
    [[nodiscard]] std::map<std::string, PaneSnapshot> get_pane_snapshot() const {
        std::lock_guard lock(mutex_);
        std::map<std::string, PaneSnapshot> out;
        for (const auto& [id, e] : entries_) out.emplace(id, e.snapshot);
        return out;
    }

    [[nodiscard]] std::optional<PaneSnapshot> get_agent_snapshot(
        std::string_view agent_id
    ) const {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(std::string(agent_id));
        if (it == entries_.end()) return std::nullopt;
        return it->second.snapshot;
    }

    /// Run one capture/dedupe pass synchronously (also used by tests).
    void refresh_once() {
        bool any_changed = false;
        struct Target {
            std::string agent_id;
            sw::PaneId pane_id;
            bool inside;
        };
        std::vector<Target> targets;
        {
            std::lock_guard lock(mutex_);
            targets.reserve(entries_.size());
            for (const auto& [id, e] : entries_) {
                targets.push_back({id, e.tracked.pane_id, e.tracked.inside_tmux});
            }
        }
        for (const auto& t : targets) {
            auto captured = backend_->capture_pane_text(
                t.pane_id, tail_lines_, !t.inside);
            bool changed = false;
            std::lock_guard lock(mutex_);
            auto it = entries_.find(t.agent_id);
            if (it == entries_.end()) continue;  // untracked while shelling out
            auto& snap = it->second.snapshot;
            snap.observed_at = std::chrono::steady_clock::now();
            if (captured) {
                snap.consecutive_failures = 0;
                snap.state = PaneRunState::Running;
                if (*captured != snap.raw) {
                    snap.raw = *captured;
                    snap.lines = split_lines(*captured);
                    ++snap.revision;
                    changed = true;
                }
            } else {
                ++snap.consecutive_failures;
                if (snap.consecutive_failures >= kFailureThreshold) {
                    if (snap.state != PaneRunState::Done) {
                        snap.state = PaneRunState::Done;
                        changed = true;
                    }
                }
            }
            if (changed) any_changed = true;
        }
        // One wake per capture pass (not per pane): the UI projection pulls
        // the full snapshot when it services the posted event.
        if (any_changed) pane_observer_detail::notify_changed();
    }

    void start() {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return;
        thread_ = std::jthread([this](std::stop_token st) { poll_loop(std::move(st)); });
    }

    void stop() {
        if (thread_.joinable()) {
            thread_.request_stop();
            cv_.notify_all();
            thread_.join();
        }
    }

private:
    struct TrackedPane {
        sw::PaneId pane_id;
        bool inside_tmux = false;
    };
    struct Entry {
        TrackedPane tracked;
        PaneSnapshot snapshot;
    };

    void poll_loop(std::stop_token st) {
        while (!st.stop_requested()) {
            refresh_once();
            std::unique_lock lock(mutex_);
            // Plain wait_for (no stop_token overload dependency): stop adds at
            // most one interval of join latency.
            cv_.wait_for(lock, interval_);
        }
    }

    static std::vector<std::string> split_lines(const std::string& raw) {
        std::vector<std::string> lines;
        std::string acc;
        for (char c : raw) {
            if (c == '\n') {
                lines.push_back(std::move(acc));
                acc.clear();
            } else if (c != '\r') {
                acc.push_back(c);
            }
        }
        lines.push_back(std::move(acc));
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        return lines;
    }

    std::shared_ptr<sw::PaneBackend> backend_;
    std::chrono::milliseconds interval_;
    int tail_lines_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, Entry> entries_;
    std::jthread thread_;
};

/// Process-wide observer the leader wires to its cached pane backend.
/// Returns nullptr until ensure_global_pane_observer() has been called or
/// after shutdown_global_pane_observer().
[[nodiscard]] std::shared_ptr<PaneObserver> global_pane_observer();

/// Idempotently create/reuse the global observer for the given backend.
/// Passing a different backend (or a destroyed previous one) recreates it.
[[nodiscard]] std::shared_ptr<PaneObserver> ensure_global_pane_observer(
    std::shared_ptr<sw::PaneBackend> backend,
    std::chrono::milliseconds interval = PaneObserver::kDefaultInterval);

/// Stop the global observer's poller and drop the singleton.
void shutdown_global_pane_observer();

} // namespace cc::utils::pane_observer

// --- process-wide singleton storage -------------------------------------
// Header-only module: all three accessors must share ONE function-local
// state, so route them through a single module-local holder().
namespace cc::utils::pane_observer::pane_observer_detail {

struct GlobalObserverState {
    std::mutex mtx;
    std::shared_ptr<PaneObserver> observer;
    std::weak_ptr<sw::PaneBackend> bound_backend;
};

[[nodiscard]] inline GlobalObserverState& holder() {
    static GlobalObserverState state;
    return state;
}

} // namespace cc::utils::pane_observer::pane_observer_detail

namespace cc::utils::pane_observer {

[[nodiscard]] inline std::shared_ptr<PaneObserver> global_pane_observer() {
    auto& state = pane_observer_detail::holder();
    std::lock_guard lock(state.mtx);
    return state.observer;
}

[[nodiscard]] inline std::shared_ptr<PaneObserver> ensure_global_pane_observer(
    std::shared_ptr<sw::PaneBackend> backend,
    std::chrono::milliseconds interval
) {
    auto& state = pane_observer_detail::holder();
    std::lock_guard lock(state.mtx);
    const auto current = state.bound_backend.lock();
    if (!state.observer || !current || current != backend) {
        // Recreate (and stop the previous poller) when unbound or rebound.
        state.observer = std::make_shared<PaneObserver>(backend, interval);
        state.bound_backend = backend;
    }
    return state.observer;
}

inline void shutdown_global_pane_observer() {
    auto& state = pane_observer_detail::holder();
    std::shared_ptr<PaneObserver> previous;
    {
        std::lock_guard lock(state.mtx);
        previous = std::move(state.observer);
        state.bound_backend.reset();
    }
    // Join the jthread outside the state lock to avoid blocking other
    // accessors behind a potentially slow thread shutdown.
    if (previous) previous->stop();
}

} // namespace cc::utils::pane_observer
