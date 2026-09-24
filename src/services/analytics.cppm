/// @file analytics.cppm
/// @brief Local-only event log (NDJSON), with no network path.
///
/// This is the local telemetry the decoupling plan chose to KEEP: one JSON
/// object per line appended under the XDG state directory, so a user can
/// inspect what the tool recorded without anything leaving the machine.
/// There is no sink other than the file -- no HTTP client is imported here,
/// and adding one would be a deliberate, separate change rather than a
/// configuration flag.
///
/// The previous module of this name was a dead stub with zero importers; this
/// is a real writer, so it is wired into the engine rather than left to be
/// discovered. A writer nobody calls is how the last one ended up dead.
///
/// ## Why append-only and why resilient
/// Telemetry is diagnostic, never load-bearing. If the file cannot be opened,
/// the event is dropped and logging does not fail: an unwritable state
/// directory must not break a session. Flush happens per event rather than on
/// a buffer threshold, because a crash is exactly the case where the log is
/// worth having.
module;

#include <cstdlib>
#include <cstdint>
export module cc.services.analytics;

import std;

import cc.utils.json;
import cc.utils.xdg;

export namespace cc::services::analytics {

/// One recorded event.
struct AnalyticsEvent {
    std::string name;
    std::chrono::system_clock::time_point timestamp;
    std::vector<std::pair<std::string, std::string>> properties;
};

/// Resolve the NDJSON log path. $LOOM_ANALYTICS_PATH overrides outright
/// (tests use this rather than writing to the real state dir); otherwise
/// `<XDG_STATE_HOME>/loom/analytics.ndjson`.
[[nodiscard]] inline std::filesystem::path analytics_log_path() {
    if (const char* env = std::getenv("LOOM_ANALYTICS_PATH"); env && *env) {
        return std::filesystem::path{env};
    }
    return cc::utils::xdg_state_home() / "loom" / "analytics.ndjson";
}

/// Whether local analytics are enabled. Off when LOOM_ANALYTICS_DISABLED is
/// set to a truthy value. Note this is a LOCAL on/off switch, not an opt-out
/// from network reporting -- there is no network reporting to opt out of.
[[nodiscard]] inline bool analytics_enabled() {
    const char* disabled = std::getenv("LOOM_ANALYTICS_DISABLED");
    if (!disabled) return true;
    const std::string_view v{disabled};
    return !(v == "1" || v == "true" || v == "TRUE" || v == "yes");
}

/// Append-only local event log. Thread-safe: the engine can emit from
/// several places, and interleaved partial lines would corrupt the file.
class LocalAnalytics {
public:
    LocalAnalytics() = default;
    explicit LocalAnalytics(std::filesystem::path path)
        : path_override_(std::move(path)) {}

    /// Record one event. Never throws and never fails a session: an
    /// unwritable log is reported by returning false, which callers may
    /// ignore.
    bool log_event(std::string_view name,
                   std::vector<std::pair<std::string, std::string>> properties = {}) {
        if (!analytics_enabled()) return false;

        const auto now = std::chrono::system_clock::now();
        auto line = serialize_event(name, now, properties);

        std::lock_guard lock(mutex_);
        const auto path = path_override_ ? *path_override_ : analytics_log_path();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::app);
        if (!out) return false;
        out << line << '\n';
        // Flush per event: a crash is the case where the log matters most.
        out.flush();
        return static_cast<bool>(out);
    }

    /// The path this instance writes to (resolved lazily for the default).
    [[nodiscard]] std::filesystem::path path() const {
        return path_override_ ? *path_override_ : analytics_log_path();
    }

private:
    /// Build the NDJSON line. Goes through the JSON builder rather than
    /// string concatenation so escaping is the library's problem, not ours.
    [[nodiscard]] static std::string serialize_event(
        std::string_view name,
        std::chrono::system_clock::time_point timestamp,
        const std::vector<std::pair<std::string, std::string>>& properties) {
        cc::utils::json::JsonBuilder builder;
        const auto seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 timestamp.time_since_epoch())
                                 .count();
        builder.str("event", name);
        builder.num("ts_ms", static_cast<int64_t>(seconds));

        auto props = builder.doc().object();
        for (const auto& [key, value] : properties) {
            props.add(key, builder.doc().string(value));
        }
        builder.root().add("properties", props);
        return builder.serialize();
    }

    std::optional<std::filesystem::path> path_override_;
    mutable std::mutex mutex_;
};

/// Process-wide instance, so callers need not thread one through.
[[nodiscard]] inline LocalAnalytics& local_analytics() {
    static LocalAnalytics instance;
    return instance;
}

}  // namespace cc::services::analytics
