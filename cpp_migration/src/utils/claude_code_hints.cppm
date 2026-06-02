module;

#include <cctype>
#include <climits>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.utils.claude_code_hints;

export namespace cc::utils::claude_code_hints {

struct ClaudeCodeHint {
    int v{};
    std::string type;
    std::string value;
    std::string source_command;
};

struct ExtractedClaudeCodeHints {
    std::vector<ClaudeCodeHint> hints;
    std::string stripped;
};

namespace detail {
    [[nodiscard]] inline bool is_space_or_tab(char ch) noexcept {
        return ch == ' ' || ch == '\t';
    }

    [[nodiscard]] inline std::string_view trim_horizontal(std::string_view value) noexcept {
        while (!value.empty() && is_space_or_tab(value.front())) value.remove_prefix(1);
        while (!value.empty() && is_space_or_tab(value.back())) value.remove_suffix(1);
        return value;
    }

    [[nodiscard]] inline bool starts_with(std::string_view value, std::string_view prefix) noexcept {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    [[nodiscard]] inline bool ends_with(std::string_view value, std::string_view suffix) noexcept {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    [[nodiscard]] inline bool is_word_char(unsigned char ch) noexcept {
        return std::isalnum(ch) != 0 || ch == '_';
    }

    [[nodiscard]] inline bool is_hint_tag_line(std::string_view raw_line) noexcept {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.remove_suffix(1);
        const auto line = trim_horizontal(raw_line);
        constexpr std::string_view prefix = "<claude-code-hint";
        if (!starts_with(line, prefix) || !ends_with(line, "/>")) return false;
        if (line.size() == prefix.size() + 2) return true;
        const char after_prefix = line[prefix.size()];
        return is_space_or_tab(after_prefix) || after_prefix == '/' || after_prefix == '>';
    }

    [[nodiscard]] inline std::optional<int> parse_int(std::string_view value) noexcept {
        if (value.empty()) return std::nullopt;
        int out = 0;
        for (unsigned char ch : value) {
            if (ch < '0' || ch > '9') return std::nullopt;
            const int digit = static_cast<int>(ch - '0');
            if (out > (INT_MAX - digit) / 10) return std::nullopt;
            out = out * 10 + digit;
        }
        return out;
    }

    [[nodiscard]] inline std::string collapse_extra_blank_lines(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        std::size_t newline_run = 0;
        for (char ch : value) {
            if (ch == '\n') {
                ++newline_run;
                if (newline_run <= 2) out.push_back(ch);
            } else {
                newline_run = 0;
                out.push_back(ch);
            }
        }
        return out;
    }
} // namespace detail

[[nodiscard]] inline std::unordered_map<std::string, std::string> parse_attrs(std::string_view tag_body) {
    std::unordered_map<std::string, std::string> attrs;
    std::size_t i = 0;
    while (i < tag_body.size()) {
        if (!detail::is_word_char(static_cast<unsigned char>(tag_body[i]))) {
            ++i;
            continue;
        }

        const std::size_t key_start = i;
        while (i < tag_body.size() && detail::is_word_char(static_cast<unsigned char>(tag_body[i]))) ++i;
        const auto key = tag_body.substr(key_start, i - key_start);
        if (i >= tag_body.size() || tag_body[i] != '=') continue;
        ++i;

        std::string value;
        if (i < tag_body.size() && tag_body[i] == '"') {
            ++i;
            const std::size_t value_start = i;
            while (i < tag_body.size() && tag_body[i] != '"') ++i;
            value = std::string(tag_body.substr(value_start, i - value_start));
            if (i < tag_body.size() && tag_body[i] == '"') ++i;
        } else {
            const std::size_t value_start = i;
            while (i < tag_body.size()) {
                const char ch = tag_body[i];
                if (std::isspace(static_cast<unsigned char>(ch)) || ch == '/' || ch == '>') break;
                ++i;
            }
            value = std::string(tag_body.substr(value_start, i - value_start));
        }
        attrs[std::string(key)] = std::move(value);
    }
    return attrs;
}

[[nodiscard]] inline std::string first_command_token(std::string_view command) {
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) command.remove_prefix(1);
    std::size_t end = 0;
    while (end < command.size() && !std::isspace(static_cast<unsigned char>(command[end]))) ++end;
    return std::string(command.substr(0, end));
}

[[nodiscard]] inline ExtractedClaudeCodeHints extract_claude_code_hints(std::string_view output, std::string_view command) {
    if (output.find("<claude-code-hint") == std::string_view::npos) {
        return {.hints = {}, .stripped = std::string(output)};
    }

    ExtractedClaudeCodeHints result;
    result.stripped.reserve(output.size());
    const std::string source_command = first_command_token(command);
    bool matched_any_line = false;

    std::size_t pos = 0;
    while (pos < output.size()) {
        const auto newline = output.find('\n', pos);
        const bool has_newline = newline != std::string_view::npos;
        const std::size_t line_end = has_newline ? newline : output.size();
        const auto line = output.substr(pos, line_end - pos);

        if (detail::is_hint_tag_line(line)) {
            matched_any_line = true;
            const auto attrs = parse_attrs(line);
            const auto v_it = attrs.find("v");
            const auto type_it = attrs.find("type");
            const auto value_it = attrs.find("value");
            const auto version = v_it == attrs.end() ? std::optional<int>{} : detail::parse_int(v_it->second);
            if (version == 1 && type_it != attrs.end() && type_it->second == "plugin" && value_it != attrs.end() && !value_it->second.empty()) {
                result.hints.push_back(ClaudeCodeHint{.v = *version, .type = type_it->second, .value = value_it->second, .source_command = source_command});
            }
            if (has_newline) result.stripped.push_back('\n');
        } else {
            result.stripped.append(line);
            if (has_newline) result.stripped.push_back('\n');
        }

        if (!has_newline) break;
        pos = newline + 1;
    }

    if (matched_any_line) {
        result.stripped = detail::collapse_extra_blank_lines(result.stripped);
    }
    return result;
}

class PendingHintStore {
public:
    using Callback = std::function<void()>;
    using Unsubscribe = std::function<void()>;

    [[nodiscard]] Unsubscribe subscribe(Callback callback) {
        const std::size_t id = next_subscription_id_++;
        subscribers_.push_back({id, std::move(callback)});
        return [this, id] {
            for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
                if (it->first == id) {
                    subscribers_.erase(it);
                    return;
                }
            }
        };
    }

    void set_pending_hint(ClaudeCodeHint hint) {
        if (shown_this_session_) return;
        pending_hint_ = std::move(hint);
        notify();
    }

    void clear_pending_hint() {
        if (!pending_hint_.has_value()) return;
        pending_hint_.reset();
        notify();
    }

    void mark_shown_this_session() noexcept {
        shown_this_session_ = true;
    }

    [[nodiscard]] std::optional<ClaudeCodeHint> get_pending_hint_snapshot() const {
        return pending_hint_;
    }

    [[nodiscard]] bool has_shown_hint_this_session() const noexcept {
        return shown_this_session_;
    }

    void reset() noexcept {
        pending_hint_.reset();
        shown_this_session_ = false;
    }

private:
    void notify() {
        const auto subscribers = subscribers_;
        for (const auto& [_, callback] : subscribers) callback();
    }

    std::optional<ClaudeCodeHint> pending_hint_;
    bool shown_this_session_ = false;
    std::size_t next_subscription_id_ = 1;
    std::vector<std::pair<std::size_t, Callback>> subscribers_;
};

} // namespace cc::utils::claude_code_hints
