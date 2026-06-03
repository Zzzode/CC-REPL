/// @file tool_summary.cppm
/// @brief Tool use summary service.
/// Generates summaries of tool invocations, aggregates statistics including
/// most used tools, failure rates, session-level and all-time stats.
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

export module cc.services.tool_summary;

import cc.types.types;

export namespace cc::services::tool_summary {

using cc::core::Error;
using cc::core::ErrorCode;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// ============================================================

// ============================================================


struct ToolUseRecord {
    std::string tool_name;
    std::string session_id;
    Duration elapsed{0};
    bool success{true};
    std::string error_type;
    TimePoint invoked_at;
};


struct ToolStats {
    std::string tool_name;
    std::size_t total_calls{0};
    std::size_t success_count{0};
    std::size_t failure_count{0};
    Duration total_time{0};
    Duration min_time{Duration::max()};
    Duration max_time{0};

    [[nodiscard]] double failure_rate() const noexcept {
        return total_calls == 0 ? 0.0 : static_cast<double>(failure_count) / static_cast<double>(total_calls);
    }

    [[nodiscard]] Duration avg_time() const noexcept {
        return total_calls == 0 ? Duration{0} : Duration{total_time.count() / static_cast<long long>(total_calls)};
    }
};


struct SessionSummary {
    std::string session_id;
    std::size_t total_calls{0};
    std::size_t unique_tools{0};
    double overall_success_rate{1.0};
    std::string most_used_tool;
    Duration total_execution_time{0};
};


struct GlobalSummary {
    std::size_t total_sessions{0};
    std::size_t total_calls{0};
    double overall_success_rate{1.0};
    std::vector<ToolStats> top_tools;
    std::vector<ToolStats> slowest_tools;
};

// ============================================================

// ============================================================

class ToolUseSummaryService {
public:
    ToolUseSummaryService() = default;


    void record(ToolUseRecord record) {
        auto& stats = tool_stats_[record.tool_name];
        stats.tool_name = record.tool_name;
        stats.total_calls++;
        if (record.success) { stats.success_count++; }
        else { stats.failure_count++; }
        stats.total_time += record.elapsed;
        stats.min_time = std::min(stats.min_time, record.elapsed);
        stats.max_time = std::max(stats.max_time, record.elapsed);

        session_records_[record.session_id].push_back(std::move(record));
    }


    [[nodiscard]] std::optional<ToolStats> get_tool_stats(std::string_view tool_name) const {
        auto it = tool_stats_.find(std::string(tool_name));
        if (it == tool_stats_.end()) return std::nullopt;
        return it->second;
    }


    [[nodiscard]] std::optional<SessionSummary> get_session_summary(std::string_view session_id) const {
        auto it = session_records_.find(std::string(session_id));
        if (it == session_records_.end()) return std::nullopt;

        const auto& records = it->second;
        SessionSummary summary;
        summary.session_id = std::string(session_id);
        summary.total_calls = records.size();

        std::unordered_map<std::string, std::size_t> tool_counts;
        std::size_t successes = 0;
        for (const auto& r : records) {
            tool_counts[r.tool_name]++;
            if (r.success) ++successes;
            summary.total_execution_time += r.elapsed;
        }
        summary.unique_tools = tool_counts.size();
        summary.overall_success_rate = records.empty() ? 1.0 :
            static_cast<double>(successes) / static_cast<double>(records.size());

        if (!tool_counts.empty()) {
            auto max_it = std::ranges::max_element(tool_counts,
                [](const auto& a, const auto& b) { return a.second < b.second; });
            summary.most_used_tool = max_it->first;
        }
        return summary;
    }


    [[nodiscard]] GlobalSummary get_global_summary(std::size_t top_n = 5) const {
        GlobalSummary summary;
        summary.total_sessions = session_records_.size();

        std::vector<ToolStats> all_stats;
        all_stats.reserve(tool_stats_.size());
        for (const auto& [_, stats] : tool_stats_) {
            summary.total_calls += stats.total_calls;
            all_stats.push_back(stats);
        }

        std::ranges::sort(all_stats, [](const auto& a, const auto& b) {
            return a.total_calls > b.total_calls;
        });
        summary.top_tools.assign(
            all_stats.begin(),
            all_stats.begin() + std::min(top_n, all_stats.size()));

        std::ranges::sort(all_stats, [](const auto& a, const auto& b) {
            return a.avg_time() > b.avg_time();
        });
        summary.slowest_tools.assign(
            all_stats.begin(),
            all_stats.begin() + std::min(top_n, all_stats.size()));

        std::size_t total_success = 0;
        for (const auto& [_, s] : tool_stats_) total_success += s.success_count;
        summary.overall_success_rate = summary.total_calls == 0 ? 1.0 :
            static_cast<double>(total_success) / static_cast<double>(summary.total_calls);
        return summary;
    }


    void reset() noexcept {
        tool_stats_.clear();
        session_records_.clear();
    }

private:
    std::unordered_map<std::string, ToolStats> tool_stats_;
    std::unordered_map<std::string, std::vector<ToolUseRecord>> session_records_;
};

} // namespace cc::services::tool_summary
