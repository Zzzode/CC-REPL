module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

export module cc.utils.stats_utils;

export namespace cc::utils {

struct Stats {
    double min;
    double max;
    double mean;
    double median;
    double p95;
    double p99;
    std::size_t count;
};

// Compute statistics from a span of doubles
Stats compute_stats(std::span<double> data) {
    Stats stats{};
    stats.count = data.size();

    if (data.empty()) {
        stats.min = stats.max = stats.mean = stats.median = stats.p95 = stats.p99 = 0.0;
        return stats;
    }

    // Sort a copy for percentile calculations
    std::vector<double> sorted(data.begin(), data.end());
    std::sort(sorted.begin(), sorted.end());

    stats.min = sorted.front();
    stats.max = sorted.back();

    // Mean
    double sum = 0.0;
    for (double v : data) sum += v;
    stats.mean = sum / static_cast<double>(data.size());

    // Median
    std::size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        stats.median = (sorted[mid - 1] + sorted[mid]) / 2.0;
    } else {
        stats.median = sorted[mid];
    }

    // Percentiles (nearest-rank method)
    auto percentile = [&sorted](double p) -> double {
        if (sorted.size() == 1) return sorted[0];
        double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
        std::size_t lower = static_cast<std::size_t>(std::floor(rank));
        std::size_t upper = static_cast<std::size_t>(std::ceil(rank));
        if (lower == upper) return sorted[lower];
        double frac = rank - static_cast<double>(lower);
        return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
    };

    stats.p95 = percentile(95.0);
    stats.p99 = percentile(99.0);

    return stats;
}

// Format statistics as a human-readable string
std::string format_stats(const Stats& stats) {
    auto fmt = [](double v) -> std::string {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return buf;
    };

    std::string result;
    result += "count: " + std::to_string(stats.count) + "\n";
    result += "  min: " + fmt(stats.min) + "\n";
    result += "  max: " + fmt(stats.max) + "\n";
    result += " mean: " + fmt(stats.mean) + "\n";
    result += "  med: " + fmt(stats.median) + "\n";
    result += "  p95: " + fmt(stats.p95) + "\n";
    result += "  p99: " + fmt(stats.p99);
    return result;
}

} // namespace cc::utils
