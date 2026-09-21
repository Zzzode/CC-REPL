/// @file stats.cppm
/// @brief StatsCommand implementing the /stats slash command.
/// Shows session statistics: messages, tokens, tool uses, duration.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <chrono>

export module cc.commands.stats;

import cc.types.types;
import cc.commands.command;

export namespace cc::commands {

using namespace cc::core;

/// Session statistics snapshot (populated by the session manager before command execution)
struct SessionStats {
    uint32_t message_count{0};
    uint32_t user_message_count{0};
    uint32_t assistant_message_count{0};
    uint64_t input_tokens{0};
    uint64_t output_tokens{0};
    uint32_t tool_use_count{0};
    std::chrono::seconds session_duration{0};
    std::string session_id;
    std::string model_name;
};

/// StatsCommand implements the /stats slash command.
/// Shows session statistics.
class StatsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "stats",
            .description = "Show session statistics",
            .args = {},
            .category = "session",
            .aliases = {"statistics"},
            .hidden = false,
        };
    }

    /// Set the current session stats (called by session manager before execute)
    void set_stats(const SessionStats& stats) { stats_ = stats; }

    [[nodiscard]] VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext&) {
        auto duration_min = stats_.session_duration.count() / 60;
        auto duration_sec = stats_.session_duration.count() % 60;

        std::string output;
        output += "Session Statistics\n";
        output += "==================\n\n";

        if (!stats_.session_id.empty()) {
            output += std::format("  Session:    {}\n", stats_.session_id);
        }
        if (!stats_.model_name.empty()) {
            output += std::format("  Model:      {}\n", stats_.model_name);
        }
        output += std::format("  Duration:   {}m {}s\n\n", duration_min, duration_sec);

        output += std::format("  Messages:   {} total ({} user, {} assistant)\n",
            stats_.message_count, stats_.user_message_count, stats_.assistant_message_count);
        output += std::format("  Tokens:     {} in / {} out ({} total)\n",
            stats_.input_tokens, stats_.output_tokens,
            stats_.input_tokens + stats_.output_tokens);
        output += std::format("  Tool uses:  {}\n", stats_.tool_use_count);

        if (stats_.input_tokens + stats_.output_tokens > 0 && stats_.session_duration.count() > 0) {
            auto tokens_per_sec = (stats_.input_tokens + stats_.output_tokens) / 
                                  static_cast<uint64_t>(stats_.session_duration.count());
            output += std::format("  Throughput: ~{} tokens/sec\n", tokens_per_sec);
        }

        return CommandResult::success(std::move(output));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view) {
        return {};
    }

private:
    SessionStats stats_;
};

} // namespace cc::commands
