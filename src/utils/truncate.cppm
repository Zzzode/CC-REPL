module;
#include <string>
#include <string_view>
#include <filesystem>
#include <algorithm>

export module cc.utils.truncate;

export namespace cc::utils {


namespace detail {
    inline size_t visible_len(std::string_view sv) {
        size_t len = 0;
        bool in_escape = false;
        for (size_t i = 0; i < sv.size(); ++i) {
            if (in_escape) {

                if ((sv[i] >= 'A' && sv[i] <= 'Z') ||
                    (sv[i] >= 'a' && sv[i] <= 'z') || sv[i] == '~') {
                    in_escape = false;
                }
            } else if (sv[i] == '\033' && i + 1 < sv.size() && sv[i + 1] == '[') {
                in_escape = true;
                ++i;
            } else {
                ++len;
            }
        }
        return len;
    }
}


[[nodiscard]] inline std::string truncate_text(std::string_view sv, size_t max_width,
                                                std::string_view ellipsis = "...") {
    if (detail::visible_len(sv) <= max_width) return std::string(sv);
    if (max_width <= ellipsis.size()) return std::string(ellipsis.substr(0, max_width));

    size_t target = max_width - ellipsis.size();
    std::string result;
    size_t visible_count = 0;
    bool in_escape = false;

    for (size_t i = 0; i < sv.size() && visible_count < target; ++i) {
        result += sv[i];
        if (in_escape) {
            if ((sv[i] >= 'A' && sv[i] <= 'Z') ||
                (sv[i] >= 'a' && sv[i] <= 'z') || sv[i] == '~') {
                in_escape = false;
            }
        } else if (sv[i] == '\033' && i + 1 < sv.size() && sv[i + 1] == '[') {
            in_escape = true;
            result += sv[++i];
        } else {
            ++visible_count;
        }
    }
    result += ellipsis;
    return result;
}


[[nodiscard]] inline std::string truncate_middle(std::string_view sv, size_t max_width) {
    auto vis_len = detail::visible_len(sv);
    if (vis_len <= max_width) return std::string(sv);

    constexpr std::string_view ellipsis = "...";
    if (max_width <= ellipsis.size()) return std::string(ellipsis.substr(0, max_width));

    size_t available = max_width - ellipsis.size();
    size_t head_len = (available + 1) / 2;
    size_t tail_len = available / 2;


    std::string result;
    result.reserve(max_width);
    result += sv.substr(0, head_len);
    result += ellipsis;
    result += sv.substr(sv.size() - tail_len);
    return result;
}


[[nodiscard]] inline std::string truncate_path(const std::filesystem::path& p, size_t max_width) {
    std::string path_str = p.string();
    if (path_str.size() <= max_width) return path_str;

    auto filename = p.filename().string();

    if (filename.size() >= max_width) {
        return truncate_text(filename, max_width);
    }


    constexpr std::string_view ellipsis = ".../";
    size_t remaining = max_width - filename.size();
    if (remaining <= ellipsis.size()) {
        return std::string(ellipsis) + filename;
    }


    auto parent = p.parent_path().string();
    size_t prefix_budget = remaining - ellipsis.size();
    if (prefix_budget > 0 && parent.size() > prefix_budget) {
        return parent.substr(0, prefix_budget) + std::string(ellipsis) + filename;
    }
    return std::string(ellipsis) + filename;
}

} // namespace cc::utils
