// File Edit Utilities
// Provides utilities for editing files with patch generation
module;

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <regex>
#include <format>

export module cc.utils.file_edit;

export namespace cc::utils::file_edit {

// Quote normalization constants
inline constexpr std::string_view LEFT_SINGLE_CURLY_QUOTE = "\xE2\x80\x98";
inline constexpr std::string_view RIGHT_SINGLE_CURLY_QUOTE = "\xE2\x80\x99";
inline constexpr std::string_view LEFT_DOUBLE_CURLY_QUOTE = "\xE2\x80\x9C";
inline constexpr std::string_view RIGHT_DOUBLE_CURLY_QUOTE = "\xE2\x80\x9D";

// File edit structure
struct FileEdit {
    std::string old_string;
    std::string new_string;
    bool replace_all = false;
};

// Patch hunk structure
struct PatchHunk {
    int old_start = 0;
    int old_lines = 0;
    int new_start = 0;
    int new_lines = 0;
    std::vector<std::string> lines;
};

// Normalizes quotes in a string by converting curly quotes to straight quotes
[[nodiscard]] inline std::string normalize_quotes(std::string_view str) {
    std::string result(str);
    
    // Replace single curly quotes
    size_t pos = 0;
    while ((pos = result.find(LEFT_SINGLE_CURLY_QUOTE, pos)) != std::string::npos) {
        result.replace(pos, LEFT_SINGLE_CURLY_QUOTE.size(), "'");
        pos += 1;
    }
    pos = 0;
    while ((pos = result.find(RIGHT_SINGLE_CURLY_QUOTE, pos)) != std::string::npos) {
        result.replace(pos, RIGHT_SINGLE_CURLY_QUOTE.size(), "'");
        pos += 1;
    }
    
    // Replace double curly quotes
    pos = 0;
    while ((pos = result.find(LEFT_DOUBLE_CURLY_QUOTE, pos)) != std::string::npos) {
        result.replace(pos, LEFT_DOUBLE_CURLY_QUOTE.size(), "\"");
        pos += 1;
    }
    pos = 0;
    while ((pos = result.find(RIGHT_DOUBLE_CURLY_QUOTE, pos)) != std::string::npos) {
        result.replace(pos, RIGHT_DOUBLE_CURLY_QUOTE.size(), "\"");
        pos += 1;
    }
    
    return result;
}

// Strips trailing whitespace from each line in a string while preserving line endings
[[nodiscard]] inline std::string strip_trailing_whitespace(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    
    std::string line;
    size_t start = 0;
    size_t end = 0;
    
    while (end < str.size()) {
        // Find line ending
        if (str[end] == '\n') {
            // Extract line without the newline
            line = std::string(str.substr(start, end - start));
            
            // Strip trailing whitespace from the line
            size_t last_non_ws = line.find_last_not_of(" \t\r");
            if (last_non_ws != std::string::npos) {
                line = line.substr(0, last_non_ws + 1);
            } else {
                line.clear();
            }
            
            result += line;
            result += '\n';
            start = end + 1;
        } else if (str[end] == '\r') {
            // Check for CRLF
            if (end + 1 < str.size() && str[end + 1] == '\n') {
                line = std::string(str.substr(start, end - start));
                
                size_t last_non_ws = line.find_last_not_of(" \t\r");
                if (last_non_ws != std::string::npos) {
                    line = line.substr(0, last_non_ws + 1);
                } else {
                    line.clear();
                }
                
                result += line;
                result += "\r\n";
                start = end + 2;
                end++; // skip the '\n'
            } else {
                line = std::string(str.substr(start, end - start));
                
                size_t last_non_ws = line.find_last_not_of(" \t\r");
                if (last_non_ws != std::string::npos) {
                    line = line.substr(0, last_non_ws + 1);
                } else {
                    line.clear();
                }
                
                result += line;
                result += '\r';
                start = end + 1;
            }
        }
        end++;
    }
    
    // Handle remaining part after last newline
    if (start < str.size()) {
        line = std::string(str.substr(start));
        size_t last_non_ws = line.find_last_not_of(" \t\r");
        if (last_non_ws != std::string::npos) {
            line = line.substr(0, last_non_ws + 1);
        } else {
            line.clear();
        }
        result += line;
    }
    
    return result;
}

// Finds the actual string in the file content that matches the search string
[[nodiscard]] inline std::optional<std::string> find_actual_string(
    std::string_view file_content, 
    std::string_view search_string) {
    
    // First try exact match
    if (file_content.find(search_string) != std::string_view::npos) {
        return std::string(search_string);
    }
    
    // Try with normalized quotes
    std::string normalized_search = normalize_quotes(search_string);
    std::string normalized_file = normalize_quotes(file_content);
    
    size_t pos = normalized_file.find(normalized_search);
    if (pos != std::string::npos) {
        return std::string(file_content.substr(pos, search_string.size()));
    }
    
    return std::nullopt;
}

// Helper function to check opening context
inline bool is_opening_context(const std::vector<char>& chars, size_t index) {
    if (index == 0) return true;
    char prev = chars[index - 1];
    return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
           prev == '(' || prev == '[' || prev == '{';
}

// Applies curly double quotes
[[nodiscard]] inline std::string apply_curly_double_quotes(std::string_view str) {
    std::vector<char> chars(str.begin(), str.end());
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < chars.size(); ++i) {
        if (chars[i] == '"') {
            result += is_opening_context(chars, i) ? LEFT_DOUBLE_CURLY_QUOTE : RIGHT_DOUBLE_CURLY_QUOTE;
        } else {
            result += chars[i];
        }
    }
    
    return result;
}

// Applies curly single quotes
[[nodiscard]] inline std::string apply_curly_single_quotes(std::string_view str) {
    std::vector<char> chars(str.begin(), str.end());
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < chars.size(); ++i) {
        if (chars[i] == '\'') {
            // Check if this is an apostrophe in a contraction
            bool is_contraction = false;
            if (i > 0 && i < chars.size() - 1) {
                char prev = chars[i - 1];
                char next = chars[i + 1];
                // Simplified check for letters (basic ASCII)
                is_contraction = ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z')) &&
                                 ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'));
            }
            
            if (is_contraction) {
                result += RIGHT_SINGLE_CURLY_QUOTE;
            } else {
                result += is_opening_context(chars, i) ? LEFT_SINGLE_CURLY_QUOTE : RIGHT_SINGLE_CURLY_QUOTE;
            }
        } else {
            result += chars[i];
        }
    }
    
    return result;
}

// Preserves quote style when applying edits
[[nodiscard]] inline std::string preserve_quote_style(
    std::string_view old_string,
    std::string_view actual_old_string,
    std::string_view new_string) {
    
    // If they're the same, no normalization happened
    if (old_string == actual_old_string) {
        return std::string(new_string);
    }
    
    // Detect which curly quote types were in the file
    bool has_double_quotes = actual_old_string.find(LEFT_DOUBLE_CURLY_QUOTE) != std::string::npos ||
                            actual_old_string.find(RIGHT_DOUBLE_CURLY_QUOTE) != std::string::npos;
    bool has_single_quotes = actual_old_string.find(LEFT_SINGLE_CURLY_QUOTE) != std::string::npos ||
                            actual_old_string.find(RIGHT_SINGLE_CURLY_QUOTE) != std::string::npos;
    
    if (!has_double_quotes && !has_single_quotes) {
        return std::string(new_string);
    }
    
    std::string result(new_string);
    
    if (has_double_quotes) {
        result = apply_curly_double_quotes(result);
    }
    if (has_single_quotes) {
        result = apply_curly_single_quotes(result);
    }
    
    return result;
}

// Applies a single edit to the file content
[[nodiscard]] inline std::string apply_edit(
    std::string_view original_content,
    std::string_view old_string,
    std::string_view new_string,
    bool replace_all = false) {
    
    std::string result(original_content);
    
    if (replace_all) {
        size_t pos = 0;
        while ((pos = result.find(old_string, pos)) != std::string::npos) {
            result.replace(pos, old_string.size(), new_string);
            pos += new_string.size();
        }
    } else {
        size_t pos = result.find(old_string);
        if (pos != std::string::npos) {
            result.replace(pos, old_string.size(), new_string);
        }
    }
    
    // Special case: check if we need to strip trailing newline
    if (new_string.empty() && !old_string.ends_with('\n')) {
        std::string with_newline = std::string(old_string) + '\n';
        if (result.find(with_newline) != std::string::npos) {
            size_t pos = result.find(with_newline);
            result.replace(pos, with_newline.size(), new_string);
        }
    }
    
    return result;
}

// Adds line numbers to content
[[nodiscard]] inline std::string add_line_numbers(
    std::string_view content,
    int start_line = 1) {
    
    std::string result;
    std::istringstream iss{std::string{content}};
    std::string line;
    int line_num = start_line;
    
    // Calculate the width needed for line numbers
    int num_lines = 1;
    int temp = start_line;
    while (std::getline(iss, line)) {
        num_lines++;
    }
    
    int width = 0;
    temp = start_line + num_lines - 1;
    while (temp > 0) {
        width++;
        temp /= 10;
    }
    if (width == 0) width = 1;
    
    // Reset and add numbers
    iss.clear();
    iss.str(std::string(content));
    line_num = start_line;
    
    while (std::getline(iss, line)) {
        result += std::format("{:{}d}\t{}\n", line_num, width, line);
        line_num++;
    }
    
    return result;
}

// Gets snippet for patch with context lines
struct SnippetResult {
    std::string formatted_snippet;
    int start_line = 0;
};

[[nodiscard]] inline SnippetResult get_snippet_for_patch(
    const std::vector<PatchHunk>& patches,
    std::string_view new_file,
    int context_lines = 4) {
    
    if (patches.empty()) {
        return {"", 1};
    }
    
    // Find the first and last changed lines across all hunks
    int min_line = std::numeric_limits<int>::max();
    int max_line = std::numeric_limits<int>::min();
    
    for (const auto& hunk : patches) {
        if (hunk.old_start < min_line) {
            min_line = hunk.old_start;
        }
        int hunk_end = hunk.old_start + hunk.new_lines - 1;
        if (hunk_end > max_line) {
            max_line = hunk_end;
        }
    }
    
    // Calculate the range with context
    int start_line = std::max(1, min_line - context_lines);
    int end_line = max_line + context_lines;
    
    // Split the new file into lines and get the snippet
    std::istringstream iss{std::string{new_file}};
    std::vector<std::string> all_lines;
    std::string line;
    while (std::getline(iss, line)) {
        all_lines.push_back(line);
    }
    
    std::vector<std::string> snippet_lines;
    for (int i = start_line - 1; i < end_line && i < static_cast<int>(all_lines.size()); ++i) {
        if (i >= 0) {
            snippet_lines.push_back(all_lines[i]);
        }
    }
    
    // Join the lines
    std::string snippet;
    for (size_t i = 0; i < snippet_lines.size(); ++i) {
        if (i > 0) snippet += '\n';
        snippet += snippet_lines[i];
    }
    
    // Add line numbers
    std::string formatted = add_line_numbers(snippet, start_line);
    
    return {formatted, start_line};
}

} // namespace cc::utils::file_edit
