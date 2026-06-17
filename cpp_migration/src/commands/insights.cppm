/// @file insights.cppm
/// @brief InsightsCommand implementing the /insights slash command.
/// Scans local session storage and reports usage statistics (session count,
/// message totals, model distribution, most recent sessions). Deep LLM facet
/// analysis from the TS CLI requires the Claude API and is reported as a
/// follow-up note rather than performed inline.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <map>
#include <utility>

export module cc.commands.insights;

import cc.types.types;
import cc.commands.command;
import cc.utils.list_sessions;

export namespace cc::commands {

using namespace cc::core;

/// InsightsCommand implements the /insights slash command.
/// Reports local usage statistics computed from session storage. The TS CLI
/// additionally runs LLM facet extraction (goal/outcome/satisfaction) over
/// each session and renders an HTML report; that step needs the Claude API
/// and is not performed in this build.
class InsightsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "insights",
            .description = "Report local usage statistics from session storage",
            .args = {},
            .category = "tools",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        auto sessions = cc::utils::list_sessions(std::nullopt);

        if (sessions.empty()) {
            return CommandResult::success(
                "No sessions found in local storage (~/.claude/sessions/).\n"
                "Usage insights are generated from recorded session transcripts.");
        }

        std::size_t total_messages = 0;
        std::size_t with_model = 0;
        std::map<std::string, std::size_t> by_model;
        for (const auto& s : sessions) {
            total_messages += s.message_count;
            if (!s.model.empty()) {
                by_model[s.model] += 1;
                ++with_model;
            }
        }

        std::string out = "Local usage insights:\n";
        out += std::format("  Total sessions: {}\n", sessions.size());
        out += std::format("  Total messages: {}\n", total_messages);
        if (total_messages > 0) {
            out += std::format("  Avg messages/session: {:.1}\n",
                               static_cast<double>(total_messages) / static_cast<double>(sessions.size()));
        }
        out += std::format("  Sessions with a recorded model: {}\n", with_model);

        if (!by_model.empty()) {
            out += "\n  Sessions by model:\n";
            std::vector<std::pair<std::string, std::size_t>> sorted(by_model.begin(), by_model.end());
            std::ranges::sort(sorted, [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            for (const auto& [model, count] : sorted) {
                out += std::format("    {:<30} {}\n", model, count);
            }
        }

        out += "\n  Most recent sessions:\n";
        std::size_t shown = 0;
        for (const auto& s : sessions) {
            if (shown++ >= 5) break;
            out += std::format("    {} ({} messages", s.id, s.message_count);
            if (!s.model.empty()) out += std::format(", {}", s.model);
            out += ")\n";
        }

        out += "\nNote: the TS CLI additionally runs LLM facet extraction "
               "(goal/outcome/satisfaction) per session and renders an HTML "
               "report. That step needs the Claude API and is not performed in "
               "this build; ask the assistant to summarize the stats above.";
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
