/// @file compact.cppm
/// @brief CompactCommand implementing the /compact slash command.
/// Context window compression: summarizes conversation history,
/// preserves key information, and manages token budgets.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <numeric>
#include <span>

export module cc.commands.compact;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Token budget configuration for compaction
struct TokenBudget {
    std::uint32_t context_window = 200000;    // Total context window capacity
    std::uint32_t reserve_output = 16384;     // Reserved for output generation
    std::uint32_t reserve_system = 8000;      // Reserved for system prompt
    std::uint32_t target_after_compact = 0;   // Target token count after compaction (0 = auto)

    /// Calculate available tokens for conversation history
    [[nodiscard]] std::uint32_t available_for_history() const noexcept {
        return context_window - reserve_output - reserve_system;
    }

    /// Calculate target size after compaction (default: 50% of available)
    [[nodiscard]] std::uint32_t effective_target() const noexcept {
        if (target_after_compact > 0) return target_after_compact;
        return available_for_history() / 2;
    }
};

/// Statistics about the compaction operation
struct CompactStats {
    std::uint32_t messages_before = 0;        // Message count before compaction
    std::uint32_t messages_after = 0;         // Message count after compaction
    std::uint32_t tokens_before = 0;          // Estimated tokens before
    std::uint32_t tokens_after = 0;           // Estimated tokens after
    double compression_ratio = 0.0;           // tokens_after / tokens_before

    /// Format stats for display
    [[nodiscard]] std::string format() const {
        return std::format(
            "Compaction complete:\n"
            "  Messages: {} -> {} ({} removed)\n"
            "  Tokens:   ~{} -> ~{} ({:.0f}% reduction)\n"
            "  Ratio:    {:.2f}x compression",
            messages_before, messages_after, messages_before - messages_after,
            tokens_before, tokens_after,
            (1.0 - compression_ratio) * 100.0,
            1.0 / compression_ratio
        );
    }
};

/// Priority level for message preservation during compaction
enum class PreservePriority : std::uint8_t {
    Critical,    // Never remove: system prompts, final decisions
    High,        // Preserve if possible: key code, user requirements
    Medium,      // Summarize: intermediate reasoning, explanations
    Low,         // Aggressively compress: tool results, verbose output
    Disposable,  // Remove entirely: debug output, retries
};

/// CompactCommand implements the /compact slash command.
/// Compresses conversation context to fit within token budget.
class CompactCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "compact",
            .description = "Compress conversation context to free up token budget",
            .args = {
                CommandArg{.name = "--target", .description = "Target token count after compaction",
                           .type = ArgType::Text, .required = false},
                CommandArg{.name = "--aggressive", .description = "More aggressive compression",
                           .type = ArgType::None, .required = false},
                CommandArg{.name = "--dry-run", .description = "Show what would be compressed without applying",
                           .type = ArgType::None, .required = false},
            },
            .category = "session",
            .aliases = {"compress"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& /*ctx*/) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto opts = parse_options(ctx.args);
        auto active_messages = conversation_messages_;
        if (active_messages.empty() && ctx.compact_message_provider) {
            active_messages = ctx.compact_message_provider(ctx.runtime_state);
        }
        if (active_messages.empty()) {
            return CommandResult::fail("No active conversation to compact.");
        }

        auto budget = budget_;
        if (opts.target) budget.target_after_compact = *opts.target;

        // Step 1: Analyze current conversation state
        auto analysis = analyze_conversation(active_messages);
        if (analysis.tokens_before <= budget.effective_target()) {
            return CommandResult::success(std::format(
                "Context is already within budget (~{} tokens, target: {}).\n"
                "No compaction needed.",
                analysis.tokens_before, budget.effective_target()
            ));
        }

        // Step 2: Build compaction plan
        auto plan = build_compaction_plan(active_messages, opts.aggressive);

        // Step 3: Dry-run mode - show plan without applying
        if (opts.dry_run) {
            return CommandResult::success(format_plan(plan, analysis));
        }

        if (ctx.compact_applier) {
            auto compacted = ctx.compact_applier(ctx.runtime_state);
            if (!compacted) return std::unexpected(compacted.error());

            auto after_messages = ctx.compact_message_provider
                ? ctx.compact_message_provider(ctx.runtime_state)
                : active_messages;
            analysis.messages_after = static_cast<std::uint32_t>(after_messages.size());
            analysis.tokens_after = estimate_messages_tokens(after_messages);
            analysis.compression_ratio = analysis.tokens_before == 0
                ? 1.0
                : static_cast<double>(analysis.tokens_after) / static_cast<double>(analysis.tokens_before);
            return CommandResult::success(analysis.format());
        }

        // Step 4: Generate summary via LLM for content that will be compressed
        conversation_messages_ = active_messages;
        auto summary_prompt = build_summary_prompt(plan);
        return CommandResult::inject(std::move(summary_prompt));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array flags = {"--target", "--aggressive", "--dry-run"};
        for (auto flag : flags) {
            if (std::string_view(flag).starts_with(partial)) {
                suggestions.emplace_back(flag);
            }
        }
        return suggestions;
    }

    /// Set the current conversation messages (called by session manager)
    void set_messages(std::vector<Message> messages) {
        conversation_messages_ = std::move(messages);
    }

    /// Set token budget (called by config)
    void set_budget(TokenBudget budget) {
        budget_ = budget;
    }

private:
    std::vector<Message> conversation_messages_;
    TokenBudget budget_;

    /// Parsed command options
    struct Options {
        std::optional<std::uint32_t> target;
        bool aggressive = false;
        bool dry_run = false;
    };

    /// Compaction plan entry
    struct PlanEntry {
        std::size_t message_index;
        PreservePriority priority;
        std::string action;             // "keep", "summarize", "remove"
        std::uint32_t estimated_tokens;
    };

    /// Parse options from arguments
    [[nodiscard]] static Options parse_options(std::span<const std::string> args) {
        Options opts;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--target" && i + 1 < args.size()) {
                try { opts.target = std::stoul(args[++i]); } catch (...) {}
            } else if (args[i] == "--aggressive") {
                opts.aggressive = true;
            } else if (args[i] == "--dry-run") {
                opts.dry_run = true;
            }
        }
        return opts;
    }

    /// Analyze conversation to get token statistics
    [[nodiscard]] CompactStats analyze_conversation(const std::vector<Message>& messages) const {
        CompactStats stats;
        stats.messages_before = static_cast<std::uint32_t>(messages.size());

        // Estimate tokens (rough: 4 chars ~= 1 token)
        stats.tokens_before = estimate_messages_tokens(messages);
        return stats;
    }

    [[nodiscard]] static std::uint32_t estimate_messages_tokens(const std::vector<Message>& messages) {
        return std::accumulate(messages.begin(), messages.end(), std::uint32_t{0},
            [](std::uint32_t total, const Message& msg) {
                return total + estimate_message_tokens(msg);
            });
    }

    /// Estimate token count for a message (simplified heuristic)
    [[nodiscard]] static std::uint32_t estimate_message_tokens(const Message& msg) {
        std::uint32_t tokens = 0;
        std::visit([&tokens](const auto& m) {
            for (const auto& block : m.content) {
                std::visit([&tokens](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, TextBlock>) {
                        tokens += static_cast<std::uint32_t>(b.text.size()) / 4;
                    } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                        tokens += static_cast<std::uint32_t>(b.content.size()) / 4;
                    } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                        tokens += static_cast<std::uint32_t>(b.input_json.size()) / 4;
                    } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                        tokens += static_cast<std::uint32_t>(b.thinking.size()) / 4;
                    }
                }, block);
            }
        }, msg);
        return std::max(tokens, 1u);
    }

    /// Classify a message's preservation priority
    [[nodiscard]] static PreservePriority classify_priority(const Message& msg) {
        return std::visit([](const auto& m) -> PreservePriority {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, SystemMessage>) {
                return PreservePriority::Critical;
            } else if constexpr (std::is_same_v<T, UserMessage>) {
                return PreservePriority::High;
            } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
                return PreservePriority::Low;
            } else {
                return PreservePriority::Medium;
            }
        }, msg);
    }

    /// Build a plan for what to compact
    [[nodiscard]] std::vector<PlanEntry> build_compaction_plan(
        const std::vector<Message>& messages, bool aggressive
    ) const {
        std::vector<PlanEntry> plan;
        plan.reserve(messages.size());

        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto priority = classify_priority(messages[i]);
            auto tokens = estimate_message_tokens(messages[i]);

            std::string action;
            if (priority == PreservePriority::Critical) {
                action = "keep";
            } else if (priority == PreservePriority::Disposable) {
                action = "remove";
            } else if (aggressive && priority >= PreservePriority::Medium) {
                action = "summarize";
            } else if (priority == PreservePriority::Low) {
                action = "summarize";
            } else {
                action = "keep";
            }

            plan.push_back(PlanEntry{
                .message_index = i,
                .priority = priority,
                .action = std::move(action),
                .estimated_tokens = tokens,
            });
        }
        return plan;
    }

    /// Format plan for dry-run display
    [[nodiscard]] static std::string format_plan(
        const std::vector<PlanEntry>& plan, const CompactStats& stats
    ) {
        std::string output = "Compaction Plan (dry-run):\n\n";

        std::uint32_t keep_tokens = 0, summarize_tokens = 0, remove_tokens = 0;
        for (const auto& entry : plan) {
            if (entry.action == "keep") keep_tokens += entry.estimated_tokens;
            else if (entry.action == "summarize") summarize_tokens += entry.estimated_tokens;
            else remove_tokens += entry.estimated_tokens;
        }

        output += std::format("  Keep:      ~{} tokens ({} messages)\n", keep_tokens,
            std::ranges::count_if(plan, [](const auto& e) { return e.action == "keep"; }));
        output += std::format("  Summarize: ~{} tokens ({} messages)\n", summarize_tokens,
            std::ranges::count_if(plan, [](const auto& e) { return e.action == "summarize"; }));
        output += std::format("  Remove:    ~{} tokens ({} messages)\n", remove_tokens,
            std::ranges::count_if(plan, [](const auto& e) { return e.action == "remove"; }));
        output += std::format("\n  Current: ~{} tokens\n", stats.tokens_before);
        output += std::format("  Estimated after: ~{} tokens\n", keep_tokens + summarize_tokens / 3);

        return output;
    }

    /// Build a prompt for the LLM to summarize compressible content
    [[nodiscard]] std::string build_summary_prompt(const std::vector<PlanEntry>& plan) const {
        std::string content_to_summarize;
        for (const auto& entry : plan) {
            if (entry.action != "summarize") continue;
            // Extract text content from the message
            const auto& msg = conversation_messages_[entry.message_index];
            std::visit([&content_to_summarize](const auto& m) {
                for (const auto& block : m.content) {
                    if (auto* text = std::get_if<TextBlock>(&block)) {
                        content_to_summarize += text->text + "\n---\n";
                    }
                }
            }, msg);
        }

        return std::format(
            "Summarize the following conversation segments into a concise summary.\n"
            "Preserve: key decisions, code snippets, file paths, requirements, errors.\n"
            "Remove: verbose explanations, repeated information, intermediate steps.\n"
            "Target: reduce to ~30%% of original length.\n\n"
            "Content to summarize:\n{}", content_to_summarize
        );
    }
};

} // namespace cc::commands
