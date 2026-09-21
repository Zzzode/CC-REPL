module;
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

export module cc.utils.slice_ansi;

export namespace cc::utils {


namespace ansi_detail {

    inline size_t skip_ansi_sequence(std::string_view sv, size_t pos) {
        if (pos + 1 >= sv.size() || sv[pos] != '\033' || sv[pos + 1] != '[') {
            return pos;
        }
        size_t i = pos + 2;

        while (i < sv.size() && sv[i] >= 0x20 && sv[i] <= 0x3F) ++i;
        while (i < sv.size() && sv[i] >= 0x20 && sv[i] <= 0x2F) ++i;

        if (i < sv.size() && sv[i] >= 0x40 && sv[i] <= 0x7E) ++i;
        return i;
    }

    inline bool is_ansi_start(std::string_view sv, size_t pos) {
        return pos + 1 < sv.size() && sv[pos] == '\033' && sv[pos + 1] == '[';
    }
}


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


[[nodiscard]] inline std::string slice_ansi(std::string_view sv, size_t start, size_t end) {
    std::string result;
    size_t visible_pos = 0;
    size_t i = 0;

    std::string active_styles;

    while (i < sv.size()) {
        if (ansi_detail::is_ansi_start(sv, i)) {
            size_t seq_end = ansi_detail::skip_ansi_sequence(sv, i);
            std::string_view seq = sv.substr(i, seq_end - i);
            if (visible_pos >= start && visible_pos < end) {
                result += seq;
            } else if (visible_pos < start) {

                active_styles += seq;
            }
            i = seq_end;
        } else {
            if (visible_pos >= start && visible_pos < end) {
                if (result.empty() && !active_styles.empty()) {
                    result += active_styles;
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


[[nodiscard]] inline std::string wrap_ansi(std::string_view sv, size_t width) {
    if (width == 0) return std::string(sv);

    std::string result;
    size_t col = 0;
    size_t i = 0;

    while (i < sv.size()) {
        if (sv[i] == '\n') {
            result += '\n';
            col = 0;
            ++i;
            continue;
        }

        if (ansi_detail::is_ansi_start(sv, i)) {

            size_t seq_end = ansi_detail::skip_ansi_sequence(sv, i);
            result += sv.substr(i, seq_end - i);
            i = seq_end;
            continue;
        }


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
