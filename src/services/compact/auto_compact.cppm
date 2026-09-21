/// @file auto_compact.cppm
/// @brief Auto-compaction service for managing conversation context length.
/// Implements configurable strategies for summarizing and compressing messages
/// when token budgets are exceeded, with priority-based message retention,
/// micro-compaction for tool results, and circuit-breaker mechanisms.
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
#include <algorithm>
#include <ranges>
#include <numeric>
#include <unordered_set>
#include <deque>

export module cc.services.compact;

import cc.types.types;
import cc.utils.error;
import cc.utils.string;
import cc.utils.json;

export namespace cc::services::compact {

using cc::core::Message;
using cc::core::UserMessage;
using cc::core::AssistantMessage;
using cc::core::SystemMessage;
using cc::core::ToolUseMessage;
using cc::core::ToolResultMessage;
using cc::core::ContentBlock;
using cc::core::TextBlock;
using cc::core::ToolUseBlock;
using cc::core::ToolResultBlock;
using cc::core::tool_result_content_text;
using cc::core::ThinkingBlock;
using cc::core::Role;
using cc::core::TokenUsage;
using cc::core::Result;
using cc::core::VoidResult;
using cc::utils::Error;
using cc::utils::ErrorCode;

// ============================================================
// Compaction strategy types
// ============================================================

/// Compaction strategy enum
enum class CompactStrategy : std::uint8_t {
    Aggressive,    // Maximum compression, minimal context retention
    Conservative,  // Preserve as much context as possible
    Selective,     // Intelligent selection based on message importance
    Micro,         // Only micro-compact tool results, preserve full conversation
};

/// Get strategy name as string
[[nodiscard]] constexpr std::string_view strategy_name(CompactStrategy s) noexcept {
    switch (s) {
        case CompactStrategy::Aggressive:   return "aggressive";
        case CompactStrategy::Conservative: return "conservative";
        case CompactStrategy::Selective:    return "selective";
        case CompactStrategy::Micro:        return "micro";
    }
    return "unknown";
}

// ============================================================
// Compaction configuration
// ============================================================

/// Configuration for auto-compaction behavior
struct CompactConfig {
    /// Reserve this many tokens for output during compaction (default: 20000)
    std::size_t max_output_tokens_for_summary = 20000;

    /// Auto-compact when tokens exceed threshold % (default: 0.85)
    double threshold_pct = 0.85;

    /// Target token usage after compaction (% of max, default: 0.50)
    double target_pct = 0.50;

    /// Number of recent messages to always preserve (default: 6)
    std::size_t preserve_recent_n = 6;

    /// Default compaction strategy
    CompactStrategy strategy = CompactStrategy::Selective;

    /// Minimum tokens before compaction is considered (default: 4096)
    std::size_t min_tokens_threshold = 4096;

    /// Auto-compact buffer tokens (default: 13000)
    std::size_t auto_compact_buffer = 13000;

    /// Warning threshold buffer (default: 20000)
    std::size_t warning_threshold_buffer = 20000;

    /// Circuit breaker: max consecutive failures (default: 3)
    std::size_t max_consecutive_failures = 3;

    /// Micro-compaction: keep N most recent tool results (default: 3)
    std::size_t microcompact_keep_recent = 3;

    /// Time-based micro-compaction threshold (seconds, default: 300)
    std::chrono::seconds microcompact_time_threshold{300};

    /// Enable auto-compaction
    bool auto_compact_enabled = true;
};

// ============================================================
// Message utility types
// ============================================================

/// Message priority levels for retention decisions
enum class Priority : std::uint8_t {
    Critical = 0,  // System messages - never drop
    High = 1,      // Recent user/assistant turns
    Medium = 2,    // Tool results that can be summarized
    Low = 3,       // Thinking blocks, old conversation
    Droppable = 4, // Can be removed entirely
};

/// Extended message metadata for compaction
struct MessageWithMetadata {
    Message message;
    std::size_t estimated_tokens = 0;
    std::size_t turn_index = 0;
    bool is_compact_boundary = false;
    bool is_compact_summary = false;

    /// Compute priority based on message characteristics
    [[nodiscard]] Priority priority(std::size_t total_turns, std::size_t preserve_n) const {
        // System messages are always critical
        if (std::holds_alternative<SystemMessage>(message)) return Priority::Critical;

        // Recent messages are high priority
        if (turn_index + preserve_n >= total_turns) return Priority::High;

        // Check content for thinking blocks
        const auto* content_ptr = std::visit([](const auto& m) -> const std::vector<ContentBlock>* {
            return &m.content;
        }, message);

        if (content_ptr) {
            for (const auto& block : *content_ptr) {
                if (std::holds_alternative<ThinkingBlock>(block)) {
                    return Priority::Droppable;
                }
            }
        }

        // Tool results can be summarized
        if (std::holds_alternative<ToolResultMessage>(message)) {
            return Priority::Medium;
        }

        // Older conversation turns
        return Priority::Low;
    }
};

// ============================================================
// Compaction result
// ============================================================

/// Result of a compaction operation
struct CompactResult {
    std::size_t original_count = 0;    // Messages before compaction
    std::size_t compacted_count = 0;   // Messages after compaction
    std::size_t tokens_saved = 0;      // Tokens freed by compaction
    std::size_t original_tokens = 0;   // Tokens before compaction
    std::size_t compacted_tokens = 0;  // Tokens after compaction
    std::string summary;               // Generated summary of dropped content
    std::vector<Message> messages;     // Resulting message list
    bool was_compacted = false;        // Whether compaction actually occurred
    bool is_auto = false;              // Whether this was auto-compaction
    std::optional<std::string> error;  // Error if compaction failed

    /// Compression ratio (0.0 = no savings, 1.0 = maximum savings)
    [[nodiscard]] double compression_ratio() const noexcept {
        if (original_tokens == 0) return 0.0;
        return 1.0 - static_cast<double>(compacted_tokens) / static_cast<double>(original_tokens);
    }
};

// ============================================================
// Tracking state for auto-compaction
// ============================================================

/// State to track across auto-compaction attempts
struct AutoCompactTrackingState {
    bool has_been_compacted = false;
    std::size_t turn_counter = 0;
    std::string last_turn_id;
    std::size_t consecutive_failures = 0;
    std::optional<std::chrono::system_clock::time_point> last_compact_time;
};

// ============================================================
// Token estimation utilities
// ============================================================

/// Approximate token count using character-based heuristic.
/// ~4 characters per token for English text, ~2 for CJK.
[[nodiscard]] inline std::size_t estimate_tokens_for_text(std::string_view text) {
    if (text.empty()) return 0;
    std::size_t cjk_chars = 0;
    std::size_t ascii_chars = 0;
    for (unsigned char c : text) {
        if (c > 127) ++cjk_chars;
        else ++ascii_chars;
    }
    return (cjk_chars / 2) + (ascii_chars / 4) + 1;
}

/// Estimate tokens for a single content block
[[nodiscard]] inline std::size_t estimate_tokens_for_block(const ContentBlock& block) {
    return std::visit([](const auto& b) -> std::size_t {
        using T = std::decay_t<decltype(b)>;
        if constexpr (std::is_same_v<T, TextBlock>) {
            return estimate_tokens_for_text(b.text);
        } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
            return estimate_tokens_for_text(b.name) + estimate_tokens_for_text(b.input_json);
        } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
            return estimate_tokens_for_text(tool_result_content_text(b));
        } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
            return estimate_tokens_for_text(b.thinking);
        }
        return 0;
    }, block);
}

/// Estimate tokens for a single message
[[nodiscard]] inline std::size_t estimate_tokens_for_message(const Message& msg) {
    return std::visit([](const auto& m) -> std::size_t {
        std::size_t total = 0;
        for (const auto& block : m.content) {
            total += estimate_tokens_for_block(block);
        }
        return total;
    }, msg);
}

/// Estimate total tokens for a message list
[[nodiscard]] inline std::size_t estimate_tokens(const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const auto& msg : messages) {
        total += estimate_tokens_for_message(msg);
    }
    return total;
}

// ============================================================
// Auto-compact threshold calculations
// ============================================================

/// Get effective context window size (max tokens - reserved for output)
[[nodiscard]] inline std::size_t get_effective_context_window(
        std::size_t max_context_window,
        const CompactConfig& config) {
    auto reserved = std::min(config.max_output_tokens_for_summary, max_context_window / 2);
    return max_context_window - reserved;
}

/// Get auto-compact threshold for a given model window size
[[nodiscard]] inline std::size_t get_auto_compact_threshold(
        std::size_t max_context_window,
        const CompactConfig& config) {
    auto effective = get_effective_context_window(max_context_window, config);
    auto threshold = static_cast<std::size_t>(effective * config.threshold_pct);
    return std::max(threshold, config.min_tokens_threshold);
}

/// Get warning threshold for token usage
[[nodiscard]] inline std::size_t get_warning_threshold(
        std::size_t max_context_window,
        const CompactConfig& config) {
    auto auto_threshold = get_auto_compact_threshold(max_context_window, config);
    return std::max(auto_threshold - config.warning_threshold_buffer, config.min_tokens_threshold);
}

/// Check if we should auto-compact based on current token usage
[[nodiscard]] inline bool should_auto_compact(
        std::size_t current_tokens,
        std::size_t max_context_window,
        const CompactConfig& config,
        const AutoCompactTrackingState& tracking) {
    if (!config.auto_compact_enabled) return false;

    // Circuit breaker: stop after max consecutive failures
    if (tracking.consecutive_failures >= config.max_consecutive_failures) {
        return false;
    }

    if (current_tokens < config.min_tokens_threshold) return false;

    auto threshold = get_auto_compact_threshold(max_context_window, config);
    return current_tokens >= threshold;
}

// ============================================================
// Micro-compaction (tool result cleanup)
// ============================================================

/// Result of micro-compaction
struct MicroCompactResult {
    std::vector<Message> messages;
    std::size_t tokens_saved = 0;
    bool was_compacted = false;
    std::vector<std::string> cleared_tool_ids;
};

/// Check if a tool is compactable
[[nodiscard]] inline bool is_compactable_tool(std::string_view tool_name) {
    static const std::unordered_set<std::string_view> compactable_tools = {
        "Read", "Write", "Edit", "Glob", "Grep", "Bash", "Powershell",
        "WebFetch", "WebSearch", "Ls", "MCP", "Script"
    };
    return compactable_tools.contains(tool_name);
}

/// Perform micro-compaction - clean up old tool results
[[nodiscard]] inline MicroCompactResult microcompact(
        const std::vector<Message>& messages,
        const CompactConfig& config,
        std::optional<std::chrono::system_clock::time_point> last_assistant_time = std::nullopt) {
    MicroCompactResult result;
    result.messages = messages;
    result.was_compacted = false;
    result.tokens_saved = 0;

    // Check time-based trigger first
    bool time_based = false;
    if (last_assistant_time) {
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *last_assistant_time);
        if (elapsed >= config.microcompact_time_threshold) {
            time_based = true;
        }
    }

    // Collect tool use/result pairs
    std::vector<std::pair<std::size_t, std::string>> tool_use_indices;  // index, tool_use_id
    std::vector<std::pair<std::size_t, std::string>> tool_result_indices;  // index, tool_use_id

    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (std::holds_alternative<ToolUseMessage>(msg)) {
            const auto& tum = std::get<ToolUseMessage>(msg);
            for (const auto& block : tum.content) {
                if (std::holds_alternative<ToolUseBlock>(block)) {
                    tool_use_indices.emplace_back(i, std::get<ToolUseBlock>(block).id.value);
                }
            }
        } else if (std::holds_alternative<ToolResultMessage>(msg)) {
            const auto& trm = std::get<ToolResultMessage>(msg);
            tool_result_indices.emplace_back(i, trm.tool_use_id.value);
        }
    }

    // Keep N most recent, mark older ones for clearing
    if (tool_result_indices.size() > config.microcompact_keep_recent || time_based) {
        std::unordered_set<std::string> keep_ids;
        std::size_t start = time_based ? 0 : tool_result_indices.size() - config.microcompact_keep_recent;
        for (std::size_t i = start; i < tool_result_indices.size(); ++i) {
            keep_ids.insert(tool_result_indices[i].second);
        }

        // Modify messages - clear content of old tool results
        bool modified = false;
        for (const auto& [idx, tool_id] : tool_result_indices) {
            if (!keep_ids.contains(tool_id)) {
                auto& msg = result.messages[idx];
                if (std::holds_alternative<ToolResultMessage>(msg)) {
                    auto& trm = std::get<ToolResultMessage>(msg);
                    std::size_t original_tokens = estimate_tokens_for_message(msg);

                    // Clear content blocks
                    bool has_content = false;
                    for (auto& block : trm.content) {
                        if (std::holds_alternative<ToolResultBlock>(block)) {
                            auto& trb = std::get<ToolResultBlock>(block);
                            if (tool_result_content_text(trb).size() > 100) {
                                result.tokens_saved += original_tokens - estimate_tokens_for_text("[Old tool result cleared]");
                                trb.content = "[Old tool result cleared]";
                                modified = true;
                                has_content = true;
                            }
                        }
                    }

                    if (has_content) {
                        result.cleared_tool_ids.push_back(tool_id);
                    }
                }
            }
        }

        result.was_compacted = modified;
    }

    return result;
}

// ============================================================
// Compaction strategies implementations
// ============================================================

class AutoCompactor {
public:
    explicit AutoCompactor(CompactConfig config = {})
        : config_(std::move(config)) {}

    /// Get current configuration
    [[nodiscard]] const CompactConfig& config() const noexcept { return config_; }

    /// Update configuration
    void set_config(CompactConfig config) { config_ = std::move(config); }

    /// Update strategy
    void set_strategy(CompactStrategy strategy) noexcept { config_.strategy = strategy; }

    /// Determine if compaction should be triggered
    [[nodiscard]] bool should_compact(
            std::size_t current_tokens,
            std::size_t max_context_window) const {
        return should_auto_compact(current_tokens, max_context_window, config_, tracking_);
    }

    /// Get effective token budget after compaction
    [[nodiscard]] std::size_t get_target_budget(std::size_t max_context_window) const {
        auto effective = get_effective_context_window(max_context_window, config_);
        return static_cast<std::size_t>(effective * config_.target_pct);
    }

    /// Perform full compaction
    [[nodiscard]] CompactResult compact(
            std::vector<Message> messages,
            std::size_t max_context_window,
            bool is_auto = false) {
        CompactResult result;
        result.original_count = messages.size();
        result.original_tokens = estimate_tokens(messages);
        result.is_auto = is_auto;

        // Check if we even need to compact
        auto target_budget = get_target_budget(max_context_window);
        if (result.original_tokens <= target_budget) {
            result.compacted_count = messages.size();
            result.compacted_tokens = result.original_tokens;
            result.messages = std::move(messages);
            result.was_compacted = false;
            return result;
        }

        // Try micro-compact first if strategy allows
        if (config_.strategy == CompactStrategy::Micro) {
            auto micro_result = microcompact(messages, config_);
            if (micro_result.was_compacted && estimate_tokens(micro_result.messages) <= target_budget) {
                result.messages = std::move(micro_result.messages);
                result.compacted_count = result.messages.size();
                result.compacted_tokens = estimate_tokens(result.messages);
                result.tokens_saved = micro_result.tokens_saved;
                result.was_compacted = true;
                result.summary = std::format("Micro-compacted {} tool results", micro_result.cleared_tool_ids.size());
                return result;
            }
        }

        // Convert to metadata-enriched messages
        std::vector<MessageWithMetadata> with_meta;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            MessageWithMetadata m;
            m.message = std::move(messages[i]);
            m.estimated_tokens = estimate_tokens_for_message(m.message);
            m.turn_index = i;
            with_meta.push_back(std::move(m));
        }

        // Apply strategy-specific compaction
        switch (config_.strategy) {
            case CompactStrategy::Aggressive:
                result = compact_aggressive(std::move(with_meta), target_budget);
                break;
            case CompactStrategy::Conservative:
                result = compact_conservative(std::move(with_meta), target_budget);
                break;
            case CompactStrategy::Selective:
            case CompactStrategy::Micro:
                result = compact_selective(std::move(with_meta), target_budget);
                break;
        }

        result.is_auto = is_auto;
        result.original_count = messages.size();

        if (result.was_compacted) {
            tracking_.has_been_compacted = true;
            tracking_.consecutive_failures = 0;
            tracking_.last_compact_time = std::chrono::system_clock::now();
        }

        return result;
    }

    /// Record a compaction failure (for circuit breaker)
    void record_failure() {
        ++tracking_.consecutive_failures;
    }

    /// Reset failure count
    void reset_failures() {
        tracking_.consecutive_failures = 0;
    }

    /// Get tracking state
    [[nodiscard]] const AutoCompactTrackingState& tracking() const noexcept { return tracking_; }

    /// Update tracking state
    void set_tracking(AutoCompactTrackingState state) { tracking_ = std::move(state); }

private:
    CompactConfig config_;
    AutoCompactTrackingState tracking_;

    /// Aggressive strategy: keep only critical and high priority
    [[nodiscard]] CompactResult compact_aggressive(
            std::vector<MessageWithMetadata> messages,
            std::size_t token_budget) {
        CompactResult result;
        result.original_count = messages.size();

        std::vector<std::string> dropped_content;
        std::size_t budget_used = 0;

        // First pass: collect critical and high priority messages in order
        std::vector<std::size_t> keep_indices;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto prio = messages[i].priority(messages.size(), config_.preserve_recent_n);
            if (prio <= Priority::High) {
                auto msg_tokens = messages[i].estimated_tokens > 0
                    ? messages[i].estimated_tokens
                    : estimate_tokens_for_message(messages[i].message);
                if (budget_used + msg_tokens <= token_budget) {
                    keep_indices.push_back(i);
                    budget_used += msg_tokens;
                } else {
                    dropped_content.push_back(get_snippet(messages[i].message, 100));
                }
            } else {
                dropped_content.push_back(get_snippet(messages[i].message, 100));
            }
        }

        // Build result in original order
        for (auto idx : keep_indices) {
            result.messages.push_back(std::move(messages[idx].message));
        }

        result.compacted_count = result.messages.size();
        result.compacted_tokens = estimate_tokens(result.messages);
        result.tokens_saved = estimate_tokens_for_metadata_list(messages) - result.compacted_tokens;
        result.summary = generate_summary(dropped_content);
        result.was_compacted = !dropped_content.empty();

        return result;
    }

    /// Conservative strategy: summarize rather than drop
    [[nodiscard]] CompactResult compact_conservative(
            std::vector<MessageWithMetadata> messages,
            std::size_t token_budget) {
        CompactResult result;
        result.original_count = messages.size();

        std::size_t budget_used = 0;
        std::vector<std::size_t> keep_indices;
        std::vector<std::pair<std::size_t, Message>> summarized;
        std::vector<std::string> dropped_content;

        // First pass: keep all critical, then add high priority in order
        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto prio = messages[i].priority(messages.size(), config_.preserve_recent_n);
            auto msg_tokens = messages[i].estimated_tokens > 0
                ? messages[i].estimated_tokens
                : estimate_tokens_for_message(messages[i].message);

            if (prio == Priority::Critical) {
                keep_indices.push_back(i);
                budget_used += msg_tokens;
            } else if (prio <= Priority::High && budget_used + msg_tokens <= token_budget) {
                keep_indices.push_back(i);
                budget_used += msg_tokens;
            } else if (prio == Priority::Medium) {
                // Try to summarize
                auto summarized_msg = summarize_message(messages[i].message);
                auto summary_tokens = estimate_tokens_for_message(summarized_msg);
                if (budget_used + summary_tokens <= token_budget) {
                    summarized.emplace_back(i, std::move(summarized_msg));
                    budget_used += summary_tokens;
                } else {
                    dropped_content.push_back(get_snippet(messages[i].message, 50));
                }
            } else {
                dropped_content.push_back(get_snippet(messages[i].message, 50));
            }
        }

        // Build final message list preserving order
        std::unordered_set<std::size_t> inserted_summaries;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            // Check if this index was kept
            if (std::find(keep_indices.begin(), keep_indices.end(), i) != keep_indices.end()) {
                result.messages.push_back(std::move(messages[i].message));
                continue;
            }
            // Check if we have a summarized version
            for (auto& [idx, msg] : summarized) {
                if (idx == i && !inserted_summaries.contains(i)) {
                    result.messages.push_back(std::move(msg));
                    inserted_summaries.insert(i);
                    break;
                }
            }
        }

        result.compacted_count = result.messages.size();
        result.compacted_tokens = estimate_tokens(result.messages);
        result.tokens_saved = estimate_tokens_for_metadata_list(messages) - result.compacted_tokens;
        result.summary = generate_summary(dropped_content);
        result.was_compacted = !dropped_content.empty() || !summarized.empty();

        return result;
    }

    /// Selective strategy: intelligent trimming
    [[nodiscard]] CompactResult compact_selective(
            std::vector<MessageWithMetadata> messages,
            std::size_t token_budget) {
        CompactResult result;
        result.original_count = messages.size();

        std::size_t budget_used = 0;
        std::vector<std::size_t> keep_indices;
        std::vector<std::pair<std::size_t, Message>> truncated;
        std::vector<std::string> dropped_content;

        // First pass: always keep critical messages
        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto prio = messages[i].priority(messages.size(), config_.preserve_recent_n);
            if (prio == Priority::Critical) {
                keep_indices.push_back(i);
                budget_used += messages[i].estimated_tokens > 0
                    ? messages[i].estimated_tokens
                    : estimate_tokens_for_message(messages[i].message);
            }
        }

        // Second pass: keep recent messages (preserve_recent_n) from the end
        std::size_t start_keep = messages.size() > config_.preserve_recent_n
            ? messages.size() - config_.preserve_recent_n
            : 0;
        for (std::size_t i = start_keep; i < messages.size(); ++i) {
            if (std::find(keep_indices.begin(), keep_indices.end(), i) == keep_indices.end()) {
                auto msg_tokens = messages[i].estimated_tokens > 0
                    ? messages[i].estimated_tokens
                    : estimate_tokens_for_message(messages[i].message);
                if (budget_used + msg_tokens <= token_budget) {
                    keep_indices.push_back(i);
                    budget_used += msg_tokens;
                }
            }
        }

        // Third pass: process older messages with truncation
        for (std::size_t i = 0; i < start_keep; ++i) {
            if (std::find(keep_indices.begin(), keep_indices.end(), i) != keep_indices.end()) {
                continue;
            }

            auto& msg_meta = messages[i];
            auto prio = msg_meta.priority(messages.size(), config_.preserve_recent_n);

            // Drop thinking blocks entirely
            bool has_thinking = false;
            std::visit([&has_thinking](const auto& m) {
                for (const auto& block : m.content) {
                    if (std::holds_alternative<ThinkingBlock>(block)) {
                        has_thinking = true;
                        break;
                    }
                }
            }, msg_meta.message);
            if (has_thinking) {
                auto stripped = strip_thinking_blocks(msg_meta.message);
                auto stripped_tokens = estimate_tokens_for_message(stripped);
                if (budget_used + stripped_tokens <= token_budget) {
                    truncated.emplace_back(i, std::move(stripped));
                    budget_used += stripped_tokens;
                    continue;
                } else {
                    dropped_content.push_back("[Thinking block dropped]");
                    continue;
                }
            }

            // Truncate long tool results
            if (std::holds_alternative<ToolResultMessage>(msg_meta.message)) {
                auto truncated_msg = truncate_tool_result_message(msg_meta.message);
                auto trunc_tokens = estimate_tokens_for_message(truncated_msg);
                if (budget_used + trunc_tokens <= token_budget) {
                    truncated.emplace_back(i, std::move(truncated_msg));
                    budget_used += trunc_tokens;
                } else {
                    dropped_content.push_back(get_snippet(msg_meta.message, 60));
                }
                continue;
            }

            // Otherwise check if we can keep it
            auto msg_tokens = msg_meta.estimated_tokens > 0
                ? msg_meta.estimated_tokens
                : estimate_tokens_for_message(msg_meta.message);
            if (budget_used + msg_tokens <= token_budget) {
                keep_indices.push_back(i);
                budget_used += msg_tokens;
            } else if (prio <= Priority::Medium) {
                // Try to summarize
                auto summarized_msg = summarize_message(msg_meta.message);
                auto summary_tokens = estimate_tokens_for_message(summarized_msg);
                if (budget_used + summary_tokens <= token_budget) {
                    truncated.emplace_back(i, std::move(summarized_msg));
                    budget_used += summary_tokens;
                } else {
                    dropped_content.push_back(get_snippet(msg_meta.message, 60));
                }
            } else {
                dropped_content.push_back(get_snippet(msg_meta.message, 60));
            }
        }

        // Build final message list preserving order
        std::unordered_set<std::size_t> inserted;
        std::sort(keep_indices.begin(), keep_indices.end());

        std::size_t keep_idx = 0;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            // Add kept messages
            if (keep_idx < keep_indices.size() && keep_indices[keep_idx] == i) {
                result.messages.push_back(std::move(messages[i].message));
                ++keep_idx;
                continue;
            }
            // Add truncated/summarized
            for (auto& [idx, msg] : truncated) {
                if (idx == i && !inserted.contains(i)) {
                    result.messages.push_back(std::move(msg));
                    inserted.insert(i);
                    break;
                }
            }
        }

        result.compacted_count = result.messages.size();
        result.compacted_tokens = estimate_tokens(result.messages);
        result.tokens_saved = estimate_tokens_for_metadata_list(messages) - result.compacted_tokens;
        result.summary = generate_summary(dropped_content);
        result.was_compacted = !dropped_content.empty() || !truncated.empty();

        return result;
    }

    // Helper: get text snippet from a message
    [[nodiscard]] static std::string get_snippet(const Message& msg, std::size_t max_len) {
        return std::visit([max_len](const auto& m) -> std::string {
            std::string result;
            for (const auto& block : m.content) {
                if (std::holds_alternative<TextBlock>(block)) {
                    result += std::get<TextBlock>(block).text;
                } else if (std::holds_alternative<ToolUseBlock>(block)) {
                    result += std::format("[Tool: {}]", std::get<ToolUseBlock>(block).name);
                } else if (std::holds_alternative<ToolResultBlock>(block)) {
                    result += "[Tool Result]";
                }
                if (result.size() >= max_len) break;
            }
            if (result.size() > max_len) {
                result = result.substr(0, max_len) + "...";
            }
            return result.empty() ? "[Message]" : result;
        }, msg);
    }

    // Helper: generate summary from dropped snippets
    [[nodiscard]] static std::string generate_summary(const std::vector<std::string>& dropped) {
        if (dropped.empty()) return {};
        std::string summary = std::format("[Compacted {} messages: ", dropped.size());
        std::size_t shown = std::min(dropped.size(), std::size_t{3});
        for (std::size_t i = 0; i < shown; ++i) {
            if (i > 0) summary += "; ";
            summary += dropped[i];
        }
        if (dropped.size() > shown) {
            summary += std::format("; ...and {} more", dropped.size() - shown);
        }
        summary += "]";
        return summary;
    }

    // Helper: strip thinking blocks from a message
    [[nodiscard]] static Message strip_thinking_blocks(const Message& msg) {
        return std::visit([](const auto& m) -> Message {
            using T = std::decay_t<decltype(m)>;
            T result = m;
            result.content.clear();
            for (const auto& block : m.content) {
                if (!std::holds_alternative<ThinkingBlock>(block)) {
                    result.content.push_back(block);
                }
            }
            return result;
        }, msg);
    }

    // Helper: truncate tool result message
    [[nodiscard]] static Message truncate_tool_result_message(const Message& msg) {
        return std::visit([](const auto& m) -> Message {
            using T = std::decay_t<decltype(m)>;
            T result = m;
            for (auto& block : result.content) {
                if (std::holds_alternative<ToolResultBlock>(block)) {
                    auto& trb = std::get<ToolResultBlock>(block);
                    auto text = tool_result_content_text(trb);
                    if (text.size() > 500) {
                        trb.content = text.substr(0, 500) + "\n... [truncated]";
                    }
                }
            }
            return result;
        }, msg);
    }

    // Helper: create a summarized version of a message
    [[nodiscard]] static Message summarize_message(const Message& msg) {
        return std::visit([](const auto& m) -> Message {
            using T = std::decay_t<decltype(m)>;
            T result = m;
            result.content.clear();

            std::string summary_text;
            for (const auto& block : m.content) {
                if (std::holds_alternative<TextBlock>(block)) {
                    auto text = std::get<TextBlock>(block).text;
                    if (text.size() > 200) {
                        summary_text += text.substr(0, 200) + "... ";
                    } else {
                        summary_text += text + " ";
                    }
                } else if (std::holds_alternative<ToolUseBlock>(block)) {
                    summary_text += std::format("[Tool: {}] ", std::get<ToolUseBlock>(block).name);
                } else if (std::holds_alternative<ToolResultBlock>(block)) {
                    summary_text += "[Tool Result] ";
                }
            }

            if (!summary_text.empty()) {
                result.content.push_back(TextBlock{summary_text});
            }
            return result;
        }, msg);
    }

    // Helper: estimate tokens for metadata list
    [[nodiscard]] static std::size_t estimate_tokens_for_metadata_list(
            const std::vector<MessageWithMetadata>& messages) {
        std::size_t total = 0;
        for (const auto& m : messages) {
            total += m.estimated_tokens > 0
                ? m.estimated_tokens
                : estimate_tokens_for_message(m.message);
        }
        return total;
    }
};

// ============================================================
// Compact warning state
// ============================================================

/// State for compact warning notifications
struct CompactWarningState {
    bool warning_suppressed = false;
    std::optional<std::chrono::system_clock::time_point> last_warning_time;
    std::size_t warning_count = 0;
};

/// Suppress compact warnings temporarily
inline void suppress_compact_warning(CompactWarningState& state) {
    state.warning_suppressed = true;
    state.last_warning_time = std::chrono::system_clock::now();
}

/// Clear compact warning suppression
inline void clear_compact_warning_suppression(CompactWarningState& state) {
    state.warning_suppressed = false;
}

// ============================================================
// Token warning calculation
// ============================================================

/// Token warning state
struct TokenWarningState {
    double percent_left = 1.0;
    bool is_above_warning_threshold = false;
    bool is_above_error_threshold = false;
    bool is_above_auto_compact_threshold = false;
    bool is_at_blocking_limit = false;
};

/// Calculate token warning state
[[nodiscard]] inline TokenWarningState calculate_token_warning_state(
        std::size_t token_usage,
        std::size_t max_context_window,
        const CompactConfig& config) {
    TokenWarningState state;

    auto auto_threshold = get_auto_compact_threshold(max_context_window, config);
    auto effective_window = get_effective_context_window(max_context_window, config);

    auto threshold = config.auto_compact_enabled ? auto_threshold : effective_window;

    if (threshold > 0) {
        state.percent_left = std::max(0.0,
            1.0 - static_cast<double>(token_usage) / static_cast<double>(threshold));
    }

    auto warning_threshold = threshold > config.warning_threshold_buffer
        ? threshold - config.warning_threshold_buffer
        : config.min_tokens_threshold;

    state.is_above_warning_threshold = token_usage >= warning_threshold;
    state.is_above_error_threshold = token_usage >= threshold - (config.warning_threshold_buffer / 2);
    state.is_above_auto_compact_threshold = config.auto_compact_enabled && token_usage >= auto_threshold;

    auto blocking_limit = effective_window > 3000 ? effective_window - 3000 : config.min_tokens_threshold;
    state.is_at_blocking_limit = token_usage >= blocking_limit;

    return state;
}

} // namespace cc::services::compact
