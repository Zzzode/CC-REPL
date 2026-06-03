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

// ============================================================


struct MemoryEntry {
    std::string id;
    std::string content;
    std::vector<std::string> tags;
    TimePoint created_at;
    TimePoint last_accessed;
    std::uint32_t access_count{0};
    double relevance_score{1.0};
};


struct ExtractedPattern {
    std::string pattern_id;
    std::string description;
    std::vector<std::string> source_memory_ids;
    double confidence{0.0};
    TimePoint extracted_at;
};


struct ConsolidationResult {
    std::size_t patterns_extracted{0};
    std::size_t memories_merged{0};
    std::size_t entries_pruned{0};
    Duration elapsed{0};
    TimePoint completed_at;
};


struct DreamConfig {
    Duration idle_threshold{std::chrono::seconds(300)};
    Duration consolidation_interval{std::chrono::seconds(3600)};
    std::size_t max_memories{10000};
    double prune_threshold{0.1};
    std::size_t min_pattern_support{3};
    std::size_t batch_size{100};
};


enum class DreamState : std::uint8_t {
    Idle,
    Running,
    Scheduled,
    Disabled,
};

// ============================================================

// ============================================================

class AutoDreamService {
public:
    explicit AutoDreamService(DreamConfig config = {})
        : config_(config), state_(DreamState::Idle) {}


    VoidResult add_memory(MemoryEntry entry) {
        std::lock_guard lock(mutex_);
        if (memories_.size() >= config_.max_memories) {
            prune_lowest_relevance();
        }
        memories_.push_back(std::move(entry));
        return {};
    }


    [[nodiscard]] std::expected<ConsolidationResult, Error> consolidate() {
        std::lock_guard lock(mutex_);
        if (state_ == DreamState::Running) {
            return std::unexpected(Error{ErrorCode::InvalidInput, {}, "consolidation already running"});
        }
        state_ = DreamState::Running;
        auto start = Clock::now();

        ConsolidationResult result;

        result.patterns_extracted = extract_patterns();

        result.memories_merged = merge_related();

        result.entries_pruned = prune_redundant();

        auto end = Clock::now();
        result.elapsed = std::chrono::duration_cast<Duration>(end - start);
        result.completed_at = end;
        last_consolidation_ = end;
        state_ = DreamState::Idle;
        return result;
    }


    void notify_activity() noexcept {
        last_activity_ = Clock::now();
    }


    [[nodiscard]] bool should_consolidate() const noexcept {
        if (state_ != DreamState::Idle) return false;
        auto idle_time = Clock::now() - last_activity_;
        auto since_last = Clock::now() - last_consolidation_;
        return idle_time >= config_.idle_threshold &&
               since_last >= config_.consolidation_interval;
    }


    void schedule() noexcept { state_ = DreamState::Scheduled; }
    void disable() noexcept { state_ = DreamState::Disabled; }
    void enable() noexcept {
        if (state_ == DreamState::Disabled) state_ = DreamState::Idle;
    }


    [[nodiscard]] DreamState state() const noexcept { return state_; }
    [[nodiscard]] std::size_t memory_count() const noexcept { return memories_.size(); }
    [[nodiscard]] std::size_t pattern_count() const noexcept { return patterns_.size(); }
    [[nodiscard]] const DreamConfig& config() const noexcept { return config_; }
    void set_config(DreamConfig config) noexcept { config_ = config; }


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


    std::size_t merge_related() {

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


    std::size_t prune_redundant() {
        auto before = memories_.size();
        std::erase_if(memories_, [this](const MemoryEntry& m) {
            return m.relevance_score < config_.prune_threshold;
        });
        return before - memories_.size();
    }


    void prune_lowest_relevance() {
        if (memories_.empty()) return;
        auto it = std::ranges::min_element(memories_,
            [](const auto& a, const auto& b) { return a.relevance_score < b.relevance_score; });
        memories_.erase(it);
    }
};

} // namespace cc::services::auto_dream
