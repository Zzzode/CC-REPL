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
// 建议类型与数据结构
// ============================================================

// 建议来源分类
enum class SuggestionSource : std::uint8_t {
    ConversationContext,  // 基于对话上下文
    CommandHistory,       // 基于命令历史
    FileSystem,           // 基于文件系统结构
    ShellHistory,         // 基于 shell 历史
    Speculative,          // 推测性建议
};

// 建议优先级
enum class SuggestionPriority : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Critical = 3,
};

// 单条建议
struct Suggestion {
    std::string text;                     // 建议的 prompt 文本
    std::string description;              // 简短描述
    SuggestionSource source;              // 来源
    SuggestionPriority priority{SuggestionPriority::Medium};
    double confidence{0.5};               // 置信度 [0, 1]
    std::vector<std::string> tags;        // 标签

    auto operator<=>(const Suggestion& other) const noexcept {
        return other.confidence <=> confidence;  // 降序排列
    }
    bool operator==(const Suggestion& other) const noexcept {
        return confidence == other.confidence;
    }
};

// 对话上下文片段
struct ConversationTurn {
    std::string role;      // user / assistant
    std::string content;
    std::chrono::system_clock::time_point timestamp;
};

// Shell 历史条目
struct ShellHistoryEntry {
    std::string command;
    std::string working_dir;
    int exit_code{0};
    std::chrono::system_clock::time_point executed_at;
};

// 建议请求配置
struct SuggestionRequest {
    std::vector<ConversationTurn> recent_turns;  // 最近对话轮次
    std::string current_directory;
    std::vector<std::string> open_files;
    std::size_t max_suggestions{5};
    bool include_speculative{true};
};

// ============================================================
// PromptSuggestionService - 生成建议
// ============================================================

class PromptSuggestionService {
public:
    PromptSuggestionService() = default;

    // 根据上下文生成建议列表
    [[nodiscard]] std::expected<std::vector<Suggestion>, Error> suggest(
        const SuggestionRequest& request) const
    {
        std::vector<Suggestion> results;
        // 从不同来源收集建议
        collect_context_suggestions(request, results);
        collect_file_suggestions(request, results);
        if (request.include_speculative) {
            collect_speculative_suggestions(request, results);
        }
        collect_shell_suggestions(results);

        // 按置信度排序并截断
        std::ranges::sort(results);
        if (results.size() > request.max_suggestions) {
            results.resize(request.max_suggestions);
        }
        return results;
    }

    // 注入 shell 历史
    void add_shell_history(ShellHistoryEntry entry) {
        if (shell_history_.size() >= max_history_size_) {
            shell_history_.pop_front();
        }
        shell_history_.push_back(std::move(entry));
    }

    // 注册自定义建议生成器
    using SuggestionGenerator = std::function<std::vector<Suggestion>(const SuggestionRequest&)>;
    void register_generator(std::string name, SuggestionGenerator gen) {
        custom_generators_[std::move(name)] = std::move(gen);
    }

    // 清空历史
    void clear_history() noexcept { shell_history_.clear(); }

    // 设置历史容量
    void set_max_history(std::size_t n) noexcept { max_history_size_ = n; }

private:
    std::deque<ShellHistoryEntry> shell_history_;
    std::size_t max_history_size_{500};
    std::unordered_map<std::string, SuggestionGenerator> custom_generators_;

    // 基于对话上下文生成建议
    void collect_context_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        if (request.recent_turns.empty()) return;
        const auto& last = request.recent_turns.back();
        // 如果最后一轮是 assistant，建议常见后续操作
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

    // 基于文件系统生成建议
    void collect_file_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        // 如果有打开的文件，建议相关操作
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

    // 推测性建议
    void collect_speculative_suggestions(
        const SuggestionRequest& request,
        std::vector<Suggestion>& out) const
    {
        if (request.recent_turns.size() < 2) return;
        // 如果对话涉及代码修改，推测下一步可能是测试
        out.push_back({
            .text = "Run the tests to verify the changes",
            .description = "Verify recent modifications",
            .source = SuggestionSource::Speculative,
            .priority = SuggestionPriority::High,
            .confidence = 0.7,
        });
    }

    // 基于 shell 历史生成建议
    void collect_shell_suggestions(std::vector<Suggestion>& out) const {
        if (shell_history_.empty()) return;
        const auto& last_cmd = shell_history_.back();
        if (last_cmd.exit_code != 0) {
            // 上次命令失败，建议修复
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
