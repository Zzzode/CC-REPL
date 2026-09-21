/// @file timestamp.cppm
/// @brief Protobuf Timestamp type representation.
/// Migrated from: src/types/generated/google/protobuf/timestamp.ts
module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

export module cc.types.timestamp;

export namespace cc::types::timestamp {

/// A Timestamp represents a point in time independent of any time zone,
/// encoded as seconds and nanoseconds since Unix epoch (1970-01-01T00:00:00Z).
struct Timestamp {
    /// Seconds of UTC time since Unix epoch
    std::optional<int64_t> seconds;
    /// Non-negative fractions of a second at nanosecond resolution (0..999999999)
    std::optional<int32_t> nanos;
};

/// Convert a Timestamp to a std::chrono::system_clock::time_point
[[nodiscard]] inline std::chrono::system_clock::time_point
to_time_point(const Timestamp& ts) {
    using namespace std::chrono;
    auto secs = seconds(ts.seconds.value_or(0));
    auto nanos_dur = nanoseconds(ts.nanos.value_or(0));
    return system_clock::time_point(duration_cast<system_clock::duration>(secs + nanos_dur));
}

/// Create a Timestamp from a std::chrono::system_clock::time_point
[[nodiscard]] inline Timestamp
from_time_point(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto since_epoch = tp.time_since_epoch();
    auto secs = duration_cast<seconds>(since_epoch);
    auto nanos_part = duration_cast<nanoseconds>(since_epoch - secs);
    return Timestamp{
        .seconds = secs.count(),
        .nanos = static_cast<int32_t>(nanos_part.count()),
    };
}

/// Create a default Timestamp (epoch)
[[nodiscard]] inline Timestamp create_default() {
    return Timestamp{.seconds = 0, .nanos = 0};
}

} // namespace cc::types::timestamp
