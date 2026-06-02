module;
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

export module cc.ui.layout.wrap_text;

export namespace cc::ui::layout {

// Break a long word that exceeds the width into multiple chunks
inline auto break_long_word(std::string_view word, int width) -> std::vector<std::string> {
    std::vector<std::string> result;
    if (width <= 0) return result;

    size_t pos = 0;
    while (pos < word.size()) {
        size_t chunk_size = std::min(static_cast<size_t>(width), word.size() - pos);
        result.emplace_back(word.substr(pos, chunk_size));
        pos += chunk_size;
    }
    return result;
}

// Wrap text at word boundaries to fit within a given width
inline auto wrap_text(std::string_view text, int width) -> std::vector<std::string> {
    std::vector<std::string> lines;
    if (width <= 0) return lines;

    // Process each input line separately (preserve explicit newlines)
    size_t line_start = 0;
    while (line_start <= text.size()) {
        auto line_end = text.find('\n', line_start);
        std::string_view input_line;
        if (line_end == std::string_view::npos) {
            input_line = text.substr(line_start);
            line_start = text.size() + 1;
        } else {
            input_line = text.substr(line_start, line_end - line_start);
            line_start = line_end + 1;
        }

        // Empty line
        if (input_line.empty()) {
            lines.emplace_back("");
            continue;
        }

        // Word-wrap this line
        std::string current_line;
        int current_width = 0;
        size_t i = 0;

        while (i < input_line.size()) {
            // Skip leading spaces
            if (input_line[i] == ' ' && current_width == 0) {
                current_line += ' ';
                ++current_width;
                ++i;
                continue;
            }

            // Extract next word
            size_t word_start = i;
            while (i < input_line.size() && input_line[i] != ' ') ++i;
            std::string_view word = input_line.substr(word_start, i - word_start);

            int word_len = static_cast<int>(word.size());

            // Word fits on current line
            if (current_width + word_len <= width) {
                current_line += std::string(word);
                current_width += word_len;
            }
            // Word fits on a new line
            else if (word_len <= width) {
                lines.push_back(current_line);
                current_line = std::string(word);
                current_width = word_len;
            }
            // Word is longer than width - must break it
            else {
                if (!current_line.empty()) {
                    lines.push_back(current_line);
                    current_line.clear();
                    current_width = 0;
                }
                auto chunks = break_long_word(word, width);
                for (size_t c = 0; c < chunks.size(); ++c) {
                    if (c < chunks.size() - 1) {
                        lines.push_back(chunks[c]);
                    } else {
                        current_line = chunks[c];
                        current_width = static_cast<int>(chunks[c].size());
                    }
                }
            }

            // Consume trailing space
            if (i < input_line.size() && input_line[i] == ' ') {
                if (current_width < width) {
                    current_line += ' ';
                    ++current_width;
                }
                ++i;
            }
        }

        // Push remaining content
        lines.push_back(current_line);
    }

    return lines;
}

// ANSI-aware text wrapping (preserves escape sequences across line breaks)
inline auto wrap_text_ansi(std::string_view text, int width) -> std::vector<std::string> {
    std::vector<std::string> lines;
    if (width <= 0) return lines;

    std::string current_line;
    int visible_width = 0;
    std::string active_style; // Track current active ANSI style
    bool in_escape = false;
    std::string escape_buf;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];

        // Handle explicit newlines
        if (c == '\n' && !in_escape) {
            lines.push_back(current_line);
            current_line.clear();
            // Carry over active style to next line
            if (!active_style.empty()) {
                current_line = active_style;
            }
            visible_width = 0;
            continue;
        }

        // Track ANSI escape sequences
        if (c == '\033') {
            in_escape = true;
            escape_buf = "\033";
            continue;
        }
        if (in_escape) {
            escape_buf += c;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                in_escape = false;
                current_line += escape_buf;
                // Track style resets and changes
                if (escape_buf.find("[0m") != std::string::npos ||
                    escape_buf.find("[m") != std::string::npos) {
                    active_style.clear();
                } else if (escape_buf.find("[") != std::string::npos) {
                    active_style = escape_buf;
                }
                escape_buf.clear();
            }
            continue;
        }

        // Regular character - check if we need to wrap
        if (visible_width >= width) {
            // Reset style at end of line
            if (!active_style.empty()) {
                current_line += "\033[0m";
            }
            lines.push_back(current_line);
            current_line.clear();
            visible_width = 0;
            // Re-apply style on new line
            if (!active_style.empty()) {
                current_line = active_style;
            }
        }

        current_line += c;
        ++visible_width;
    }

    // Push final line
    if (!current_line.empty() || lines.empty()) {
        lines.push_back(current_line);
    }

    return lines;
}

// Convenience: wrap text and join with newlines
inline auto word_wrap(std::string_view text, int width) -> std::string {
    auto lines = wrap_text(text, width);
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i < lines.size() - 1) out << "\n";
    }
    return out.str();
}

} // namespace cc::ui::layout
