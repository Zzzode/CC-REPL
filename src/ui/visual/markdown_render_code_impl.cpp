// markdown_render_code_impl.cpp - impl unit for cc.ui.visual.markdown
// (RFC 0001 Phase C batch 6). Holds render_code_block(), the ONLY body that
// uses cc.ui.visual.code_highlight: keeping that import here (instead of in
// the cppm) removes code_highlight (+ its cc.types.types closure) from the
// markdown BMI and from every importer of cc.ui.visual.markdown.
module;

#include <ftxui/dom/elements.hpp>

module cc.ui.visual.markdown;

import std;

import cc.ui.visual.code_highlight;

namespace cc::ui {
namespace detail {

[[nodiscard]] Element render_code_block(const BlockToken& tok,
                                        const MarkdownOptions& opts) {
    // Split into lines (preserve trailing empty line behavior like TS EOL).
    auto split_lines = [](std::string_view code) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= code.size()) {
            auto nl = code.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(code.substr(start));
                break;
            }
            lines.emplace_back(code.substr(start, nl - start));
            start = nl + 1;
        }
        return lines;
    };

    auto lines = split_lines(tok.code_content);

    if (opts.syntax_highlighting && !tok.code_content.empty()) {
        auto highlighted = code_highlight::highlight_source(
            tok.code_content, tok.code_lang);
        auto theme_result = code_highlight::get_syntax_theme(opts.theme_name);
        auto theme = theme_result ? *theme_result
                                  : code_highlight::get_theme(
                                        code_highlight::ThemeName::Dark);

        Elements out;
        for (const auto& hl : highlighted.lines) {
            // Chrome-free: no line-number gutter, no diff background, no
            // highlight background. Just the colored tokens of each line.
            Elements parts;
            if (hl.tokens.empty()) {
                parts.push_back(text(""));
            } else {
                for (const auto& t : hl.tokens) {
                    parts.push_back(text(t.text) | color(
                        code_highlight::token_color(t.type, theme)));
                }
            }
            Element line_el = hbox(std::move(parts));
            if (opts.dim_color) line_el = line_el | dim;
            out.push_back(std::move(line_el));
        }
        // highlight_source emits one line per source line; if the tokenizer
        // produced fewer (shouldn't), fall back to plain lines.
        if (out.size() == lines.size()) {
            return vbox(std::move(out));
        }
    }

    // Plain code block (no highlighting or tokenizer mismatch).
    Elements out;
    for (const auto& line : lines) {
        Element el = text(line);
        if (opts.dim_color) el = el | dim;
        out.push_back(std::move(el));
    }
    return vbox(std::move(out));
}
} // namespace detail
} // namespace cc::ui
