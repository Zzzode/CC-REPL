/// @file usage.cppm
/// @brief UsageCommand implementing the /usage slash command.
/// Show token usage, cost breakdown, session duration, and rate limit status.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <chrono>

export module cc.commands.usage;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Rate limit status information
struct RateLimitInfo {
    std::uint32_t requests_remaining = 0;
    std::uint32_t tokens_remaining = 0;
    std::chrono::seconds reset_in{0};
};

/// Tracks accumulated usage for a session
struct SessionUsage {
    TokenUsage total_tokens{};
    std::uint32_t request_count = 0;
    double total_cost_usd = 0.0;
    std::chrono::system_clock::time_point session_start{};
    std::optional<RateLimitInfo> rate_limit{};
};

/// UsageCommand implements the /usage slash command.
/// Displays token usage, costs, session duration, and rate limit info.
class UsageCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "usage",
            .description = "Show token usage, cost, and rate limit status",
            .args = {
                CommandArg{.name = "section", .description = "tokens | cost | time | limits | all",
                           .type = ArgType::Choice, .required = false,
                           .choices = {"tokens", "cost", "time", "limits", "all"}},
            },
            .category = "session",
            .aliases = {"tokens"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};
        auto section = ctx.args[0];
        static constexpr std::array valid = {"tokens", "cost", "time", "limits", "all"};
        if (std::ranges::find(valid, section) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid section: '{}'. Use: tokens|cost|time|limits|all", section)));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        auto section = ctx.args.empty() ? std::string("all") : std::string(ctx.args[0]);
        std::string output;

        if (section == "all" || section == "tokens") {
            output += format_token_usage();
        }
        if (section == "all" || section == "cost") {
            if (!output.empty()) output += "\n";
            output += format_cost();
        }
        if (section == "all" || section == "time") {
            if (!output.empty()) output += "\n";
            output += format_duration();
        }
        if (section == "all" || section == "limits") {
            if (!output.empty()) output += "\n";
            output += format_rate_limits();
        }

        return CommandResult::success(std::move(output));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"tokens", "cost", "time", "limits", "all"}) {
            if (std::string_view(s).starts_with(partial)) {
                suggestions.emplace_back(s);
            }
        }
        return suggestions;
    }

    /// Update usage data (called by the engine after each request)
    void record_request(const TokenUsage& tokens, double cost) {
        usage_.total_tokens += tokens;
        usage_.request_count++;
        usage_.total_cost_usd += cost;
    }

    void set_rate_limit(RateLimitInfo info) { usage_.rate_limit = info; }

private:
    SessionUsage usage_{.session_start = std::chrono::system_clock::now()};

    [[nodiscard]] std::string format_token_usage() const {
        return std::format(
            "Token Usage:\n"
            "  Input:          {:>8} tokens\n"
            "  Output:         {:>8} tokens\n"
            "  Cache creation: {:>8} tokens\n"
            "  Cache read:     {:>8} tokens\n"
            "  Total:          {:>8} tokens\n"
            "  Requests:       {:>8}",
            usage_.total_tokens.input_tokens,
            usage_.total_tokens.output_tokens,
            usage_.total_tokens.cache_creation_tokens,
            usage_.total_tokens.cache_read_tokens,
            usage_.total_tokens.total(),
            usage_.request_count);
    }

    [[nodiscard]] std::string format_cost() const {
        return std::format("Cost: ${:.4f} USD", usage_.total_cost_usd);
    }

    [[nodiscard]] std::string format_duration() const {
        auto elapsed = std::chrono::system_clock::now() - usage_.session_start;
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed);
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed) - minutes;
        return std::format("Session duration: {}m {}s", minutes.count(), seconds.count());
    }

    [[nodiscard]] std::string format_rate_limits() const {
        if (!usage_.rate_limit) {
            return "Rate limits: No data available";
        }
        auto& rl = *usage_.rate_limit;
        return std::format(
            "Rate Limits:\n"
            "  Requests remaining: {}\n"
            "  Tokens remaining:   {}\n"
            "  Resets in:          {}s",
            rl.requests_remaining, rl.tokens_remaining, rl.reset_in.count());
    }
};

} // namespace cc::commands
