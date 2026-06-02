module;
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.billing;

export namespace cc::utils {

using TimePoint = std::chrono::system_clock::time_point;

struct UsageRecord {
    std::string model;
    int input_tokens;
    int output_tokens;
    double cost_usd;
    TimePoint timestamp;
};

namespace detail {
    inline std::mutex& billing_mutex() {
        static std::mutex m;
        return m;
    }

    inline std::vector<UsageRecord>& usage_records() {
        static std::vector<UsageRecord> records;
        return records;
    }

    // Pricing per 1M tokens (input/output)
    inline double get_input_cost_per_million(std::string_view model) {
        if (model.find("opus") != std::string_view::npos) return 15.0;
        if (model.find("sonnet") != std::string_view::npos) return 3.0;
        if (model.find("haiku") != std::string_view::npos) return 0.25;
        return 3.0; // Default to sonnet pricing
    }

    inline double get_output_cost_per_million(std::string_view model) {
        if (model.find("opus") != std::string_view::npos) return 75.0;
        if (model.find("sonnet") != std::string_view::npos) return 15.0;
        if (model.find("haiku") != std::string_view::npos) return 1.25;
        return 15.0;
    }
} // namespace detail

// Record a usage entry
void record_usage(UsageRecord record) {
    // Calculate cost if not provided
    if (record.cost_usd == 0.0) {
        double input_cost = static_cast<double>(record.input_tokens) *
                            detail::get_input_cost_per_million(record.model) / 1'000'000.0;
        double output_cost = static_cast<double>(record.output_tokens) *
                             detail::get_output_cost_per_million(record.model) / 1'000'000.0;
        record.cost_usd = input_cost + output_cost;
    }

    if (record.timestamp == TimePoint{}) {
        record.timestamp = std::chrono::system_clock::now();
    }

    std::lock_guard lock(detail::billing_mutex());
    detail::usage_records().push_back(std::move(record));
}

// Get total cost for current session
double get_session_cost() {
    std::lock_guard lock(detail::billing_mutex());
    double total = 0.0;
    for (auto& r : detail::usage_records()) {
        total += r.cost_usd;
    }
    return total;
}

// Get total cost for current month
double get_monthly_cost() {
    std::lock_guard lock(detail::billing_mutex());

    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm{};
    localtime_r(&now_t, &now_tm);

    // Start of current month
    std::tm month_start_tm = now_tm;
    month_start_tm.tm_mday = 1;
    month_start_tm.tm_hour = 0;
    month_start_tm.tm_min = 0;
    month_start_tm.tm_sec = 0;
    auto month_start = std::chrono::system_clock::from_time_t(std::mktime(&month_start_tm));

    double total = 0.0;
    for (auto& r : detail::usage_records()) {
        if (r.timestamp >= month_start) {
            total += r.cost_usd;
        }
    }
    return total;
}

// Format cost as human-readable string
std::string format_cost(double usd) {
    char buf[32];
    if (usd < 0.01) {
        std::snprintf(buf, sizeof(buf), "$%.4f", usd);
    } else if (usd < 1.0) {
        std::snprintf(buf, sizeof(buf), "$%.3f", usd);
    } else {
        std::snprintf(buf, sizeof(buf), "$%.2f", usd);
    }
    return buf;
}

} // namespace cc::utils
