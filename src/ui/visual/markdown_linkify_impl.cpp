// markdown_linkify_impl.cpp - impl unit for cc.ui.visual.markdown (RFC 0001
// Phase C batch 6). Holds linkify_issue_references(): owner/repo#123 ->
// FTXUI OSC 8 hyperlink elements. Moved out of the interface BMI.
module;

#include <cctype>

#include <ftxui/dom/elements.hpp>

module cc.ui.visual.markdown;

import std;

namespace cc::ui {
namespace detail {

[[nodiscard]] Elements linkify_issue_references(
    std::string_view text,
    const MarkdownOptions& opts) {

    Elements result;
    std::string buffer;
    buffer.reserve(text.size());

    auto flush_buffer = [&]() {
        if (!buffer.empty()) {
            Element el = ftxui::text(std::move(buffer));
            if (opts.dim_color) el = el | dim;
            result.push_back(std::move(el));
            buffer.clear();
        }
    };

    std::size_t i = 0;
    while (i < text.size()) {
        // Look for '#' that could start an issue ref.
        auto hash_pos = text.find('#', i);
        if (hash_pos == std::string_view::npos) {
            buffer.append(text.substr(i));
            break;
        }

        // Everything up to the '#' goes into the buffer.
        buffer.append(text.substr(i, hash_pos - i));

        // Try to parse owner/repo#NNN starting from the '#'.
        // Walk backwards to find the repo segment.
        // The pattern requires: [prefix_not_word_dot_slash_dash] [A-Za-z0-9][\w-]* / [A-Za-z0-9][\w.-]* # digits
        //
        // Since we're at '#', scan forward for the digits first, then
        // backward for the repo.
        std::size_t num_start = hash_pos + 1;
        std::size_t num_end = num_start;
        while (num_end < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[num_end]))) {
            ++num_end;
        }
        std::size_t num_len = num_end - num_start;
        // Must have at least one digit, and the char after must be a word
        // boundary (non-alnum, non-underscore, or end of string).
        if (num_len == 0) {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }
        bool word_boundary = (num_end == text.size()) ||
            (!std::isalnum(static_cast<unsigned char>(text[num_end])) &&
             text[num_end] != '_');
        if (!word_boundary) {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }

        // Now scan backwards from hash_pos to find the owner/repo segment.
        // We need: [A-Za-z0-9][\w.-]* / [A-Za-z0-9][\w-]*
        // preceded by a non-(word, dot, slash, dash) character or start of string.
        std::size_t repo_end = hash_pos;
        // Walk back through the repo name (alnum, dot, dash, underscore).
        std::size_t p = repo_end;
        while (p > 0) {
            char c = text[p - 1];
            if (std::isalnum(static_cast<unsigned char>(c)) ||
                c == '.' || c == '-' || c == '_') {
                --p;
            } else {
                break;
            }
        }
        // p now points to the start of the repo segment (after '/').
        // But we need at least one alphanumeric start char for the repo.
        if (p == repo_end ||
            !std::isalnum(static_cast<unsigned char>(text[p]))) {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }

        // Check for '/' separating owner and repo.
        if (p == 0 || text[p - 1] != '/') {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }
        std::size_t slash_pos = p - 1;

        // Walk back through the owner segment (alnum, dash, underscore).
        std::size_t owner_end = slash_pos;
        std::size_t o = owner_end;
        while (o > 0) {
            char c = text[o - 1];
            if (std::isalnum(static_cast<unsigned char>(c)) ||
                c == '-' || c == '_') {
                --o;
            } else {
                break;
            }
        }
        // Owner must start with alphanumeric.
        if (o == owner_end ||
            !std::isalnum(static_cast<unsigned char>(text[o]))) {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }

        // Check prefix boundary: character before owner must not be a word char,
        // dot, slash, or dash. (Or start of string.)
        bool prefix_ok = (o == 0);
        if (!prefix_ok) {
            char prev = text[o - 1];
            prefix_ok = !std::isalnum(static_cast<unsigned char>(prev)) &&
                        prev != '_' && prev != '.' &&
                        prev != '/' && prev != '-';
        }
        if (!prefix_ok) {
            buffer += '#';
            i = hash_pos + 1;
            continue;
        }

        // We have a valid match: text[o..num_end] = "owner/repo#NNN"
        // Flush any accumulated text before the owner.
        // The buffer already contains text up to hash_pos.  We need to remove
        // the "owner/repo" part from the buffer (it was appended above) and
        // emit it as a hyperlink instead.
        //
        // Actually, the buffer has text[i..hash_pos].  The owner/repo part
        // starts at o which is >= i (since we scanned from hash_pos backward).
        // So the buffer contains: text[i..o] + text[o..hash_pos] = text[i..o] + "owner/repo"
        //
        // We need to split the buffer: keep text[i..o] in buffer, flush it,
        // then emit the hyperlink for "owner/repo#NNN".

        // Remove the "owner/repo" suffix from the buffer.
        std::size_t owner_repo_len = hash_pos - o;
        if (buffer.size() >= owner_repo_len) {
            buffer.resize(buffer.size() - owner_repo_len);
        }
        flush_buffer();

        // Build the hyperlink.
        std::string repo_str(text.substr(o, num_end - o));
        std::string url = "https://github.com/" +
                          std::string(text.substr(o, slash_pos - o)) +
                          "/issues/" +
                          std::string(text.substr(num_start, num_len));

        Element link_el = ftxui::text(repo_str) | hyperlink(url) | underlined;
        if (opts.dim_color) link_el = link_el | dim;
        result.push_back(std::move(link_el));

        i = num_end;
    }

    flush_buffer();
    return result;
}
} // namespace detail
} // namespace cc::ui
