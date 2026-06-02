/// @file diagnostic.cppm
/// @brief Diagnostic and performance monitoring service.
/// Tracks performance metrics, error frequency, latency percentiles,
/// memory usage, and generates diagnostic reports.
module;

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <chrono>
#include <format>
#include <ranges>
#include <algorithm>
#include <unordered_map>
#include <numeric>
#include <mutex>

export module cc.services.diagnostic;

import cc.types.types;

export namespace cc::services::diagnostic {

using cc::core::Error;
using cc::core::ErrorCode;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// ============================================================
// 诊断数据结构
// ============================================================

// 延迟百分位数据
struct LatencyPercentiles {
    Duration p50{0};
    Duration p90{0};
    Duration p95{0};
    Duration p99{0};
    Duration max{0};
};

// 错误频率记录
struct ErrorFrequency {
    std::string error_type;
    std::size_t count{0};
    TimePoint first_seen;
    TimePoint last_seen;
};

// 内存使用快照
struct MemorySnapshot {
    std::size_t resident_bytes{0};   // RSS
    std::size_t virtual_bytes{0};    // 虚拟内存
    std::size_t heap_used{0};
    TimePoint captured_at;
};

// 性能指标
struct PerformanceMetric {
    std::string name;
    Duration value{0};
    TimePoint recorded_at;
};

// 诊断报告
struct DiagnosticReport {
    TimePoint generated_at;
    Duration uptime{0};
    LatencyPercentiles api_latency;
    std::vector<ErrorFrequency> top_errors;
    MemorySnapshot memory;
    std::size_t total_requests{0};
    double error_rate{0.0};
    std::string summary;
};

// ============================================================
// DiagnosticService - 诊断与性能监控
// ============================================================

class DiagnosticService {
public:
    DiagnosticService() : start_time_(Clock::now()) {}

    // 记录延迟样本
    void record_latency(std::string_view operation, Duration latency) {
        std::lock_guard lock(mutex_);
        latency_samples_[std::string(operation)].push_back(latency);
        // 限制样本数量
        auto& samples = latency_samples_[std::string(operation)];
        if (samples.size() > max_samples_) {
            samples.erase(samples.begin(), samples.begin() + static_cast<long>(samples.size() - max_samples_));
        }
    }

    // 记录错误
    void record_error(std::string error_type) {
        std::lock_guard lock(mutex_);
        auto& freq = error_counts_[error_type];
        if (freq.count == 0) {
            freq.error_type = error_type;
            freq.first_seen = Clock::now();
        }
        freq.count++;
        freq.last_seen = Clock::now();
        total_errors_++;
    }

    // 记录请求
    void record_request() noexcept {
        std::lock_guard lock(mutex_);
        total_requests_++;
    }

    // 更新内存快照
    void update_memory(MemorySnapshot snapshot) {
        std::lock_guard lock(mutex_);
        latest_memory_ = snapshot;
        latest_memory_.captured_at = Clock::now();
    }

    // 计算延迟百分位
    [[nodiscard]] LatencyPercentiles compute_percentiles(std::string_view operation) const {
        std::lock_guard lock(mutex_);
        auto it = latency_samples_.find(std::string(operation));
        if (it == latency_samples_.end() || it->second.empty()) {
            return {};
        }
        auto samples = it->second;  // 复制用于排序
        std::ranges::sort(samples);
        return {
            .p50 = percentile(samples, 0.50),
            .p90 = percentile(samples, 0.90),
            .p95 = percentile(samples, 0.95),
            .p99 = percentile(samples, 0.99),
            .max = samples.back(),
        };
    }

    // 生成诊断报告
    [[nodiscard]] DiagnosticReport generate_report() const {
        std::lock_guard lock(mutex_);
        DiagnosticReport report;
        report.generated_at = Clock::now();
        report.uptime = std::chrono::duration_cast<Duration>(Clock::now() - start_time_);
        report.total_requests = total_requests_;
        report.error_rate = total_requests_ == 0 ? 0.0 :
            static_cast<double>(total_errors_) / static_cast<double>(total_requests_);
        report.memory = latest_memory_;

        // 收集 API 延迟
        if (latency_samples_.contains("api")) {
            auto samples = latency_samples_.at("api");
            std::ranges::sort(samples);
            report.api_latency = {
                .p50 = percentile(samples, 0.50),
                .p90 = percentile(samples, 0.90),
                .p95 = percentile(samples, 0.95),
                .p99 = percentile(samples, 0.99),
                .max = samples.empty() ? Duration{0} : samples.back(),
            };
        }

        // Top 错误
        std::vector<ErrorFrequency> errors;
        errors.reserve(error_counts_.size());
        for (const auto& [_, freq] : error_counts_) {
            errors.push_back(freq);
        }
        std::ranges::sort(errors, [](const auto& a, const auto& b) {
            return a.count > b.count;
        });
        if (errors.size() > 10) errors.resize(10);
        report.top_errors = std::move(errors);

        // 生成摘要文本
        report.summary = std::format(
            "Uptime: {}ms | Requests: {} | Error rate: {:.2f}% | Memory: {} bytes",
            report.uptime.count(), report.total_requests,
            report.error_rate * 100.0, report.memory.resident_bytes);
        return report;
    }

    // 重置所有指标
    void reset() noexcept {
        std::lock_guard lock(mutex_);
        latency_samples_.clear();
        error_counts_.clear();
        total_requests_ = 0;
        total_errors_ = 0;
        start_time_ = Clock::now();
    }

    // 设置最大样本数
    void set_max_samples(std::size_t n) noexcept { max_samples_ = n; }

private:
    mutable std::mutex mutex_;
    TimePoint start_time_;
    std::unordered_map<std::string, std::vector<Duration>> latency_samples_;
    std::unordered_map<std::string, ErrorFrequency> error_counts_;
    MemorySnapshot latest_memory_;
    std::size_t total_requests_{0};
    std::size_t total_errors_{0};
    std::size_t max_samples_{10000};

    // 计算指定百分位值
    static Duration percentile(const std::vector<Duration>& sorted_samples, double p) {
        if (sorted_samples.empty()) return Duration{0};
        auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted_samples.size() - 1));
        return sorted_samples[idx];
    }
};

} // namespace cc::services::diagnostic
