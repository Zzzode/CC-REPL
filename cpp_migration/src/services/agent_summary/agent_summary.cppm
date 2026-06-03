/// @file agent_summary.cppm
/// @brief Agent execution summary service.
/// Generates structured summaries of agent execution including tools used,
/// files modified, time taken, with token-budget-aware truncation.
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
#include <numeric>

export module cc.services.agent_summary;

import cc.types.types;

export namespace cc::services::agent_summary {

using cc::core::Error;
using cc::core::ErrorCode;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;


enum class SummaryFormat : std::uint8_t {
    PlainText,
    Structured,
    Markdown,
};


struct ToolInvocation {
    std::string tool_name;
    std::string input_summary;
    Duration elapsed{0};
    bool success{true};
    std::string error_message;
};


struct FileModification {
    std::string path;
    std::size_t lines_added{0};
    std::size_t lines_removed{0};
    TimePoint modified_at;
};


struct ExecutionSummary {
    std::string session_id;
    TimePoint started_at;
    TimePoint ended_at;
    Duration total_elapsed{0};
    std::vector<ToolInvocation> tools_used;
    std::vector<FileModification> files_modified;
    std::size_t total_tokens_consumed{0};
    std::size_t turns_count{0};


    [[nodiscard]] double success_rate() const noexcept {
        if (tools_used.empty()) return 1.0;
        auto successes = std::ranges::count_if(tools_used,
            [](const auto& t) { return t.success; });
        return static_cast<double>(successes) / static_cast<double>(tools_used.size());
    }


    [[nodiscard]] double elapsed_seconds() const noexcept {
        return static_cast<double>(total_elapsed.count()) / 1000.0;
    }
};


struct TokenBudget {
    std::size_t max_tokens{4096};
    std::size_t reserved_tokens{256};

    [[nodiscard]] std::size_t available() const noexcept {
        return max_tokens > reserved_tokens ? max_tokens - reserved_tokens : 0;
    }
};

// ============================================================

// ============================================================

class AgentSummaryService {
public:
    explicit AgentSummaryService(TokenBudget budget = {})
        : budget_(budget) {}


    [[nodiscard]] std::expected<std::string, Error> generate(
        const ExecutionSummary& summary,
        SummaryFormat format = SummaryFormat::PlainText) const
    {
        switch (format) {
            case SummaryFormat::PlainText:   return generate_text(summary);
            case SummaryFormat::Structured:  return generate_structured(summary);
            case SummaryFormat::Markdown:    return generate_markdown(summary);
        }
        return std::unexpected(Error{ErrorCode::InvalidInput, {}, "unknown format"});
    }


    void set_budget(TokenBudget budget) noexcept { budget_ = budget; }


    [[nodiscard]] TokenBudget budget() const noexcept { return budget_; }

private:
    TokenBudget budget_;


    [[nodiscard]] std::string truncate_to_budget(std::string text) const {

        std::size_t max_chars = budget_.available() * 4;
        if (text.size() <= max_chars) return text;
        text.resize(max_chars);
        text += "\n... [truncated due to token budget]";
        return text;
    }

    [[nodiscard]] std::expected<std::string, Error> generate_text(
        const ExecutionSummary& summary) const
    {
        auto result = std::format(
            "Session: {}\nDuration: {:.1f}s | Tools: {} | Files: {} | Tokens: {}\nSuccess rate: {:.0f}%",
            summary.session_id, summary.elapsed_seconds(),
            summary.tools_used.size(), summary.files_modified.size(),
            summary.total_tokens_consumed, summary.success_rate() * 100.0);
        return truncate_to_budget(std::move(result));
    }

    [[nodiscard]] std::expected<std::string, Error> generate_structured(
        const ExecutionSummary& summary) const
    {
        auto result = std::format(
            R"({{"session":"{}","duration_ms":{},"tools_count":{},"files_count":{},"tokens":{},"success_rate":{:.2f}}})",
            summary.session_id, summary.total_elapsed.count(),
            summary.tools_used.size(), summary.files_modified.size(),
            summary.total_tokens_consumed, summary.success_rate());
        return truncate_to_budget(std::move(result));
    }

    [[nodiscard]] std::expected<std::string, Error> generate_markdown(
        const ExecutionSummary& summary) const
    {
        auto result = std::format(
            "## Agent Summary\n- **Duration**: {:.1f}s\n- **Tools used**: {}\n- **Files modified**: {}\n- **Success rate**: {:.0f}%\n",
            summary.elapsed_seconds(), summary.tools_used.size(),
            summary.files_modified.size(), summary.success_rate() * 100.0);
        return truncate_to_budget(std::move(result));
    }
};

} // namespace cc::services::agent_summary
