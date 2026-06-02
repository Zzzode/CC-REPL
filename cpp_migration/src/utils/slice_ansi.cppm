module;
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

export module cc.utils.slice_ansi;

export namespace cc::utils {

// ANSI 转义序列检测：判断当前位置是否为 ESC[ 起始
namespace ansi_detail {
    // 解析单个 ANSI 转义序列，返回序列结束后的位置
    inline size_t skip_ansi_sequence(std::string_view sv, size_t pos) {
        if (pos + 1 >= sv.size() || sv[pos] != '\033' || sv[pos + 1] != '[') {
            return pos;
        }
        size_t i = pos + 2; // 跳过 ESC[
        // 跳过参数字节 (0x30–0x3F) 和中间字节 (0x20–0x2F)
        while (i < sv.size() && sv[i] >= 0x20 && sv[i] <= 0x3F) ++i;
        while (i < sv.size() && sv[i] >= 0x20 && sv[i] <= 0x2F) ++i;
        // 最终字节 (0x40–0x7E)
        if (i < sv.size() && sv[i] >= 0x40 && sv[i] <= 0x7E) ++i;
        return i;
    }

    inline bool is_ansi_start(std::string_view sv, size_t pos) {
        return pos + 1 < sv.size() && sv[pos] == '\033' && sv[pos + 1] == '[';
    }
}

// 计算不含 ANSI 转义码的可见字符长度
[[nodiscard]] inline size_t visible_length(std::string_view sv) {
    size_t len = 0;
    size_t i = 0;
    while (i < sv.size()) {
        if (ansi_detail::is_ansi_start(sv, i)) {
            i = ansi_detail::skip_ansi_sequence(sv, i);
        } else {
            ++len;
            ++i;
        }
    }
    return len;
}

// ANSI 感知的字符串切片，按可见字符位置切割
[[nodiscard]] inline std::string slice_ansi(std::string_view sv, size_t start, size_t end) {
    std::string result;
    size_t visible_pos = 0;
    size_t i = 0;
    // 收集处于激活状态的 ANSI 序列以在切片开头重新应用
    std::string active_styles;

    while (i < sv.size()) {
        if (ansi_detail::is_ansi_start(sv, i)) {
            size_t seq_end = ansi_detail::skip_ansi_sequence(sv, i);
            std::string_view seq = sv.substr(i, seq_end - i);
            if (visible_pos >= start && visible_pos < end) {
                result += seq;
            } else if (visible_pos < start) {
                // 跟踪在切片起点前激活的样式
                active_styles += seq;
            }
            i = seq_end;
        } else {
            if (visible_pos >= start && visible_pos < end) {
                if (result.empty() && !active_styles.empty()) {
                    result += active_styles; // 重新应用之前的样式
                }
                result += sv[i];
            }
            ++visible_pos;
            ++i;
            if (visible_pos >= end) break;
        }
    }
    return result;
}

// 移除所有 ANSI 转义序列
[[nodiscard]] inline std::string strip_ansi(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    size_t i = 0;
    while (i < sv.size()) {
        if (ansi_detail::is_ansi_start(sv, i)) {
            i = ansi_detail::skip_ansi_sequence(sv, i);
        } else {
            result += sv[i];
            ++i;
        }
    }
    return result;
}

// ANSI 感知的文本自动换行
[[nodiscard]] inline std::string wrap_ansi(std::string_view sv, size_t width) {
    if (width == 0) return std::string(sv);

    std::string result;
    size_t col = 0; // 当前列（可见字符计数）
    size_t i = 0;

    while (i < sv.size()) {
        if (sv[i] == '\n') {
            result += '\n';
            col = 0;
            ++i;
            continue;
        }

        if (ansi_detail::is_ansi_start(sv, i)) {
            // ANSI 序列不占可见宽度，直接输出
            size_t seq_end = ansi_detail::skip_ansi_sequence(sv, i);
            result += sv.substr(i, seq_end - i);
            i = seq_end;
            continue;
        }

        // 达到宽度限制时插入换行
        if (col >= width) {
            result += '\n';
            col = 0;
        }
        result += sv[i];
        ++col;
        ++i;
    }
    return result;
}

} // namespace cc::utils
