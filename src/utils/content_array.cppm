module;

#include <string>
#include <utility>
#include <vector>

export module cc.utils.content_array;

export namespace cc::utils::content_array {

struct ContentBlock {
    std::string type;
    std::string text;
};

inline void insert_block_after_tool_results(std::vector<ContentBlock>& content, ContentBlock block) {
    int last_tool_result_index = -1;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i].type == "tool_result") {
            last_tool_result_index = static_cast<int>(i);
        }
    }

    if (last_tool_result_index >= 0) {
        const auto insert_pos = static_cast<std::size_t>(last_tool_result_index + 1);
        content.insert(content.begin() + static_cast<std::ptrdiff_t>(insert_pos), std::move(block));
        if (insert_pos == content.size() - 1) {
            content.push_back(ContentBlock{.type = "text", .text = "."});
        }
    } else {
        const std::size_t insert_index = content.empty() ? 0 : content.size() - 1;
        content.insert(content.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(block));
    }
}

} // namespace cc::utils::content_array
