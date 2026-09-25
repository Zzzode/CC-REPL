// markdown_render_impl.cpp - impl unit for cc.ui.visual.markdown (RFC 0001
// Phase C batch 6). Holds the FTXUI block/inline renderers: render_inlines,
// render_heading, the depth numbering helpers (number_to_letter,
// number_to_roman, get_list_number - in the pre-existing nested
// cc::ui::detail::detail namespace), render_ulist/olist/blockquote/table/hr.
// render_code_block lives in markdown_render_code_impl.cpp (it pulls in
// cc.ui.visual.code_highlight).
module;

#include <ftxui/dom/elements.hpp>

module cc.ui.visual.markdown;

import std;

namespace cc::ui {
namespace detail {

[[nodiscard]] Element render_inlines(
    const std::vector<InlineToken>& tokens,
    const MarkdownOptions& opts) {

    Elements elements;
    for (const auto& tok : tokens) {
        switch (tok.kind) {
            case InlineTokenKind::Text: {
                // TS REF: utils/markdown.ts linkifyIssueReferences — applied
                // to plain text tokens so owner/repo#NNN becomes clickable.
                auto linked = linkify_issue_references(tok.text, opts);
                for (auto& el : linked) {
                    elements.push_back(std::move(el));
                }
                break;
            }
            case InlineTokenKind::Bold: {
                Element el = text(tok.text) | bold;
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
            case InlineTokenKind::Italic: {
                // chalk.italic. FTXUI has no italic Screen style, so we use
                // dim as the closest visual proxy (italic on dark terminals
                // is itself low-contrast; dim is a reasonable approximation).
                Element el = text(tok.text) | dim;
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
            case InlineTokenKind::Code: {
                // codespan -> color('permission', theme). Dark theme
                // permission = rgb(87,105,247). No background inversion.
                Element el = text(tok.text) | color(Color::RGB(87, 105, 247));
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
            case InlineTokenKind::Link: {
                // TS REF: FullscreenLayout.tsx L630-667 + utils/markdown.ts
                // createHyperlink.  TS wraps link text in OSC 8 hyperlinks so
                // modern terminals (iTerm2, WezTerm, VS Code, GNOME Terminal)
                // render them as clickable → openBrowser/openPath.
                //
                // We use FTXUI's `hyperlink(url)` decorator instead of raw
                // OSC 8 text (cc::utils::make_hyperlink).  The decorator:
                //   1. Registers the URL with the Screen via
                //      RegisterHyperlink(), storing it in hyperlinks_[].
                //   2. Sets pixel.hyperlink = id for every pixel the link
                //      text occupies — so AppAdapter::OnEvent can detect
                //      clicks on hyperlinked text by calling
                //      screen.PixelAt(x,y).hyperlink and screen.Hyperlink(id).
                //   3. Emits the OSC 8 escape sequence \e]8;;url\e\\ in
                //      Print() so terminal-native OSC 8 handling works when
                //      mouse tracking is NOT intercepting clicks.
                //
                // When fullscreen mouse tracking IS active (the common case
                // for loom), AppAdapter::OnEvent detects left-button
                // releases at pixels with hyperlink != 0 and calls
                // cc::utils::try_open_hyperlink(url) — mirroring TS Ink's
                // ink.onHyperlinkClick callback.
                Element el = text(tok.text) | hyperlink(tok.url) | underlined;
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
            case InlineTokenKind::Escape: {
                Element el = text(tok.text);
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
            case InlineTokenKind::Math: {
                // Inline LaTeX math ($...$).  TS renders via KaTeX → HTML;
                // in a terminal we approximate with a distinct cyan color
                // so the user can tell it's been recognized as math rather
                // than plain text with visible $ delimiters.
                Element el = text(tok.text) | color(Color::CyanLight);
                if (opts.dim_color) el = el | dim;
                elements.push_back(std::move(el));
                break;
            }
        }
    }
    return elements.empty() ? text("") : hbox(std::move(elements));
}

[[nodiscard]] Element render_heading(const BlockToken& tok,
                                     const MarkdownOptions& opts) {
    auto el = render_inlines(tok.inlines, opts) | bold;
    if (tok.heading_level == 1) {
        // chalk.bold.italic.underline. FTXUI lacks italic, so bold+underline
        // is the closest visual proxy (the bold already differentiates h1
        // from body text; underline mirrors the TS attribute).
        el = el | underlined;
    }
    // h2 and h3+ are plain bold (no extra attributes).
    return vbox({el, text("")});
}

namespace detail {

[[nodiscard]] std::string number_to_letter(int n) {
    if (n <= 0) return "a";
    std::string result;
    while (n > 0) {
        --n;  // 0-indexed: 0→'a', 25→'z'
        result = static_cast<char>('a' + (n % 26)) + result;
        n /= 26;
    }
    return result;
}

[[nodiscard]] std::string number_to_roman(int n) {
    if (n <= 0 || n > 3999) return std::to_string(n);

    struct RomanPair { int value; const char* symbol; };
    static constexpr RomanPair roman_map[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
        {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
        {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
        {1, "i"}
    };

    std::string result;
    for (const auto& [value, symbol] : roman_map) {
        while (n >= value) {
            result += symbol;
            n -= value;
        }
    }
    return result;
}

[[nodiscard]] std::string get_list_number(int depth, int n) {
    switch (depth) {
        case 0:
        case 1:
            return std::to_string(n);
        case 2:
            return number_to_letter(n);
        case 3:
            return number_to_roman(n);
        default:
            // Depth 4+ wraps back to Arabic, matching TS default case.
            return std::to_string(n);
    }
}

} // namespace detail

[[nodiscard]] Element render_ulist(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    Elements items;
    for (std::size_t i = 0; i < tok.list_items.size(); ++i) {
        int depth = i < tok.list_depths.size() ? tok.list_depths[i] : 0;
        // Indent prefix: '  ' × depth
        std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
        Element indent_el = text(indent);
        if (opts.dim_color) indent_el = indent_el | dim;

        Element bullet = text("- ");
        if (opts.dim_color) bullet = bullet | dim;

        items.push_back(hbox({
            std::move(indent_el),
            std::move(bullet),
            render_inlines(tok.list_items[i], opts)
        }));
    }
    return vbox(std::move(items));
}

[[nodiscard]] Element render_olist(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    Elements items;

    // Track per-depth counters so numbering resets correctly when nesting
    // changes (e.g., depth-0 item #3 followed by depth-1 item #1).
    std::unordered_map<int, int> depth_counters;

    for (std::size_t i = 0; i < tok.list_items.size(); ++i) {
        int depth = i < tok.list_depths.size() ? tok.list_depths[i] : 0;
        int& counter = depth_counters[depth];
        ++counter;

        // Clear counters for deeper levels when returning to a shallower depth.
        // E.g., going depth 1 → depth 0 should reset depth 1's counter for
        // the next time it appears.
        for (auto it = depth_counters.begin(); it != depth_counters.end(); ) {
            if (it->first > depth) {
                it = depth_counters.erase(it);
            } else {
                ++it;
            }
        }

        std::string label = detail::get_list_number(depth, counter) + ". ";
        // Indent prefix: '  ' × depth
        std::string indent(static_cast<std::size_t>(depth) * 2, ' ');

        Element indent_el = text(indent);
        if (opts.dim_color) indent_el = indent_el | dim;

        Element num_el = text(label);
        if (opts.dim_color) num_el = num_el | dim;

        items.push_back(hbox({
            std::move(indent_el),
            std::move(num_el),
            render_inlines(tok.list_items[i], opts)
        }));
    }
    return vbox(std::move(items));
}

[[nodiscard]] Element render_blockquote(const BlockToken& tok,
                                        const MarkdownOptions& opts) {
    // tok.inlines may contain '\n' embedded when the quote spanned multiple
    // source lines (the lexer joins them with ' '); render each segment.
    // We re-split on any embedded newlines to mirror the per-line bar prefix.
    auto content_el = render_inlines(tok.inlines, opts);
    // chalk.italic on the content is unavailable in FTXUI; the dim bar alone
    // provides the blockquote visual cue (TS note: chalk.dim on text is
    // nearly invisible, so we keep content at normal brightness like TS).
    Element bar = text("\xE2\x96\x8E ") | dim;  // ▎ (U+258E) + space
    if (opts.dim_color) bar = bar | dim;
    return hbox({bar, content_el});
}

[[nodiscard]] Element render_table(const BlockToken& tok,
                                   const MarkdownOptions& opts) {
    auto display_width = [](std::string_view s) -> std::size_t {
        // ASCII width approximation (TS uses stringWidth which handles wide
        // CJK / combining chars). For markdown tables in this CLI, ASCII
        // width is the dominant case. Residual: wide-char cells.
        return s.size();
    };

    // Unicode box-drawing glyphs (UTF-8 encoded).
    constexpr std::string_view kHBar      = "\xE2\x94\x80";  // ─ U+2500
    constexpr std::string_view kVBar      = "\xE2\x94\x82";  // │ U+2502
    constexpr std::string_view kCornerTL  = "\xE2\x94\x8C";  // ┌ U+250C
    constexpr std::string_view kCornerTR  = "\xE2\x94\x90";  // ┐ U+2510
    constexpr std::string_view kCornerBL  = "\xE2\x94\x94";  // └ U+2514
    constexpr std::string_view kCornerBR  = "\xE2\x94\x98";  // ┘ U+2518
    constexpr std::string_view kTeeDown   = "\xE2\x94\xAC";  // ┬ U+252C
    constexpr std::string_view kTeeUp     = "\xE2\x94\xB4";  // ┴ U+2534
    constexpr std::string_view kTeeRight  = "\xE2\x94\x9C";  // ├ U+251C
    constexpr std::string_view kTeeLeft   = "\xE2\x94\xA4";  // ┤ U+2524
    constexpr std::string_view kCross     = "\xE2\x94\xBC";  // ┼ U+253C

    std::size_t num_cols = tok.table_headers.size();
    if (num_cols == 0) return text("");

    // Compute column widths (min 3).
    std::vector<std::size_t> col_widths(num_cols, 3);
    for (std::size_t c = 0; c < num_cols; ++c) {
        std::size_t w = display_width(tok.table_headers[c]);
        for (const auto& row : tok.table_rows) {
            if (c < row.size()) {
                w = std::max(w, display_width(row[c]));
            }
        }
        col_widths[c] = std::max(w, std::size_t{3});
    }

    auto pad_right = [](std::string s, std::size_t width) {
        if (s.size() < width) s.append(width - s.size(), ' ');
        return s;
    };
    auto pad_center = [](std::string s, std::size_t width) {
        // TS centers headers: spaces split evenly on left / right.
        if (s.size() >= width) return s;
        std::size_t diff = width - s.size();
        std::size_t left = diff / 2;
        std::size_t right = diff - left;
        std::string out;
        out.reserve(width);
        out.append(left, ' ');
        out += s;
        out.append(right, ' ');
        return out;
    };

    // Repeat a (possibly multi-byte) glyph N times into `out`.
    auto repeat_glyph = [](std::string& out, std::string_view g, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) out.append(g);
    };

    // Build a horizontal border: {corner_left} hbar*(width+2) {cross or end} ...
    auto make_border = [&](std::string_view l, std::string_view mid,
                           std::string_view cross, std::string_view r) {
        std::string line;
        line += l;
        for (std::size_t c = 0; c < num_cols; ++c) {
            repeat_glyph(line, kHBar, col_widths[c] + 2);  // +2 for inner spaces
            line += (c + 1 < num_cols) ? cross : r;
        }
        (void)mid;
        return line;
    };

    // Build a content row using │ cells. `cells` may be shorter than cols
    // (empty cells are padded).
    auto make_row = [&](const std::vector<std::string>& cells, bool is_header) {
        std::string line;
        line += kVBar;
        for (std::size_t c = 0; c < num_cols; ++c) {
            std::string cell = (c < cells.size()) ? cells[c] : std::string{};
            // Header is centered (TS), data uses left-align.
            std::string padded = is_header
                ? pad_center(std::move(cell), col_widths[c])
                : pad_right(std::move(cell), col_widths[c]);
            line += " " + std::move(padded) + " ";
            line += kVBar;
        }
        Element el = text(line);
        if (is_header) el = el | bold;
        if (opts.dim_color) el = el | dim;
        return el;
    };

    Elements rows;
    // Top border.
    {
        Element el = text(make_border(kCornerTL, kHBar, kTeeDown, kCornerTR));
        if (opts.dim_color) el = el | dim;
        rows.push_back(std::move(el));
    }
    // Header row.
    rows.push_back(make_row(tok.table_headers, /*is_header=*/true));
    // Header/data separator.
    {
        Element el = text(make_border(kTeeRight, kHBar, kCross, kTeeLeft));
        if (opts.dim_color) el = el | dim;
        rows.push_back(std::move(el));
    }
    // Data rows.
    for (const auto& row : tok.table_rows) {
        rows.push_back(make_row(row, /*is_header=*/false));
    }
    // Bottom border.
    {
        Element el = text(make_border(kCornerBL, kHBar, kTeeUp, kCornerBR));
        if (opts.dim_color) el = el | dim;
        rows.push_back(std::move(el));
    }

    return vbox(std::move(rows));
}

[[nodiscard]] Element render_hr(const MarkdownOptions& opts) {
    Element el = text("---");
    if (opts.dim_color) el = el | dim;
    return el;
}

} // namespace detail
} // namespace cc::ui
