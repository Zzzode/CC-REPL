module;
#include <string>
#include <optional>
#include <algorithm>
#include <cctype>

export module cc.vim.vim_motions;

export namespace cc::vim {

// A text motion result (start and end positions in the buffer)
struct Motion {
    int start = 0;
    int end = 0;
    bool linewise = false;
};

// Apply a motion to text, returning the selected substring
inline auto apply_motion(std::string_view text, Motion motion) -> std::string_view {
    int s = std::max(0, std::min(motion.start, motion.end));
    int e = std::min(static_cast<int>(text.size()), std::max(motion.start, motion.end));
    if (s >= e) return {};
    return text.substr(s, e - s);
}

// Parse a motion key sequence and compute the resulting motion
inline auto parse_motion(std::string_view keys, std::string_view text, int cursor)
    -> std::optional<Motion> {
    if (keys.empty() || text.empty()) return std::nullopt;

    int len = static_cast<int>(text.size());
    cursor = std::clamp(cursor, 0, len - 1);

    char key = keys[0];

    switch (key) {
        case 'w': {
            // Word forward: move to start of next word
            int pos = cursor;
            // Skip current word
            while (pos < len && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            // Skip whitespace
            while (pos < len && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            return Motion{cursor, pos, false};
        }

        case 'b': {
            // Word backward: move to start of previous word
            int pos = cursor;
            // Skip whitespace before cursor
            if (pos > 0) --pos;
            while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos]))) --pos;
            // Skip word characters backward
            while (pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) --pos;
            return Motion{cursor, pos, false};
        }

        case 'e': {
            // Word end: move to end of current/next word
            int pos = cursor;
            if (pos < len - 1) ++pos;
            // Skip whitespace
            while (pos < len && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            // Move to end of word
            while (pos < len - 1 && !std::isspace(static_cast<unsigned char>(text[pos + 1]))) ++pos;
            return Motion{cursor, pos + 1, false};
        }

        case '0': {
            // Line start: move to beginning of current line
            int pos = cursor;
            while (pos > 0 && text[pos - 1] != '\n') --pos;
            return Motion{cursor, pos, false};
        }

        case '$': {
            // Line end: move to end of current line
            int pos = cursor;
            while (pos < len && text[pos] != '\n') ++pos;
            return Motion{cursor, pos, false};
        }

        case 'f': {
            // Find forward: move to next occurrence of character
            if (keys.size() < 2) return std::nullopt;
            char target = keys[1];
            int pos = cursor + 1;
            while (pos < len) {
                if (text[pos] == target) {
                    return Motion{cursor, pos + 1, false};
                }
                ++pos;
            }
            return std::nullopt; // Character not found
        }

        case 't': {
            // Till forward: move to character before next occurrence
            if (keys.size() < 2) return std::nullopt;
            char target = keys[1];
            int pos = cursor + 1;
            while (pos < len) {
                if (text[pos] == target) {
                    return Motion{cursor, pos, false};
                }
                ++pos;
            }
            return std::nullopt; // Character not found
        }

        case '/': {
            // Search forward: find next occurrence of pattern
            if (keys.size() < 2) return std::nullopt;
            std::string_view pattern = keys.substr(1);
            auto found = text.find(pattern, cursor + 1);
            if (found != std::string_view::npos) {
                return Motion{cursor, static_cast<int>(found), false};
            }
            // Wrap around
            found = text.find(pattern, 0);
            if (found != std::string_view::npos && static_cast<int>(found) != cursor) {
                return Motion{cursor, static_cast<int>(found), false};
            }
            return std::nullopt;
        }

        case '?': {
            // Search backward: find previous occurrence of pattern
            if (keys.size() < 2) return std::nullopt;
            std::string_view pattern = keys.substr(1);
            // Search backward from cursor
            auto haystack = text.substr(0, cursor);
            auto found = haystack.rfind(pattern);
            if (found != std::string_view::npos) {
                return Motion{cursor, static_cast<int>(found), false};
            }
            // Wrap around
            found = text.rfind(pattern);
            if (found != std::string_view::npos && static_cast<int>(found) != cursor) {
                return Motion{cursor, static_cast<int>(found), false};
            }
            return std::nullopt;
        }

        case 'g': {
            // gg: go to top of document
            if (keys.size() >= 2 && keys[1] == 'g') {
                return Motion{cursor, 0, true};
            }
            return std::nullopt;
        }

        case 'G': {
            // G: go to end of document
            return Motion{cursor, len, true};
        }

        default:
            return std::nullopt;
    }
}

} // namespace cc::vim
