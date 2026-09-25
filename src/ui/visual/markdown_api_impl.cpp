// markdown_api_impl.cpp - impl unit for cc.ui.visual.markdown (RFC 0001
// Phase C batch 6). Holds the public entry points render_markdown /
// render_markdown_dim, the cache-management accessors, and the
// StreamingMarkdown out-of-line members (update, reset, private static
// find_block_boundary). Default arguments stay on the cppm declarations.
module;

#include <ftxui/dom/elements.hpp>

module cc.ui.visual.markdown;

import std;

namespace cc::ui {

[[nodiscard]] Element render_markdown(std::string_view source,
                                      const MarkdownOptions& opts) {
    std::vector<BlockToken> tokens;

    // Fast path: plain text with no markdown syntax
    if (!detail::has_markdown_syntax(source)) {
        BlockToken tok;
        tok.kind = BlockTokenKind::Paragraph;
        tok.inlines = {{InlineTokenKind::Text, std::string(source), ""}};
        tokens.push_back(std::move(tok));
    } else {
        // Check cache
        auto& cache = detail::global_token_cache();
        const auto* cached = cache.find(source);
        if (cached) {
            tokens = *cached;
        } else {
            tokens = detail::lex_blocks(source);
            cache.put(std::string(source), tokens);
        }
    }

    // Render tokens
    Elements elements;
    for (const auto& tok : tokens) {
        switch (tok.kind) {
            case BlockTokenKind::Paragraph:
                elements.push_back(detail::render_inlines(tok.inlines, opts));
                break;
            case BlockTokenKind::Heading:
                elements.push_back(detail::render_heading(tok, opts));
                break;
            case BlockTokenKind::CodeBlock:
                elements.push_back(detail::render_code_block(tok, opts));
                break;
            case BlockTokenKind::UnorderedList:
                elements.push_back(detail::render_ulist(tok, opts));
                break;
            case BlockTokenKind::OrderedList:
                elements.push_back(detail::render_olist(tok, opts));
                break;
            case BlockTokenKind::Blockquote:
                elements.push_back(detail::render_blockquote(tok, opts));
                break;
            case BlockTokenKind::Table:
                elements.push_back(detail::render_table(tok, opts));
                break;
            case BlockTokenKind::HorizontalRule:
                elements.push_back(detail::render_hr(opts));
                break;
            case BlockTokenKind::MathBlock: {
                // Display math ($...$).  TS renders via KaTeX → HTML; in a
                // terminal we show the raw LaTeX centered and in a distinct
                // cyan color so it's visually recognized as a formula block.
                Element math_el = text(tok.math_content) |
                                  color(Color::CyanLight);
                elements.push_back(
                    hbox({filler(), std::move(math_el), filler()}));
                break;
            }
        }
    }

    if (elements.empty()) {
        return text("");
    }
    return vbox(std::move(elements));
}

/// Convenience: render with dim_color = true
[[nodiscard]] Element render_markdown_dim(std::string_view source) {
    MarkdownOptions opts;
    opts.dim_color = true;
    return render_markdown(source, opts);
}

Element StreamingMarkdown::update(std::string_view content) {
        // Reset if content was replaced (not a prefix extension)
        if (!content.starts_with(stable_prefix_)) {
            stable_prefix_.clear();
        }

        // Find a safe block boundary in the new content
        std::size_t boundary = find_block_boundary(content);

        // Advance stable prefix
        if (boundary > stable_prefix_.size()) {
            stable_prefix_ = std::string(content.substr(0, boundary));
        }

        std::string_view unstable = content.substr(stable_prefix_.size());

        Elements parts;
        if (!stable_prefix_.empty()) {
            parts.push_back(render_markdown(stable_prefix_));
        }
        if (!unstable.empty()) {
            parts.push_back(render_markdown(unstable));
        }
        return vbox(std::move(parts));
    }

void StreamingMarkdown::reset() { stable_prefix_.clear(); }

std::size_t StreamingMarkdown::find_block_boundary(
    std::string_view content) {

        std::size_t last_good = 0;
        bool in_code_fence = false;

        std::size_t pos = 0;
        while (pos < content.size()) {
            auto nl = content.find('\n', pos);
            if (nl == std::string_view::npos) break;

            std::string_view line;
            if (nl > pos) {
                line = content.substr(pos, nl - pos);
            }

            if (line.starts_with("```")) {
                if (in_code_fence) {
                    // End of code fence — boundary after this line
                    last_good = nl + 1;
                    in_code_fence = false;
                } else {
                    in_code_fence = true;
                }
            }

            // Double newline = paragraph boundary
            if (nl + 1 < content.size() && content[nl + 1] == '\n') {
                if (!in_code_fence) {
                    last_good = nl + 1;
                }
            }

            // Horizontal rule = block boundary
            if (line == "---" || line == "***" || line == "___") {
                if (!in_code_fence) {
                    last_good = nl + 1;
                }
            }

            pos = nl + 1;
        }

        return last_good;
    }

/// Clear the global markdown token cache.
/// Call this when memory pressure is high or on settings change.
void clear_markdown_cache() {
    detail::global_token_cache().clear();
}

/// Get current cache size (for diagnostics)
[[nodiscard]] std::size_t markdown_cache_size() {
    return detail::global_token_cache().size();
}

/// Get maximum cache size
[[nodiscard]] std::size_t markdown_cache_max_size() {
    return detail::global_token_cache().max_size();
}

} // namespace cc::ui
