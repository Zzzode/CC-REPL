/// @file session_memory.cppm
/// @brief 管理每会话级别的记忆，跨 compaction 持久化。
/// 支持会话级事实存储、compact 后关键信息携带、prompt 注入和会话结束时的记忆整合。
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
#include <mutex>
#include <coroutine>

export module cc.services.session_memory;

import cc.types.types;
import cc.utils.async;
import cc.utils.error;
import cc.utils.json;

export namespace cc::services::session_memory {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;
using cc::utils::async::Task;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================
// 核心数据结构
// ============================================================

/// 会话级事实条目
struct SessionFact {
    std::string id;                // 唯一标识
    std::string content;           // 事实内容
    double importance;             // 重要性权重 [0.0, 1.0]
    TimePoint created_at;          // 创建时间
    TimePoint last_referenced;     // 最后被引用时间

    /// 事实是否过期 (超过 max_age 未被引用)
    [[nodiscard]] bool is_stale(std::chrono::seconds max_age) const noexcept {
        auto age = Clock::now() - last_referenced;
        return age > max_age;
    }
};

/// 会话记忆完整状态
struct SessionMemoryState {
    std::vector<SessionFact> facts;     // 所有事实
    std::string session_id;             // 当前会话 ID
    std::uint32_t compaction_count{0};  // 已执行 compact 次数
};

// ============================================================
// 会话记忆管理器
// ============================================================

/// 管理单个会话的上下文记忆，确保 compact 后关键信息不丢失
class SessionMemory {
public:
    explicit SessionMemory(std::string session_id)
        : state_{.facts = {}, .session_id = std::move(session_id), .compaction_count = 0}
        , next_id_(1) {}

    ~SessionMemory() = default;

    // 禁止拷贝，允许移动
    SessionMemory(const SessionMemory&) = delete;
    SessionMemory& operator=(const SessionMemory&) = delete;
    SessionMemory(SessionMemory&&) noexcept = default;
    SessionMemory& operator=(SessionMemory&&) noexcept = default;

    /// 添加一条新事实到会话记忆
    /// @param content 事实内容
    /// @param importance 重要性 [0.0, 1.0]
    /// @return 新事实的唯一 ID
    [[nodiscard]] std::string add_fact(std::string_view content, double importance = 0.5) {
        auto id = generate_id();
        auto now = Clock::now();

        state_.facts.push_back(SessionFact{
            .id = id,
            .content = std::string(content),
            .importance = std::clamp(importance, 0.0, 1.0),
            .created_at = now,
            .last_referenced = now,
        });

        return id;
    }

    /// 获取所有当前事实（按重要性降序排列）
    [[nodiscard]] std::vector<SessionFact> get_facts() const {
        auto sorted = state_.facts;
        std::ranges::sort(sorted, [](const auto& a, const auto& b) {
            return a.importance > b.importance;
        });
        return sorted;
    }

    /// 生成 prompt 注入文本，用于在 compact 后恢复上下文
    [[nodiscard]] std::string get_prompt_injection() const {
        if (state_.facts.empty()) return "";

        std::string injection = "<session_memory>\n";
        injection += std::format("Session: {} (compacted {} times)\n",
            state_.session_id, state_.compaction_count);
        injection += "Key facts from this session:\n";

        // 按重要性排序后注入
        auto sorted_facts = get_facts();
        for (const auto& fact : sorted_facts) {
            injection += std::format("- [importance={:.1f}] {}\n",
                fact.importance, fact.content);
        }
        injection += "</session_memory>\n";
        return injection;
    }

    /// 处理 compact 事件，保留指定的关键事实
    /// @param retained_fact_ids 要保留的事实 ID 列表 (空 = 自动选择高重要性的)
    void on_compact(const std::vector<std::string>& retained_fact_ids = {}) {
        state_.compaction_count++;

        if (retained_fact_ids.empty()) {
            // 自动保留重要性 >= 0.6 的事实
            std::erase_if(state_.facts, [](const SessionFact& f) {
                return f.importance < 0.6;
            });
        } else {
            // 仅保留明确指定的事实
            std::erase_if(state_.facts, [&](const SessionFact& f) {
                return std::ranges::find(retained_fact_ids, f.id) == retained_fact_ids.end();
            });
        }
    }

    /// 会话结束时整合记忆（提取长期记忆候选）
    Task<void> consolidate() {
        merge_related_facts();
        std::erase_if(state_.facts, [](const SessionFact& fact) {
            return fact.importance < 0.2 || fact.content.empty();
        });
        co_return;
    }

    /// 清除超过最大存活时间且未被引用的陈旧事实
    void prune_stale(std::chrono::seconds max_age) {
        std::erase_if(state_.facts, [max_age](const SessionFact& f) {
            return f.is_stale(max_age);
        });
    }

    /// 序列化当前状态为 JSON
    [[nodiscard]] std::string serialize() const {
        std::string json = std::format(
            R"({{"session_id":"{}","compaction_count":{},"facts":[)",
            state_.session_id, state_.compaction_count);

        for (std::size_t i = 0; i < state_.facts.size(); ++i) {
            if (i > 0) json += ",";
            const auto& f = state_.facts[i];
            json += std::format(
                R"({{"id":"{}","content":"{}","importance":{},"created_at":{},"last_referenced":{}}})",
                f.id, f.content, f.importance,
                std::chrono::duration_cast<std::chrono::seconds>(f.created_at.time_since_epoch()).count(),
                std::chrono::duration_cast<std::chrono::seconds>(f.last_referenced.time_since_epoch()).count()
            );
        }
        json += "]}";
        return json;
    }

    /// 从 JSON 反序列化恢复状态
    [[nodiscard]] static std::expected<SessionMemoryState, Error> deserialize(std::string_view json) {
        SessionMemoryState state;
        if (json.empty()) {
            return std::unexpected(Error(ErrorCode::parse_error, "Empty JSON input"));
        }
        if (json.front() != '{') {
            return std::unexpected(Error(ErrorCode::parse_error, "Invalid JSON format"));
        }

        auto extract_string = [&](std::string_view key) -> std::string {
            auto marker = std::string{"\""} + std::string{key} + "\":\"";
            auto pos = json.find(marker);
            if (pos == std::string_view::npos) return {};
            pos += marker.size();
            auto end = json.find('"', pos);
            if (end == std::string_view::npos) return {};
            return std::string{json.substr(pos, end - pos)};
        };
        auto extract_uint = [&](std::string_view key) -> std::uint32_t {
            auto marker = std::string{"\""} + std::string{key} + "\":";
            auto pos = json.find(marker);
            if (pos == std::string_view::npos) return 0;
            pos += marker.size();
            auto end = json.find_first_not_of("0123456789", pos);
            auto digits = json.substr(pos, end == std::string_view::npos ? json.size() - pos : end - pos);
            if (digits.empty()) return 0;
            return static_cast<std::uint32_t>(std::stoul(std::string{digits}));
        };

        state.session_id = extract_string("session_id");
        state.compaction_count = extract_uint("compaction_count");
        return state;
    }

    /// 标记某个事实被引用（更新 last_referenced 时间戳）
    void touch_fact(std::string_view fact_id) {
        auto it = std::ranges::find_if(state_.facts, [fact_id](const auto& f) {
            return f.id == fact_id;
        });
        if (it != state_.facts.end()) {
            it->last_referenced = Clock::now();
        }
    }

    /// 获取当前会话 ID
    [[nodiscard]] std::string_view session_id() const noexcept { return state_.session_id; }

    /// 获取事实总数
    [[nodiscard]] std::size_t fact_count() const noexcept { return state_.facts.size(); }

private:
    /// 生成唯一事实 ID
    [[nodiscard]] std::string generate_id() {
        return std::format("sf_{}_{}",
            state_.session_id.substr(0, 8), next_id_++);
    }

    /// 合并语义相关的事实
    void merge_related_facts() {
        std::ranges::sort(state_.facts, [](const auto& a, const auto& b) {
            return a.content < b.content;
        });
        auto [first, last] = std::ranges::unique(state_.facts, [](const auto& a, const auto& b) {
            return a.content == b.content;
        });
        state_.facts.erase(first, last);
    }

    SessionMemoryState state_;
    std::uint64_t next_id_;
};

} // namespace cc::services::session_memory
