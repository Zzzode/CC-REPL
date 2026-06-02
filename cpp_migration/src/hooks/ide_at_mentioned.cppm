module;
#include <expected>
#include <string>
#include <string_view>
#include <vector>

export module cc.hooks.ide_at_mentioned;

export namespace cc::hooks {

// @mention 解析结果
struct AtMention {
    std::string type;   // 类型：file, symbol, url, user 等
    std::string value;  // 值：被引用的资源标识
    int start;          // 在输入字符串中的起始位置
    int end;            // 在输入字符串中的结束位置
};

// 从输入文本中解析所有 @mention
inline std::vector<AtMention> parse_at_mentions(std::string_view input) {
    std::vector<AtMention> mentions;
    std::size_t pos = 0;

    while (pos < input.size()) {
        // 查找 '@' 字符
        auto at_pos = input.find('@', pos);
        if (at_pos == std::string_view::npos) break;

        // 提取 @后面的标识符（直到空格或行尾）
        auto end_pos = input.find_first_of(" \t\n", at_pos + 1);
        if (end_pos == std::string_view::npos) end_pos = input.size();

        if (end_pos > at_pos + 1) {
            auto value = std::string(input.substr(at_pos + 1, end_pos - at_pos - 1));
            // 根据前缀推断类型
            std::string type = "symbol";
            if (value.find('/') != std::string::npos || value.find('.') != std::string::npos) {
                type = "file";
            }
            mentions.push_back(AtMention{
                .type = type,
                .value = value,
                .start = static_cast<int>(at_pos),
                .end = static_cast<int>(end_pos)
            });
        }
        pos = end_pos;
    }
    return mentions;
}

// 解析 @mention 为实际内容（如读取文件内容、获取符号定义）
inline std::expected<std::string, std::string> resolve_at_mention(AtMention mention) {
    if (mention.value.empty()) {
        return std::unexpected("Empty mention value");
    }
    // 根据 type 分发到不同的解析逻辑
    if (mention.type == "file") {
        // 尝试读取文件内容
        return std::unexpected("File resolution not yet connected to filesystem");
    }
    return std::unexpected("Unknown mention type: " + mention.type);
}

// 获取 @mention 的自动补全建议
inline std::vector<std::string> get_at_mention_completions(std::string_view prefix) {
    // Return common @mention targets that match the given prefix
    static const std::vector<std::string> known_targets = {
        "file", "symbol", "url", "workspace", "selection", "terminal",
        "git", "problems", "codebase",
    };
    std::vector<std::string> completions;
    for (const auto& target : known_targets) {
        if (prefix.empty() || target.find(prefix) == 0) {
            completions.push_back(target);
        }
    }
    return completions;
}

} // namespace cc::hooks
