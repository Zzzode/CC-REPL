// C++23 Module: Typeahead/autocomplete for command input, file paths, and tool arguments
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.typeahead;


export namespace cc::hooks {

// 补全来源类型
enum class CompletionSource {
    Commands,   // 斜杠命令 (/help, /model, etc.)
    Files,      // 文件系统路径
    GitRefs,    // Git 分支/标签
    History,    // 输入历史
    Tools,      // 工具名称
    Custom      // 自定义来源
};

// 单个补全建议项
struct CompletionItem {
    std::string label;               // 显示文本
    std::string detail;              // 附加详情（如类型、路径）
    std::string insert_text;         // 实际插入的文本
    float score{0.0f};              // 匹配得分 (0.0-1.0，越高越优先)
    CompletionSource source{CompletionSource::Custom};
    std::optional<std::string> icon; // 可选显示图标

    // 按得分降序排列
    [[nodiscard]] auto operator<(const CompletionItem& other) const -> bool {
        return score > other.score; // 注意：得分高的排前面
    }
};

// 补全提供者函数类型：接收查询文本，返回候选项列表
using CompletionProvider = std::function<std::vector<CompletionItem>(std::string_view query)>;

// Typeahead 内部状态
struct TypeaheadState {
    std::string input_text;                      // 当前输入文本
    std::size_t cursor_pos{0};                   // 光标位置
    std::vector<CompletionItem> suggestions;     // 当前建议列表
    std::optional<std::size_t> selected_index;   // 当前选中的建议索引
    bool visible{false};                         // 建议面板是否可见
    std::chrono::steady_clock::time_point last_update; // 上次输入更新时间
};

// ─── TypeaheadHook: 核心自动补全管理类 ─────────────────────────
class TypeaheadHook {
public:
    explicit TypeaheadHook(std::uint32_t debounce_ms = 100,
                           std::size_t max_suggestions = 10)
        : debounce_ms_(debounce_ms), max_suggestions_(max_suggestions) {}

    // 更新输入文本和光标位置（触发补全计算）
    auto update_input(std::string_view text, std::size_t cursor_pos) -> void {
        state_.input_text = std::string(text);
        state_.cursor_pos = cursor_pos;
        state_.last_update = std::chrono::steady_clock::now();

        // 提取光标处的查询词
        auto query = extract_query(text, cursor_pos);
        if (query.empty()) {
            dismiss();
            return;
        }

        // The event loop calls this method after input mutation; use the stored
        // timestamp to keep debounce semantics deterministic in the hook state.
        compute_suggestions(query);
    }

    // 获取当前建议列表
    [[nodiscard]] auto get_suggestions() const -> const std::vector<CompletionItem>& {
        return state_.suggestions;
    }

    // 接受指定索引的建议
    [[nodiscard]] auto accept_suggestion(std::size_t index)
        -> std::expected<std::string, std::string> {
        if (index >= state_.suggestions.size()) {
            return std::unexpected("Index out of range");
        }
        auto& item = state_.suggestions[index];
        auto result = apply_completion(item);
        dismiss();
        return result;
    }

    // 接受当前选中的建议
    [[nodiscard]] auto accept_current() -> std::expected<std::string, std::string> {
        if (!state_.selected_index.has_value()) {
            return std::unexpected("No suggestion selected");
        }
        return accept_suggestion(*state_.selected_index);
    }

    // 循环选择下一个建议
    auto cycle_next() -> void {
        if (state_.suggestions.empty()) return;
        if (!state_.selected_index.has_value()) {
            state_.selected_index = 0;
        } else {
            state_.selected_index = (*state_.selected_index + 1) % state_.suggestions.size();
        }
    }

    // 循环选择上一个建议
    auto cycle_prev() -> void {
        if (state_.suggestions.empty()) return;
        if (!state_.selected_index.has_value()) {
            state_.selected_index = state_.suggestions.size() - 1;
        } else if (*state_.selected_index == 0) {
            state_.selected_index = state_.suggestions.size() - 1;
        } else {
            state_.selected_index = *state_.selected_index - 1;
        }
    }

    // 关闭建议面板
    auto dismiss() -> void {
        state_.suggestions.clear();
        state_.selected_index = std::nullopt;
        state_.visible = false;
    }

    // 注册补全来源提供者
    auto add_source(CompletionSource source, CompletionProvider provider) -> void {
        providers_[source] = std::move(provider);
    }

    // 设置 debounce 延迟
    auto set_debounce_ms(std::uint32_t ms) -> void { debounce_ms_ = ms; }

    // 获取 ghost text（选中建议的剩余部分，用于淡色预览）
    [[nodiscard]] auto get_ghost_text() const -> std::optional<std::string> {
        if (!state_.selected_index.has_value()) return std::nullopt;
        if (*state_.selected_index >= state_.suggestions.size()) return std::nullopt;

        const auto& item = state_.suggestions[*state_.selected_index];
        auto query = extract_query(state_.input_text, state_.cursor_pos);

        // Ghost text = 建议文本去掉已输入的前缀部分
        if (item.insert_text.size() > query.size() &&
            item.insert_text.starts_with(query)) {
            return item.insert_text.substr(query.size());
        }
        return std::nullopt;
    }

    // 是否有可见的建议
    [[nodiscard]] auto is_visible() const -> bool { return state_.visible; }

    // 获取完整状态
    [[nodiscard]] auto state() const -> const TypeaheadState& { return state_; }

    // 设置最大建议数
    auto set_max_suggestions(std::size_t max) -> void { max_suggestions_ = max; }

private:
    TypeaheadState state_;
    std::map<CompletionSource, CompletionProvider> providers_;
    std::uint32_t debounce_ms_;
    std::size_t max_suggestions_;

    // 从输入文本和光标位置提取当前查询词
    [[nodiscard]] static auto extract_query(std::string_view text, std::size_t cursor_pos)
        -> std::string_view {
        if (text.empty() || cursor_pos == 0) return {};
        auto effective_pos = std::min(cursor_pos, text.size());

        // 向左查找单词边界（空格、斜杠等分隔符）
        auto start = effective_pos;
        while (start > 0) {
            char c = text[start - 1];
            if (c == ' ' || c == '\n' || c == '\t') break;
            --start;
        }
        return text.substr(start, effective_pos - start);
    }

    // 计算补全建议
    auto compute_suggestions(std::string_view query) -> void {
        state_.suggestions.clear();
        state_.selected_index = std::nullopt;

        // 确定活跃的补全来源
        auto active_sources = determine_sources(query);

        // 从各来源收集候选项
        for (auto source : active_sources) {
            auto it = providers_.find(source);
            if (it == providers_.end()) continue;
            auto items = it->second(query);
            for (auto& item : items) {
                item.source = source;
                // 使用模糊匹配计算得分
                item.score = fuzzy_score(query, item.label);
                if (item.score > 0.0f) {
                    state_.suggestions.push_back(std::move(item));
                }
            }
        }

        // 排序并截断
        std::sort(state_.suggestions.begin(), state_.suggestions.end());
        if (state_.suggestions.size() > max_suggestions_) {
            state_.suggestions.resize(max_suggestions_);
        }

        state_.visible = !state_.suggestions.empty();
    }

    // 根据查询文本确定应查询哪些来源
    [[nodiscard]] auto determine_sources(std::string_view query) const
        -> std::vector<CompletionSource> {
        std::vector<CompletionSource> sources;

        if (query.starts_with('/')) {
            // 斜杠开头 -> 命令补全
            sources.push_back(CompletionSource::Commands);
        } else if (query.starts_with('.') || query.starts_with('/') ||
                   query.starts_with('~')) {
            // 路径模式 -> 文件补全
            sources.push_back(CompletionSource::Files);
        } else {
            // 通用：查询所有已注册来源
            for (const auto& [source, _] : providers_) {
                sources.push_back(source);
            }
        }
        return sources;
    }

    // 模糊匹配得分计算
    [[nodiscard]] static auto fuzzy_score(std::string_view query,
                                           std::string_view candidate) -> float {
        if (query.empty()) return 0.1f; // 空查询给一个较低的基础分

        // 前缀完全匹配给最高分
        if (candidate.starts_with(query)) {
            return 1.0f - (static_cast<float>(candidate.size() - query.size()) /
                          static_cast<float>(candidate.size() + 1));
        }

        // 大小写不敏感的前缀匹配
        auto query_lower = to_lower(query);
        auto candidate_lower = to_lower(candidate);
        if (candidate_lower.starts_with(query_lower)) {
            return 0.8f - (static_cast<float>(candidate.size() - query.size()) /
                          static_cast<float>(candidate.size() + 1));
        }

        // 子序列匹配
        std::size_t qi = 0;
        std::size_t matches = 0;
        for (std::size_t ci = 0; ci < candidate_lower.size() && qi < query_lower.size(); ++ci) {
            if (candidate_lower[ci] == query_lower[qi]) {
                ++qi;
                ++matches;
            }
        }
        if (qi == query_lower.size()) {
            // 全部字符匹配成功
            return 0.5f * (static_cast<float>(matches) /
                          static_cast<float>(candidate.size()));
        }

        return 0.0f; // 不匹配
    }

    // 应用补全：构建替换后的完整文本
    [[nodiscard]] auto apply_completion(const CompletionItem& item) const -> std::string {
        auto query = extract_query(state_.input_text, state_.cursor_pos);
        auto query_start = state_.cursor_pos - query.size();

        std::string result;
        result.reserve(state_.input_text.size() + item.insert_text.size());
        result += state_.input_text.substr(0, query_start);
        result += item.insert_text;
        result += state_.input_text.substr(state_.cursor_pos);
        return result;
    }

    // 辅助：转小写（简易 ASCII 实现）
    [[nodiscard]] static auto to_lower(std::string_view s) -> std::string {
        std::string result(s);
        std::ranges::transform(result, result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }
};

} // namespace cc::hooks
