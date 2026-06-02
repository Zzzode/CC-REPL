/// @file away_summary.cppm
/// @brief 离开总结服务 - 生成对话回顾
/// 当用户离开一段时间后返回时，生成简短的对话摘要
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
#include <coroutine>

export module cc.services.away_summary;

import cc.types.types;
import cc.utils.async;
import cc.utils.error;
import cc.utils.json;

export namespace cc::services::away_summary {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================
// 常量定义
// ============================================================

constexpr std::size_t RECENT_MESSAGE_WINDOW = 30;

// ============================================================
// 类型定义
// ============================================================

/// 总结配置
struct SummaryConfig {
    std::size_t recent_window = RECENT_MESSAGE_WINDOW;
    bool include_session_memory = true;
};

// ============================================================
// 离开总结服务
// ============================================================

class AwaySummaryService {
public:
    explicit AwaySummaryService(SummaryConfig config = {})
        : config_(std::move(config)) {}

    ~AwaySummaryService() = default;

    // 禁止拷贝，允许移动
    AwaySummaryService(const AwaySummaryService&) = delete;
    AwaySummaryService& operator=(const AwaySummaryService&) = delete;
    AwaySummaryService(AwaySummaryService&&) noexcept = default;
    AwaySummaryService& operator=(AwaySummaryService&&) noexcept = default;

    /// 生成离开总结
    /// @param messages 对话消息列表
    /// @param session_memory 可选的会话记忆
    /// @return 总结文本
    Task<std::expected<std::string, Error>> generate_summary(
        const std::vector<std::string>& messages,
        const std::optional<std::string>& session_memory = std::nullopt)
    {
        if (messages.empty()) {
            co_return std::unexpected(Error(ErrorCode::invalid_argument, "no messages to summarize"));
        }

        // 截取最近的消息
        auto recent_messages = std::vector<std::string>(
            messages.end() - std::min(messages.size(), config_.recent_window),
            messages.end()
        );

        // 构建提示
        auto prompt = build_summary_prompt(session_memory);

        auto summary = generate_local_summary(recent_messages, prompt);

        co_return summary;
    }

    /// 更新配置
    void set_config(SummaryConfig config) noexcept {
        config_ = std::move(config);
    }

    /// 获取当前配置
    [[nodiscard]] const SummaryConfig& config() const noexcept {
        return config_;
    }

private:
    /// 构建总结提示
    [[nodiscard]] std::string build_summary_prompt(
        const std::optional<std::string>& session_memory) const
    {
        std::string prompt;
        if (session_memory && config_.include_session_memory) {
            prompt += std::format("Session memory (broader context):\n{}\n\n", *session_memory);
        }
        prompt += "The user stepped away and is coming back. Write exactly 1-3 short sentences.\n"
                  "Start by stating the high-level task - what they are building or debugging, "
                  "not implementation details.\n"
                  "Next: the concrete next step.\n"
                  "Skip status reports and commit recaps.";
        return prompt;
    }

    /// 根据最近消息生成本地启发式总结
    [[nodiscard]] std::string generate_local_summary(
        const std::vector<std::string>& recent_messages,
        std::string_view prompt) const
    {
        (void)prompt;
        auto last_non_empty = std::find_if(recent_messages.rbegin(), recent_messages.rend(), [](const auto& msg) {
            return !msg.empty();
        });
        if (last_non_empty == recent_messages.rend()) {
            return "You were reviewing the session context. The next step is to continue from the latest actionable message.";
        }

        auto latest = *last_non_empty;
        if (latest.size() > 180) latest = latest.substr(0, 177) + "...";
        return std::format(
            "You were working through the current CC-REPL task. The latest relevant point was: {} Next, continue from that point and verify the result.",
            latest);
    }

    SummaryConfig config_;
};

// ============================================================
// 便捷函数
// ============================================================

/// 快速生成离开总结
[[nodiscard]] Task<std::expected<std::string, Error>> quick_summary(
    const std::vector<std::string>& messages,
    const std::optional<std::string>& session_memory = std::nullopt)
{
    AwaySummaryService service;
    co_return co_await service.generate_summary(messages, session_memory);
}

} // namespace cc::services::away_summary
