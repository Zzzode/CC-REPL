/// @file worker_registry.cppm
/// @brief Daemon worker registry — tracks available workers (in-process,
///        subprocess, gRPC, bridge), their health, capacity and capabilities.
///
/// The registry is thread-safe (shared_mutex) and exposes:
///   * register / unregister / heartbeat / cordon mutations
///   * lookup, find_matching, pick_best queries
///   * TTL-based expiry of stale entries (heartbeat timestamp older than ttl)
///   * JSON ser/de for persistence / cross-process handoff
///   * singleton instance() for the running daemon

module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.daemon.worker_registry;

import cc.utils.json;

export namespace cc::daemon {

using namespace std::chrono_literals;

// ─── Enums ──────────────────────────────────────────────────────────────────

/// How a worker is hosted / reached from the daemon orchestrator.
enum class WorkerKind : uint8_t {
    /// Running inside the daemon process itself as a dedicated task queue.
    InProcess,
    /// A local subprocess the daemon spawned (stdout/stderr/ipc pairs).
    Subprocess,
    /// Reached over gRPC to a remote agent binary.
    RemoteGrpc,
    /// Reached over the IDE bridge (JWT-authenticated bidirectional channel).
    RemoteBridge,
};

/// Liveness view of a worker from the daemon's perspective.
enum class WorkerHealth : uint8_t {
    /// No heartbeat yet or unknown.
    Unknown,
    /// Heartbeats within TTL, error rate nominal, CPU/memory within limits.
    Healthy,
    /// Degraded: elevated latency or transient errors but still usable.
    Degraded,
    /// Timed out, repeatedly erroring, or administratively cordoned.
    Unhealthy,
};

// ─── Data structures ────────────────────────────────────────────────────────

struct WorkerInfo {
    using Id = std::string;

    Id id;
    WorkerKind kind = WorkerKind::InProcess;
    std::string hostname;
    int port = 0;
    /// Capability tags (e.g. "query", "bash", "mcp", "file") used for routing.
    std::vector<std::string> capabilities;
    int max_concurrent_tasks = 1;
    int current_tasks = 0;
    /// Monotonic wall-clock timestamps stored as epoch ms (from std::chrono::system_clock).
    int64_t registered_at_ms = 0;
    int64_t last_heartbeat_ms = 0;
    int64_t started_at_ms = 0;
    /// Observability fields reported by the worker.
    double cpu_usage_pct = 0.0;
    uint64_t memory_used_bytes = 0;
    uint64_t memory_limit_bytes = 0;
    /// Version string for compatibility checks (semver-like, not validated).
    std::string version;
    /// If true, the worker will not receive NEW assignments (drain behaviour).
    bool cordoned = false;
    WorkerHealth health = WorkerHealth::Unknown;
};

/// Filters used for find_matching / pick_best.  All fields are ANDed together.
struct WorkerQueryFilters {
    /// Restrict to a specific hosting kind.  nullopt = any.
    std::optional<WorkerKind> kind;
    /// Required capability tag (empty string = no filter).
    std::string capability_required;
    /// If false, cordoned workers are filtered out (default).
    bool include_cordoned = false;
    /// If true, only WorkerHealth::Healthy is accepted (default).
    bool only_healthy = true;
    /// Require at least this many free slots (max - current).  0 disables.
    size_t min_free_tasks = 1;
    /// Heartbeat must be within this many milliseconds of "now".  0 disables.
    int64_t require_heartbeat_within_ms = 30000;
};

// ─── WorkerRegistry ─────────────────────────────────────────────────────────

class WorkerRegistry {
public:
    /// Hard upper bound on number of simultaneous workers (anti-overcommit guard).
    static constexpr size_t kMaxWorkers = 1024;
    /// Default TTL used by expire_stale() when the caller does not specify one.
    static constexpr std::chrono::seconds kDefaultTTL{30};

    /// Singleton accessor (used by the running daemon).
    static WorkerRegistry& instance() {
        static WorkerRegistry inst;
        return inst;
    }

    WorkerRegistry();
    ~WorkerRegistry();
    WorkerRegistry(const WorkerRegistry&) = delete;
    WorkerRegistry& operator=(const WorkerRegistry&) = delete;
    WorkerRegistry(WorkerRegistry&&) noexcept = delete;
    WorkerRegistry& operator=(WorkerRegistry&&) noexcept = delete;

    /// Register a new worker.  Returns the canonical id on success.  The input
    /// WorkerInfo's id may be empty (then one is generated) or pre-filled.
    /// Errors:
    ///   * "capacity_exceeded" — kMaxWorkers reached
    ///   * "duplicate_id"      — a worker with this id already exists
    ///   * "invalid"           — required fields missing
    auto register_worker(WorkerInfo info) -> std::expected<std::string, std::string>;

    /// Remove a worker from the registry.  Returns true if one was actually removed.
    auto unregister_worker(std::string_view id) -> bool;

    /// Update observability fields on an existing worker.  Omitted optionals keep
    /// their current values.  The last_heartbeat_ms field is always advanced to "now"
    /// unless the caller explicitly passed an unhealthy health (which the registry
    /// preserves as-is for diagnostic purposes).
    auto heartbeat(std::string_view id,
                   std::optional<double> cpu = std::nullopt,
                   std::optional<uint64_t> mem = std::nullopt,
                   std::optional<int> cur_tasks = std::nullopt,
                   std::optional<WorkerHealth> health = std::nullopt)
        -> std::expected<void, std::string>;

    /// Look up a single worker by id.  nullopt if absent.
    auto lookup(std::string_view id) -> std::optional<WorkerInfo>;

    /// Return every worker matching the filters, insertion order.
    auto find_matching(const WorkerQueryFilters& filters) const -> std::vector<WorkerInfo>;

    /// Pick the "best" matching worker using the following sort order:
    ///   1. Lowest current_tasks / max_concurrent_tasks load ratio
    ///   2. Most free memory (memory_limit_bytes - memory_used_bytes) descending
    ///   3. Earliest registration (stable tie-breaker)
    /// Returns nullopt if no worker satisfies the filters.
    auto pick_best(const WorkerQueryFilters& filters) const -> std::optional<WorkerInfo>;

    /// Remove every worker whose last_heartbeat_ms is older than (now - ttl).
    /// Returns the number of workers that were expired.  Provided as a template so
    /// callers can pass any chrono duration; internally the ttl is converted to
    /// milliseconds against std::chrono::system_clock.
    template <class Rep, class Period>
    auto expire_stale(std::chrono::duration<Rep, Period> ttl = kDefaultTTL) -> size_t {
        using namespace std::chrono;
        const auto ttl_ms = duration_cast<milliseconds>(ttl).count();
        const auto now_ms = duration_cast<milliseconds>(
                                system_clock::now().time_since_epoch())
                                .count();
        const int64_t cutoff = now_ms - static_cast<int64_t>(ttl_ms);
        return expire_older_than(cutoff);
    }

    /// Mark a worker cordoned (= no new assignments, drain existing tasks).
    auto set_cordon(std::string_view id, bool v) -> bool;

    /// Snapshot of all workers (useful for logging / status endpoints).
    auto snapshot() const -> std::vector<WorkerInfo>;

    /// Aggregate stats used for the /status endpoint and log gauges.
    struct Stats {
        size_t total = 0;
        size_t healthy = 0;
        size_t cordoned = 0;
        size_t expired = 0;
        /// Sum of max_concurrent_tasks across non-expired workers.
        size_t capacity_total = 0;
        /// Sum of current_tasks across non-expired workers.
        size_t capacity_used = 0;
    };
    auto stats() const -> Stats;

    // ─── JSON ser/de ───────────────────────────────────────────────────────────
    static auto to_json(const std::vector<WorkerInfo>&) -> std::string;
    static auto from_json(std::string_view) -> std::expected<std::vector<WorkerInfo>, std::string>;

    /// Clear all workers (mainly used by tests).
    void clear();

private:
    /// Internal helper that expires workers with last_heartbeat_ms <= cutoff_ms.
    auto expire_older_than(int64_t cutoff_ms) -> size_t;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── pImpl ───────────────────────────────────────────────────────────────────

struct WorkerRegistry::Impl {
    mutable std::shared_mutex mu;
    std::unordered_map<std::string, WorkerInfo> by_id;
    // Insertion order preserved via an auxiliary vector so pick_best/snapshot stay
    // deterministic across runs (unordered_map iteration order varies otherwise).
    std::vector<std::string> insertion_order;
};

inline WorkerRegistry::WorkerRegistry() : impl_(std::make_unique<Impl>()) {}
inline WorkerRegistry::~WorkerRegistry() = default;

namespace worker_detail {

auto now_ms() -> int64_t {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

auto gen_id() -> std::string {
    std::ostringstream ss;
    ss << "w-" << std::hex << now_ms() << "-"
       << (std::rand() & 0xFFFFu);  // NOLINT(cert-msc30-c,cert-msc50-cpp) — non-crypto use
    return ss.str();
}

}  // namespace worker_detail
using namespace worker_detail;

// ─── Mutations ───────────────────────────────────────────────────────────────

inline auto WorkerRegistry::register_worker(WorkerInfo info)
    -> std::expected<std::string, std::string> {
    std::unique_lock lock{impl_->mu};
    if (impl_->by_id.size() >= kMaxWorkers) {
        return std::unexpected<std::string>{"capacity_exceeded"};
    }
    if (info.id.empty()) {
        info.id = gen_id();
    } else if (impl_->by_id.count(info.id)) {
        return std::unexpected<std::string>{"duplicate_id"};
    }
    if (info.max_concurrent_tasks <= 0) info.max_concurrent_tasks = 1;
    if (info.current_tasks < 0) info.current_tasks = 0;
    if (info.current_tasks > info.max_concurrent_tasks) {
        info.current_tasks = info.max_concurrent_tasks;
    }
    if (info.registered_at_ms == 0) info.registered_at_ms = now_ms();
    if (info.last_heartbeat_ms == 0) info.last_heartbeat_ms = info.registered_at_ms;
    if (info.health == WorkerHealth::Unknown) info.health = WorkerHealth::Healthy;
    const std::string saved_id = info.id;
    impl_->by_id.emplace(saved_id, std::move(info));
    impl_->insertion_order.push_back(saved_id);
    return saved_id;
}

inline auto WorkerRegistry::unregister_worker(std::string_view id) -> bool {
    std::unique_lock lock{impl_->mu};
    auto it = impl_->by_id.find(std::string{id});
    if (it == impl_->by_id.end()) return false;
    impl_->by_id.erase(it);
    std::erase(impl_->insertion_order, std::string{id});
    return true;
}

inline auto WorkerRegistry::heartbeat(std::string_view id,
                                      std::optional<double> cpu,
                                      std::optional<uint64_t> mem,
                                      std::optional<int> cur_tasks,
                                      std::optional<WorkerHealth> health)
    -> std::expected<void, std::string> {
    std::unique_lock lock{impl_->mu};
    auto it = impl_->by_id.find(std::string{id});
    if (it == impl_->by_id.end()) {
        return std::unexpected<std::string>{"not_found"};
    }
    WorkerInfo& w = it->second;
    if (cpu.has_value()) w.cpu_usage_pct = *cpu;
    if (mem.has_value()) w.memory_used_bytes = *mem;
    if (cur_tasks.has_value()) {
        w.current_tasks = std::clamp(*cur_tasks, 0, w.max_concurrent_tasks);
    }
    if (health.has_value()) w.health = *health;
    // Heartbeat only advances liveness if the caller did not explicitly report
    // Unhealthy (in that case we keep the old heartbeat so TTL expiry agrees with
    // the explicit failure report).
    if (!health.has_value() || *health != WorkerHealth::Unhealthy) {
        w.last_heartbeat_ms = now_ms();
    }
    return {};
}

inline auto WorkerRegistry::lookup(std::string_view id) -> std::optional<WorkerInfo> {
    std::shared_lock lock{impl_->mu};
    auto it = impl_->by_id.find(std::string{id});
    if (it == impl_->by_id.end()) return std::nullopt;
    return it->second;
}

// ─── Queries ─────────────────────────────────────────────────────────────────

namespace worker_detail {

auto satisfies(const WorkerInfo& w, const WorkerQueryFilters& f, int64_t now_ms) -> bool {
    if (!f.include_cordoned && w.cordoned) return false;
    if (f.only_healthy && w.health != WorkerHealth::Healthy) return false;
    if (f.kind.has_value() && w.kind != *f.kind) return false;
    if (!f.capability_required.empty()) {
        const auto& tag = f.capability_required;
        if (std::find(w.capabilities.begin(), w.capabilities.end(), tag)
            == w.capabilities.end()) {
            return false;
        }
    }
    const int free = std::max(0, w.max_concurrent_tasks - w.current_tasks);
    if (f.min_free_tasks != 0 && static_cast<size_t>(free) < f.min_free_tasks) return false;
    if (f.require_heartbeat_within_ms > 0) {
        const int64_t age = now_ms - w.last_heartbeat_ms;
        if (age > f.require_heartbeat_within_ms) return false;
    }
    return true;
}

double load_ratio(const WorkerInfo& w) noexcept {
    const double max = static_cast<double>(std::max(1, w.max_concurrent_tasks));
    const double cur = static_cast<double>(std::max(0, w.current_tasks));
    return cur / max;
}

}  // namespace worker_detail
using namespace worker_detail;

inline auto WorkerRegistry::find_matching(const WorkerQueryFilters& filters) const
    -> std::vector<WorkerInfo> {
    std::shared_lock lock{impl_->mu};
    const int64_t t = now_ms();
    std::vector<WorkerInfo> out;
    out.reserve(impl_->insertion_order.size());
    for (const auto& id : impl_->insertion_order) {
        const WorkerInfo& w = impl_->by_id.at(id);
        if (satisfies(w, filters, t)) out.push_back(w);
    }
    return out;
}

inline auto WorkerRegistry::pick_best(const WorkerQueryFilters& filters) const
    -> std::optional<WorkerInfo> {
    std::shared_lock lock{impl_->mu};
    const int64_t t = now_ms();
    const WorkerInfo* best = nullptr;
    int best_insertion = -1;
    for (size_t i = 0; i < impl_->insertion_order.size(); ++i) {
        const WorkerInfo& w = impl_->by_id.at(impl_->insertion_order[i]);
        if (!satisfies(w, filters, t)) continue;
        if (!best) {
            best = &w;
            best_insertion = static_cast<int>(i);
            continue;
        }
        const double cur_ratio = load_ratio(w);
        const double best_ratio = load_ratio(*best);
        if (cur_ratio < best_ratio) { best = &w; best_insertion = static_cast<int>(i); continue; }
        if (cur_ratio > best_ratio) continue;
        // Equal load ratio — prefer more free memory.
        const uint64_t cur_free  = w.memory_limit_bytes > w.memory_used_bytes
                                       ? w.memory_limit_bytes - w.memory_used_bytes
                                       : 0;
        const uint64_t best_free = best->memory_limit_bytes > best->memory_used_bytes
                                       ? best->memory_limit_bytes - best->memory_used_bytes
                                       : 0;
        if (cur_free > best_free) { best = &w; best_insertion = static_cast<int>(i); continue; }
        if (cur_free == best_free && static_cast<int>(i) < best_insertion) {
            best = &w; best_insertion = static_cast<int>(i);
        }
    }
    if (!best) return std::nullopt;
    return *best;
}

inline auto WorkerRegistry::expire_older_than(int64_t cutoff_ms) -> size_t {
    std::unique_lock lock{impl_->mu};
    size_t removed = 0;
    auto new_order = std::vector<std::string>{};
    new_order.reserve(impl_->insertion_order.size());
    for (const auto& id : impl_->insertion_order) {
        auto it = impl_->by_id.find(id);
        if (it == impl_->by_id.end()) continue;
        if (it->second.last_heartbeat_ms <= cutoff_ms) {
            impl_->by_id.erase(it);
            ++removed;
        } else {
            new_order.push_back(id);
        }
    }
    impl_->insertion_order = std::move(new_order);
    return removed;
}

inline auto WorkerRegistry::set_cordon(std::string_view id, bool v) -> bool {
    std::unique_lock lock{impl_->mu};
    auto it = impl_->by_id.find(std::string{id});
    if (it == impl_->by_id.end()) return false;
    it->second.cordoned = v;
    return true;
}

inline auto WorkerRegistry::snapshot() const -> std::vector<WorkerInfo> {
    std::shared_lock lock{impl_->mu};
    std::vector<WorkerInfo> out;
    out.reserve(impl_->insertion_order.size());
    for (const auto& id : impl_->insertion_order) {
        out.push_back(impl_->by_id.at(id));
    }
    return out;
}

inline auto WorkerRegistry::stats() const -> Stats {
    std::shared_lock lock{impl_->mu};
    Stats s{};
    const int64_t ttl_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultTTL).count());
    const int64_t t = now_ms();
    s.total = impl_->insertion_order.size();
    for (const auto& id : impl_->insertion_order) {
        const WorkerInfo& w = impl_->by_id.at(id);
        if (w.health == WorkerHealth::Healthy) s.healthy++;
        if (w.cordoned) s.cordoned++;
        const int64_t age = t - w.last_heartbeat_ms;
        if (age > ttl_ms) s.expired++;
        s.capacity_total += static_cast<size_t>(w.max_concurrent_tasks);
        s.capacity_used  += static_cast<size_t>(std::max(0, w.current_tasks));
    }
    return s;
}

inline void WorkerRegistry::clear() {
    std::unique_lock lock{impl_->mu};
    impl_->by_id.clear();
    impl_->insertion_order.clear();
}

// ─── JSON ser/de ─────────────────────────────────────────────────────────────

namespace worker_detail {

auto kind_to_str(WorkerKind k) -> std::string_view {
    switch (k) {
        case WorkerKind::InProcess:    return "in_process";
        case WorkerKind::Subprocess:   return "subprocess";
        case WorkerKind::RemoteGrpc:   return "remote_grpc";
        case WorkerKind::RemoteBridge: return "remote_bridge";
    }
    return "unknown";
}
auto kind_from_str(std::string_view s) -> std::optional<WorkerKind> {
    if (s == "in_process")    return WorkerKind::InProcess;
    if (s == "subprocess")   return WorkerKind::Subprocess;
    if (s == "remote_grpc")  return WorkerKind::RemoteGrpc;
    if (s == "remote_bridge") return WorkerKind::RemoteBridge;
    return std::nullopt;
}
auto health_to_str(WorkerHealth h) -> std::string_view {
    switch (h) {
        case WorkerHealth::Unknown:   return "unknown";
        case WorkerHealth::Healthy:   return "healthy";
        case WorkerHealth::Degraded:  return "degraded";
        case WorkerHealth::Unhealthy: return "unhealthy";
    }
    return "unknown";
}
auto health_from_str(std::string_view s) -> std::optional<WorkerHealth> {
    if (s == "unknown")   return WorkerHealth::Unknown;
    if (s == "healthy")   return WorkerHealth::Healthy;
    if (s == "degraded")  return WorkerHealth::Degraded;
    if (s == "unhealthy") return WorkerHealth::Unhealthy;
    return std::nullopt;
}

auto worker_info_to_json(const WorkerInfo& w, cc::utils::json::JsonMutDoc& doc) {
    using namespace cc::utils::json;
    auto obj = doc.object();
    obj.add("id", doc.string(w.id));
    obj.add("kind", doc.string(std::string{kind_to_str(w.kind)}));
    obj.add("hostname", doc.string(w.hostname));
    obj.add("port", doc.number(static_cast<int64_t>(w.port)));
    auto caps = doc.array();
    for (const auto& c : w.capabilities) caps.append(doc.string(c));
    obj.add("capabilities", caps);
    obj.add("max_concurrent_tasks", doc.number(static_cast<int64_t>(w.max_concurrent_tasks)));
    obj.add("current_tasks", doc.number(static_cast<int64_t>(w.current_tasks)));
    obj.add("registered_at_ms", doc.number(static_cast<int64_t>(w.registered_at_ms)));
    obj.add("last_heartbeat_ms", doc.number(static_cast<int64_t>(w.last_heartbeat_ms)));
    obj.add("started_at_ms", doc.number(static_cast<int64_t>(w.started_at_ms)));
    obj.add("cpu_usage_pct", doc.number(static_cast<double>(w.cpu_usage_pct)));
    obj.add("memory_used_bytes", doc.number(static_cast<int64_t>(w.memory_used_bytes)));
    obj.add("memory_limit_bytes", doc.number(static_cast<int64_t>(w.memory_limit_bytes)));
    obj.add("version", doc.string(w.version));
    obj.add("cordoned", doc.boolean(w.cordoned));
    obj.add("health", doc.string(std::string{health_to_str(w.health)}));
    return obj;
}

} // namespace worker_detail
using namespace worker_detail;  // namespace

inline auto WorkerRegistry::to_json(const std::vector<WorkerInfo>& workers) -> std::string {
    using namespace cc::utils::json;
    JsonMutDoc doc;
    auto arr = doc.array();
    for (const auto& w : workers) {
        arr.append(worker_info_to_json(w, doc));
    }
    doc.set_root(arr);
    return doc.to_string();
}

inline auto WorkerRegistry::from_json(std::string_view raw)
    -> std::expected<std::vector<WorkerInfo>, std::string> {
    using namespace cc::utils::json;
    auto parsed = parse(raw);
    if (!parsed) return std::unexpected<std::string>(parsed.error().message());
    JsonVal root = parsed->root();
    if (!root.is_arr()) {
        return std::unexpected<std::string>{std::string{"expected array"}};
    }
    std::vector<WorkerInfo> out;
    out.reserve(root.size());
    bool ok = true;
    std::string err;
    root.iter([&](JsonVal el) {
        if (!ok || !el.is_obj()) { ok = false; err = "element not object"; return; }
        WorkerInfo w;
        w.id = std::string{el.get("id").as_str()};
        auto k = kind_from_str(el.get("kind").as_str());
        if (!k) { ok = false; err = "bad kind"; return; }
        w.kind = *k;
        w.hostname = std::string{el.get("hostname").as_str()};
        w.port = static_cast<int>(el.get("port").as_int());
        JsonVal caps = el.get("capabilities");
        if (caps.is_arr()) {
            caps.iter([&](JsonVal v) {
                w.capabilities.emplace_back(v.as_str());
            });
        }
        w.max_concurrent_tasks = static_cast<int>(el.get("max_concurrent_tasks").as_int());
        w.current_tasks = static_cast<int>(el.get("current_tasks").as_int());
        w.registered_at_ms = el.get("registered_at_ms").as_int();
        w.last_heartbeat_ms = el.get("last_heartbeat_ms").as_int();
        w.started_at_ms = el.get("started_at_ms").as_int();
        w.cpu_usage_pct = el.get("cpu_usage_pct").as_double();
        w.memory_used_bytes  = static_cast<uint64_t>(el.get("memory_used_bytes").as_int());
        w.memory_limit_bytes = static_cast<uint64_t>(el.get("memory_limit_bytes").as_int());
        w.version = std::string{el.get("version").as_str()};
        w.cordoned = el.get("cordoned").as_bool();
        auto h = health_from_str(el.get("health").as_str());
        if (h) w.health = *h;
        if (w.id.empty()) { ok = false; err = "empty id"; return; }
        if (w.max_concurrent_tasks <= 0) w.max_concurrent_tasks = 1;
        out.push_back(std::move(w));
    });
    if (!ok) return std::unexpected(err);
    return out;
}

}  // namespace cc::daemon
