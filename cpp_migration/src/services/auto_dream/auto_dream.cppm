/// @file auto_dream.cppm
/// @brief Auto-dream (background memory consolidation) service.
/// Runs during idle time to extract patterns from recent sessions,
/// consolidate related memories, prune redundant entries, and schedule
/// periodic consolidation tasks.
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
#include <functional>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>

export module cc.services.auto_dream;

import cc.types.types;

export namespace cc::services::auto_dream {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::seconds;

// ============================================================
// 数据结构
// ============================================================

// 记忆条目
struct MemoryEntry {
    std::string id;
    std::string content;
    std::vector<std::string> tags;
    TimePoint created_at;
    TimePoint last_accessed;
    std::uint32_t access_count{0};
    double relevance_score{1.0};
};

// 提取到的模式
struct ExtractedPattern {
    std::string pattern_id;
    std::string description;
    std::vector<std::string> source_memory_ids;  // 来源记忆
    double confidence{0.0};
    TimePoint extracted_at;
};

// 合并结果
struct ConsolidationResult {
    std::size_t patterns_extracted{0};
    std::size_t memories_merged{0};
    std::size_t entries_pruned{0};
    Duration elapsed{0};
    TimePoint completed_at;
};

// 合并配置
struct DreamConfig {
    Duration idle_threshold{std::chrono::seconds(300)};  // 5 分钟空闲触发
    Duration consolidation_interval{std::chrono::seconds(3600)};  // 每小时
    std::size_t max_memories{10000};
    double prune_threshold{0.1};       // 低于此相关性分数的条目被清除
    std::size_t min_pattern_support{3}; // 最少源记忆数才提取模式
    std::size_t batch_size{100};        // 每轮处理的记忆数
};

// 调度状态
enum class DreamState : std::uint8_t {
    Idle,         // 等待触发
    Running,      // 正在执行合并
    Scheduled,    // 已调度待执行
    Disabled,     // 已禁用
};

// ============================================================
// AutoDreamService - 后台记忆合并
// ============================================================

class AutoDreamService {
public:
    explicit AutoDreamService(DreamConfig config = {})
        : config_(config), state_(DreamState::Idle) {}

    // 添加记忆条目
    VoidResult add_memory(MemoryEntry entry) {
        std::lock_guard lock(mutex_);
        if (memories_.size() >= config_.max_memories) {
            prune_lowest_relevance();
        }
        memories_.push_back(std::move(entry));
        return {};
    }

    // 手动触发合并
    [[nodiscard]] std::expected<ConsolidationResult, Error> consolidate() {
        std::lock_guard lock(mutex_);
        if (state_ == DreamState::Running) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "consolidation already running"});
        }
        state_ = DreamState::Running;
        auto start = Clock::now();

        ConsolidationResult result;
        // 步骤 1: 提取模式
        result.patterns_extracted = extract_patterns();
        // 步骤 2: 合并相关记忆
        result.memories_merged = merge_related();
        // 步骤 3: 清除冗余条目
        result.entries_pruned = prune_redundant();

        auto end = Clock::now();
        result.elapsed = std::chrono::duration_cast<Duration>(end - start);
        result.completed_at = end;
        last_consolidation_ = end;
        state_ = DreamState::Idle;
        return result;
    }

    // 通知用户活动（重置空闲计时器）
    void notify_activity() noexcept {
        last_activity_ = Clock::now();
    }

    // 检查是否应当触发合并
    [[nodiscard]] bool should_consolidate() const noexcept {
        if (state_ != DreamState::Idle) return false;
        auto idle_time = Clock::now() - last_activity_;
        auto since_last = Clock::now() - last_consolidation_;
        return idle_time >= config_.idle_threshold &&
               since_last >= config_.consolidation_interval;
    }

    // 调度定期合并 (由外部事件循环调用)
    void schedule() noexcept { state_ = DreamState::Scheduled; }
    void disable() noexcept { state_ = DreamState::Disabled; }
    void enable() noexcept {
        if (state_ == DreamState::Disabled) state_ = DreamState::Idle;
    }

    // 获取状态
    [[nodiscard]] DreamState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t memory_count() const noexcept { return memories_.size(); }
    [[nodiscard]] std::size_t pattern_count() const noexcept { return patterns_.size(); }
    [[nodiscard]] const DreamConfig& config() const noexcept { return config_; }
    void set_config(DreamConfig config) noexcept { config_ = config; }

    // 获取所有已提取的模式
    [[nodiscard]] const std::vector<ExtractedPattern>& patterns() const noexcept {
        return patterns_;
    }

private:
    DreamConfig config_;
    DreamState state_;
    mutable std::mutex mutex_;
    std::vector<MemoryEntry> memories_;
    std::vector<ExtractedPattern> patterns_;
    TimePoint last_activity_{Clock::now()};
    TimePoint last_consolidation_{};

    // 提取模式: 寻找重复出现的标签组合
    std::size_t extract_patterns() {
        std::unordered_map<std::string, std::vector<std::string>> tag_groups;
        for (const auto& mem : memories_ | std::views::take(config_.batch_size)) {
            for (const auto& tag : mem.tags) {
                tag_groups[tag].push_back(mem.id);
            }
        }
        std::size_t count = 0;
        for (auto& [tag, ids] : tag_groups) {
            if (ids.size() >= config_.min_pattern_support) {
                patterns_.push_back({
                    .pattern_id = std::format("pattern_{}", patterns_.size()),
                    .description = std::format("Recurring theme: {}", tag),
                    .source_memory_ids = std::move(ids),
                    .confidence = 0.7,
                    .extracted_at = Clock::now(),
                });
                ++count;
            }
        }
        return count;
    }

    // 合并相关记忆 (相同标签集合的条目)
    std::size_t merge_related() {
        // 简化实现: 合并完全重复内容的条目
        std::unordered_set<std::string> seen_content;
        std::size_t merged = 0;
        auto it = std::ranges::remove_if(memories_, [&](const MemoryEntry& m) {
            if (seen_content.contains(m.content)) { ++merged; return true; }
            seen_content.insert(m.content);
            return false;
        });
        memories_.erase(it.begin(), it.end());
        return merged;
    }

    // 清除低相关性条目
    std::size_t prune_redundant() {
        auto before = memories_.size();
        std::erase_if(memories_, [this](const MemoryEntry& m) {
            return m.relevance_score < config_.prune_threshold;
        });
        return before - memories_.size();
    }

    // 当超出容量时移除最低相关性条目
    void prune_lowest_relevance() {
        if (memories_.empty()) return;
        auto it = std::ranges::min_element(memories_,
            [](const auto& a, const auto& b) { return a.relevance_score < b.relevance_score; });
        memories_.erase(it);
    }
};

} // namespace cc::services::auto_dream
