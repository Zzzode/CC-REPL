/// @file stats.cppm
/// @brief Stats store for metrics collection (counters, gauges, histograms, sets).
/// Migrated from: src/context/stats.tsx
module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module cc.context.stats;

export namespace cc::context::stats {

/// Maximum reservoir size for histogram sampling (Algorithm R)
inline constexpr std::size_t kReservoirSize = 1024;

/// Histogram accumulator using reservoir sampling
struct Histogram {
    std::vector<double> reservoir;
    int64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
};

/// Compute a percentile from a sorted array of values
[[nodiscard]] inline double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double index = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    auto lower = static_cast<std::size_t>(std::floor(index));
    auto upper = static_cast<std::size_t>(std::ceil(index));
    if (lower == upper) {
        return sorted[lower];
    }
    // Linear interpolation between adjacent values
    return sorted[lower] + (sorted[upper] - sorted[lower]) * (index - static_cast<double>(lower));
}

/// Thread-safe stats store supporting counters, gauges, histograms, and sets.
class StatsStore {
public:
    /// Increment a counter by the given value (default 1)
    void increment(const std::string& name, double value = 1.0) {
        std::lock_guard lock(mutex_);
        metrics_[name] += value;
    }

    /// Set a gauge to an absolute value
    void set(const std::string& name, double value) {
        std::lock_guard lock(mutex_);
        metrics_[name] = value;
    }

    /// Record an observation to a histogram (reservoir sampled)
    void observe(const std::string& name, double value) {
        std::lock_guard lock(mutex_);
        auto& h = histograms_[name];
        h.count++;
        h.sum += value;
        if (h.count == 1) {
            h.min = value;
            h.max = value;
        } else {
            if (value < h.min) h.min = value;
            if (value > h.max) h.max = value;
        }
        // Reservoir sampling (Algorithm R)
        if (h.reservoir.size() < kReservoirSize) {
            h.reservoir.push_back(value);
        } else {
            std::uniform_int_distribution<int64_t> dist(0, h.count - 1);
            auto j = dist(rng_);
            if (j < static_cast<int64_t>(kReservoirSize)) {
                h.reservoir[static_cast<std::size_t>(j)] = value;
            }
        }
    }

    /// Add a string value to a set (tracks unique values)
    void add(const std::string& name, const std::string& value) {
        std::lock_guard lock(mutex_);
        sets_[name].insert(value);
    }

    /// Get all metrics as a flat key-value map (histograms expand to _count/_min/_max/_avg/_p50/_p95/_p99)
    [[nodiscard]] std::unordered_map<std::string, double> get_all() const {
        std::lock_guard lock(mutex_);
        std::unordered_map<std::string, double> result(metrics_.begin(), metrics_.end());

        for (const auto& [name, h] : histograms_) {
            if (h.count == 0) continue;
            result[name + "_count"] = static_cast<double>(h.count);
            result[name + "_min"] = h.min;
            result[name + "_max"] = h.max;
            result[name + "_avg"] = h.sum / static_cast<double>(h.count);
            // Sort reservoir for percentile computation
            auto sorted = h.reservoir;
            std::ranges::sort(sorted);
            result[name + "_p50"] = percentile(sorted, 50.0);
            result[name + "_p95"] = percentile(sorted, 95.0);
            result[name + "_p99"] = percentile(sorted, 99.0);
        }

        for (const auto& [name, s] : sets_) {
            result[name] = static_cast<double>(s.size());
        }

        return result;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> metrics_;
    std::unordered_map<std::string, Histogram> histograms_;
    std::unordered_map<std::string, std::unordered_set<std::string>> sets_;
    mutable std::mt19937_64 rng_{std::random_device{}()};
};

/// Factory function to create a new StatsStore instance
[[nodiscard]] inline StatsStore create_stats_store() {
    return StatsStore{};
}

} // namespace cc::context::stats
