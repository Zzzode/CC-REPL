/// @file prompt_suggestion.cppm
/// @brief Prompt suggestion service.
/// Suggests follow-up prompts, commands, file/directory paths, and speculative
/// predictions based on conversation context, shell history, and project state.
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
#include <functional>
#include <deque>

export module cc.services.prompt_suggestion;

import cc.types.types;

export namespace cc::services::prompt_suggestion {

using cc::core::Error;
using cc::core::ErrorCode;

// ============================================================

// ============================================================


enum class SuggestionSource : std::uint8_t {
    ConversationContext,
    CommandHistory,
    FileSystem,
    ShellHistory,
    Speculative,
};


enum class SuggestionPriority : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Critical = 3,
};


struct Suggestion {
    std::string text;
    std::string description;
    SuggestionSource source;
    SuggestionPriority priority{SuggestionPriority::Medium};
    double confidence{0.5};
    std::vector<std::string> tags;

    auto operator<=>(const Suggestion& other) const noexcept {
        return other.confidence <=> confidence;
    }
    bool operator==(const Suggestion& other) const noexcept {
        return confidence == other.confidence;
    }
};


struct ConversationTurn {
    std::string role;      // user / assistant
    std::string content;
    std::chrono::system_clock::time_point timestamp;
};


struct ShellHistoryEntry {
    std::string command;
    std::string working_dir;
    int exit_code{0};
    std::chrono::system_clock::time_point executed_at;
};


struct SuggestionRequest {
    std::vector<ConversationTurn> recent_turns;
    std::string current_directory;
    std::vector<std::string> open_files;
    std::size_t max_suggestions{5};
    bool include_speculative{true};
};

// ============================================================

// ============================================================

class PromptSuggestionService {
public:
    PromptSuggestionService() = default;


    [[nodiscard]] std::expected<std::vector<Suggestion>, Error> suggest(
        const SuggestionRequest& request) const
    {
        std::vector<Suggestion> results;

        collect_context_suggestions(request, results);
        collect_file_suggestions(request, results);
        if (request.include_speculative) {
            collect_speculative_suggestions(request, results);
        }
        collect_shell_suggestions(results);


        std::ranges::sort(results);
        if (results.size() > request.max_suggestions) {
            results.resize(request.max_suggestions);
        }
        return results;
    }


    void add_shell_history(ShellHistoryEntry entry) {
        if (shell_history_.size() >= max_history_size_) {
            shell_history_.pop_front();
        }
        shell_history_.push_back(std::move(entry));
    }


    using SuggestionGenerator = std::function<std::vector<Suggestion>(const SuggestionRequest&)>;
    void register_generator(std::string name, SuggestionGenerator gen) {
        custom_generators_[std::move(name)] = std::move(gen);
    }


    void clear_history() noexcept { shell_history_.clear(); }


    void set_max_history(std::size_t n) noexcept { max_history_size_ = n; }

private:
    std::deque<ShellHistoryEntry> shell_history_;
    std::size_t max_history_size_{500};
    std::unordered_map<std::string, SuggestionGenerator> custom_generators_;


    void collect_context_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        if (request.recent_turns.empty()) return;
        const auto& last = request.recent_turns.back();

        if (last.role == "assistant") {
            out.push_back({
                .text = "Can you explain that in more detail?",
                .description = "Request elaboration",
                .source = SuggestionSource::ConversationContext,
                .priority = SuggestionPriority::Medium,
                .confidence = 0.6,
            });
            out.push_back({
                .text = "What are the potential issues with this approach?",
                .description = "Ask about risks",
                .source = SuggestionSource::ConversationContext,
                .priority = SuggestionPriority::Medium,
                .confidence = 0.5,
            });
        }
    }


    void collect_file_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {

        for (const auto& file : request.open_files | std::views::take(3)) {
            out.push_back({
                .text = std::format("Review {}", file),
                .description = std::format("Review open file: {}", file),
                .source = SuggestionSource::FileSystem,
                .priority = SuggestionPriority::Low,
                .confidence = 0.4,
            });
        }
    }


    void collect_speculative_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        if (request.recent_turns.size() < 2) return;

        out.push_back({
            .text = "Run the tests to verify the changes",
            .description = "Verify recent modifications",
            .source = SuggestionSource::Speculative,
            .priority = SuggestionPriority::High,
            .confidence = 0.7,
        });
    }


    void collect_shell_suggestions(std::vector<Suggestion>& out) const {
        if (shell_history_.empty()) return;
        const auto& last_cmd = shell_history_.back();
        if (last_cmd.exit_code != 0) {

            out.push_back({
                .text = std::format("Fix the failing command: {}", last_cmd.command),
                .description = "Previous command failed",
                .source = SuggestionSource::ShellHistory,
                .priority = SuggestionPriority::High,
                .confidence = 0.8,
            });
        }
    }
};

} // namespace cc::services::prompt_suggestion
