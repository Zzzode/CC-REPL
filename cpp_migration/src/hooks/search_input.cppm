// C++23 Module: Incremental search within conversation/output with regex support
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.search_input;


export namespace cc::hooks {

// 搜索方向
enum class SearchDirection { forward, backward };

// 搜索匹配结果
struct SearchMatch {
    std::size_t line_index{0};     // 匹配所在的行号
    std::size_t col_start{0};      // 匹配起始列
    std::size_t col_end{0};        // 匹配结束列（不含）
    std::string context;           // 匹配所在行的完整文本

    // 匹配文本的长度
    [[nodiscard]] auto length() const -> std::size_t { return col_end - col_start; }

    // 提取匹配的文本片段
    [[nodiscard]] auto matched_text() const -> std::string_view {
        if (col_start >= context.size()) return {};
        return std::string_view(context).substr(col_start, col_end - col_start);
    }
};

// 搜索状态
struct SearchState {
    std::string query;                        // 当前搜索词
    SearchDirection direction{SearchDirection::forward};
    std::vector<SearchMatch> matches;         // 所有匹配结果
    std::size_t current_match_idx{0};         // 当前高亮的匹配索引
    bool is_regex{false};                     // 是否使用正则表达式
    bool case_sensitive{false};               // 是否区分大小写
    bool active{false};                       // 搜索是否处于激活状态
};

// SearchInputHook: 管理增量搜索功能
class SearchInputHook {
public:
    SearchInputHook() = default;

    // 开始搜索（进入搜索模式）
    auto start_search(SearchDirection direction = SearchDirection::forward) -> void {
        state_.active = true;
        state_.direction = direction;
        state_.query.clear();
        state_.matches.clear();
        state_.current_match_idx = 0;
    }

    // 更新搜索词并重新执行搜索
    auto update_query(std::string_view text) -> void {
        if (!state_.active) return;
        state_.query = std::string(text);
        execute_search();
    }

    // 跳转到下一个匹配
    auto next_match() -> void {
        if (state_.matches.empty()) return;
        state_.current_match_idx = (state_.current_match_idx + 1) % state_.matches.size();
    }

    // 跳转到上一个匹配
    auto prev_match() -> void {
        if (state_.matches.empty()) return;
        if (state_.current_match_idx == 0) {
            state_.current_match_idx = state_.matches.size() - 1;
        } else {
            --state_.current_match_idx;
        }
    }

    // 确认当前匹配（退出搜索并定位到该匹配位置）
    auto accept_match() -> std::optional<SearchMatch> {
        if (!state_.active || state_.matches.empty()) {
            cancel();
            return std::nullopt;
        }
        auto match = state_.matches[state_.current_match_idx];
        // 保存到搜索历史
        if (!state_.query.empty()) {
            add_to_history(state_.query);
        }
        state_.active = false;
        return match;
    }

    // 取消搜索
    auto cancel() -> void {
        state_.active = false;
        state_.query.clear();
        state_.matches.clear();
        state_.current_match_idx = 0;
    }

    // 获取所有匹配结果
    [[nodiscard]] auto get_matches() const -> std::span<const SearchMatch> {
        return state_.matches;
    }

    // 获取当前高亮的匹配
    [[nodiscard]] auto get_current_match() const -> std::optional<SearchMatch> {
        if (state_.matches.empty()) return std::nullopt;
        if (state_.current_match_idx >= state_.matches.size()) return std::nullopt;
        return state_.matches[state_.current_match_idx];
    }

    // 切换大小写敏感
    auto toggle_case_sensitivity() -> void {
        state_.case_sensitive = !state_.case_sensitive;
        if (state_.active && !state_.query.empty()) {
            execute_search(); // 重新搜索
        }
    }

    // 切换正则表达式模式
    auto toggle_regex() -> void {
        state_.is_regex = !state_.is_regex;
        if (state_.active && !state_.query.empty()) {
            execute_search();
        }
    }

    // 设置搜索内容源（所有可搜索的行）
    auto set_content(std::vector<std::string> lines) -> void {
        content_lines_ = std::move(lines);
    }

    // 追加内容行（流式场景）
    auto append_content(std::string_view line) -> void {
        content_lines_.emplace_back(line);
    }

    // 获取搜索状态
    [[nodiscard]] auto state() const -> const SearchState& { return state_; }

    // 获取搜索历史
    [[nodiscard]] auto history() const -> std::span<const std::string> { return history_; }

    // 从历史中选择上一个搜索词
    auto prev_history() -> void {
        if (history_.empty()) return;
        if (history_idx_ == 0) return;
        --history_idx_;
        state_.query = history_[history_idx_];
        execute_search();
    }

    // 从历史中选择下一个搜索词
    auto next_history() -> void {
        if (history_.empty()) return;
        if (history_idx_ >= history_.size() - 1) {
            state_.query.clear();
            return;
        }
        ++history_idx_;
        state_.query = history_[history_idx_];
        execute_search();
    }

    // 获取匹配计数的格式化字符串（如 "3/15"）
    [[nodiscard]] auto match_count_display() const -> std::string {
        if (state_.matches.empty()) return "0/0";
        return std::to_string(state_.current_match_idx + 1) + "/" +
               std::to_string(state_.matches.size());
    }

private:
    SearchState state_;
    std::vector<std::string> content_lines_;
    std::vector<std::string> history_;
    std::size_t history_idx_{0};
    static constexpr std::size_t max_history_size_ = 50;

    // 执行搜索逻辑
    auto execute_search() -> void {
        state_.matches.clear();
        state_.current_match_idx = 0;

        if (state_.query.empty()) return;

        if (state_.is_regex) {
            search_regex();
        } else {
            search_literal();
        }

        // 反向搜索时，翻转匹配顺序
        if (state_.direction == SearchDirection::backward && !state_.matches.empty()) {
            std::reverse(state_.matches.begin(), state_.matches.end());
        }
    }

    // 字面量搜索
    auto search_literal() -> void {
        auto query = state_.query;
        // 不区分大小写时转换为小写比较
        auto prepare = [this](std::string_view s) -> std::string {
            if (!state_.case_sensitive) {
                std::string lower(s);
                std::ranges::transform(lower, lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                return lower;
            }
            return std::string(s);
        };

        auto prepared_query = prepare(query);

        for (std::size_t i = 0; i < content_lines_.size(); ++i) {
            auto prepared_line = prepare(content_lines_[i]);
            std::size_t pos = 0;
            while ((pos = prepared_line.find(prepared_query, pos)) != std::string::npos) {
                state_.matches.push_back(SearchMatch{
                    .line_index = i,
                    .col_start = pos,
                    .col_end = pos + prepared_query.size(),
                    .context = content_lines_[i]
                });
                pos += prepared_query.size(); // 避免重叠匹配
            }
        }
    }

    // 正则搜索
    auto search_regex() -> void {
        try {
            auto flags = std::regex::ECMAScript;
            if (!state_.case_sensitive) flags |= std::regex::icase;
            std::regex pattern(state_.query, flags);

            for (std::size_t i = 0; i < content_lines_.size(); ++i) {
                auto begin = std::sregex_iterator(
                    content_lines_[i].begin(), content_lines_[i].end(), pattern);
                auto end = std::sregex_iterator();

                for (auto it = begin; it != end; ++it) {
                    state_.matches.push_back(SearchMatch{
                        .line_index = i,
                        .col_start = static_cast<std::size_t>(it->position()),
                        .col_end = static_cast<std::size_t>(it->position() + it->length()),
                        .context = content_lines_[i]
                    });
                }
            }
        } catch (const std::regex_error&) {
            // 正则语法无效时静默忽略
        }
    }

    // 添加到搜索历史（去重）
    auto add_to_history(const std::string& query) -> void {
        // 移除已存在的相同条目
        std::erase(history_, query);
        history_.push_back(query);
        // 限制历史大小
        if (history_.size() > max_history_size_) {
            history_.erase(history_.begin());
        }
        history_idx_ = history_.size();
    }
};

} // namespace cc::hooks
