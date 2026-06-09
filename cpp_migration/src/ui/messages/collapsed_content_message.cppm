/// @file collapsed_content_message.cppm
/// @brief Collapsed aggregator message — summary header "📦 Read 12 files,
/// 4,521 lines" + per-file table (path | lines | matches) + pagination for
/// >20 entries + expanded 10-line per-file preview via code_highlight.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.collapsed_content_message;

import cc.ui.code_highlight;

export namespace cc::ui::messages::collapsed_content {
using namespace ftxui;
namespace ch = cc::ui::code_highlight;

// ============================================================
// Types
// ============================================================

/// A single aggregated content entry (e.g. one file Read / Search result).
struct ContentEntry {
    std::string path;           // File path or URL
    std::string title;          // Optional short display override
    std::string kind;           // "Read", "Search", "Bash", "MCP", ...
    int total_lines{0};         // Lines in file / result
    int match_lines{0};         // Matching lines (for Search / Grep results)
    std::string preview;        // Optional raw preview snippet (first N lines)
    std::string language;       // For code_highlight when expanded
};

/// Rendering options
struct CollapsedContentOptions {
    std::vector<ContentEntry> entries;
    std::string aggregate_kind;    // "Read", "Read + Search", etc.
    bool group_collapsed{true};    // Top-level collapse state
    int table_page_size{20};       // Per UI5 spec: paginate when >20
    int table_page{0};             // 0-based
    std::optional<int> expanded_entry; // Which row is showing preview
    int preview_lines{10};         // Per UI5 spec
    int max_path_width{40};        // Path col truncation width

    std::function<void()> on_toggle;
    std::function<void(int page)> on_page_change;
    std::function<void(std::size_t idx)> on_toggle_entry;
};

// ============================================================
// Helpers
// ============================================================

struct Aggregates {
    std::size_t entry_count{0};
    std::uint64_t total_lines{0};
    std::uint64_t total_matches{0};
};

[[nodiscard]] inline Aggregates compute_aggregates(
    const std::vector<ContentEntry>& es) {
    Aggregates a{es.size(), 0, 0};
    for (const auto& e : es) {
        a.total_lines += static_cast<std::uint64_t>(std::max(0, e.total_lines));
        a.total_matches += static_cast<std::uint64_t>(std::max(0, e.match_lines));
    }
    return a;
}

[[nodiscard]] inline std::string format_thousands(std::uint64_t n) {
    std::string s = std::format("{}", n);
    std::string out;
    out.reserve(s.size() + (s.size() - 1) / 3);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i && ((s.size() - i) % 3) == 0) out.push_back(',');
        out.push_back(s[i]);
    }
    return out;
}

/// Truncate/pad a string to a fixed display width.
[[nodiscard]] inline std::string clamp_width(const std::string& s, int w) {
    if (w <= 0) return {};
    if (static_cast<int>(s.size()) <= w) return s + std::string(w - s.size(), ' ');
    if (w <= 3) return std::string(w, '.');
    return s.substr(0, static_cast<std::size_t>(w - 3)) + "..." + std::string(1, ' ');
}

// ============================================================
// Summary header
// ============================================================

[[nodiscard]] inline Element RenderCollapsedSummary(
    const CollapsedContentOptions& opts, const Aggregates& agg) {
    const auto& kind = opts.aggregate_kind;
    std::string label = kind.empty() ? std::string("content") : kind;

    Elements parts;
    parts.push_back(text(opts.group_collapsed ? "📦 " : "📂 ") | color(Color::BlueLight));
    parts.push_back(text(label + " ") | bold | color(Color::BlueLight));

    // "{N} files"
    if (!kind.empty() && kind.find("Search") == std::string::npos) {
        parts.push_back(
            text(std::format("{} file{}",
                             format_thousands(agg.entry_count),
                             agg.entry_count == 1 ? "" : "s"))
            | color(Color::White));
    } else {
        parts.push_back(
            text(std::format("{} result{}",
                             format_thousands(agg.entry_count),
                             agg.entry_count == 1 ? "" : "s"))
            | color(Color::White));
    }

    parts.push_back(text(", ") | dim);
    parts.push_back(
        text(std::format("{} lines", format_thousands(agg.total_lines)))
        | color(Color::GrayLight));
    if (agg.total_matches > 0) {
        parts.push_back(text(", ") | dim);
        parts.push_back(
            text(std::format("{} matches", format_thousands(agg.total_matches)))
            | color(Color::GreenLight));
    }

    parts.push_back(filler());
    parts.push_back(text(opts.group_collapsed ? "▸ [enter] expand "
                                              : "▾ [enter] collapse ")
                    | dim | color(Color::GrayDark));

    return hbox(parts);
}

// ============================================================
// File table
// ============================================================

[[nodiscard]] inline Element RenderFileTable(
    const CollapsedContentOptions& opts) {
    const auto n = opts.entries.size();
    if (n == 0) {
        return text("  (no entries)") | dim;
    }

    const int psize = std::max(1, opts.table_page_size);
    const int max_page = std::max(0, (static_cast<int>(n) + psize - 1) / psize - 1);
    const int page = std::clamp(opts.table_page, 0, max_page);
    const std::size_t s = static_cast<std::size_t>(page * psize);
    const std::size_t e = std::min(n, s + static_cast<std::size_t>(psize));

    // Column widths
    const int KIND_W = 8;
    const int PATH_W = std::max(18, opts.max_path_width);
    const int LINES_W = 10;
    const int MATCH_W = 10;

    Elements rows;

    // Header row
    {
        auto head = hbox({
            text("  "),
            text(clamp_width("Kind", KIND_W)) | bold | color(Color::Cyan),
            text(clamp_width("File path", PATH_W)) | bold | color(Color::Cyan),
            text(clamp_width("Lines", LINES_W)) | bold | color(Color::Cyan),
            text(clamp_width("Matches", MATCH_W)) | bold | color(Color::Cyan),
        });
        rows.push_back(head);
        rows.push_back(separator());
    }

    for (std::size_t i = s; i < e; ++i) {
        const auto& entry = opts.entries[i];
        const bool expanded = opts.expanded_entry.has_value()
            && *opts.expanded_entry == static_cast<int>(i);
        const std::string idx = std::format("{:>2}", i + 1);
        std::string disp_path = entry.title.empty() ? entry.path : entry.title;

        // Kind column
        Element kind_el = text(clamp_width(entry.kind, KIND_W))
            | color(Color::MagentaLight);
        if (expanded) kind_el = kind_el | bold;

        // Path
        Element path_el = text(clamp_width(disp_path, PATH_W));

        // Lines + Matches
        Element lines_el = text(
            clamp_width(format_thousands(static_cast<std::uint64_t>(
                            std::max(0, entry.total_lines))), LINES_W))
            | dim;
        Element matches_el;
        if (entry.match_lines > 0) {
            matches_el = text(
                clamp_width(format_thousands(static_cast<std::uint64_t>(
                                entry.match_lines)), MATCH_W))
                | color(Color::Green);
        } else {
            matches_el = text(clamp_width("-", MATCH_W)) | dim;
        }

        auto row = hbox({
            text(idx + " ") | dim,
            std::move(kind_el),
            std::move(path_el),
            std::move(lines_el),
            std::move(matches_el),
        });

        if (expanded) {
            row = row | bgcolor(Color::RGB(18, 22, 32));
        }
        rows.push_back(std::move(row));

        // --- Expanded preview ---
        if (expanded && !entry.preview.empty()) {
            Elements preview_rows;
            ch::CodeHighlightOptions ch_opts;
            ch_opts.source = entry.preview;
            ch_opts.language = entry.language;
            ch_opts.show_line_numbers = true;
            ch_opts.visible_lines = std::max(5, opts.preview_lines);
            auto hl = ch::highlight_source(ch_opts.source, ch_opts.language);
            auto preview_el = ch::RenderCodeHighlight(ch_opts, hl);
            // indent + wrap
            rows.push_back(hbox({
                text("      └─ preview ──") | dim | color(Color::GrayDark),
            }));
            rows.push_back(
                hbox({
                    text("    "),
                    preview_el | color(Color::GrayDark),
                }));
        }
    }

    // Footer controls
    rows.push_back(separator());
    {
        Elements foot;
        foot.push_back(text(" [Tab/↑/↓] row  [enter] preview  [←/→] page ") | dim);
        foot.push_back(filler());
        if (n > static_cast<std::size_t>(psize)) {
            foot.push_back(
                text(std::format(" rows {}-{} of {}, page {}/{} ",
                                 s + 1, e, n, page + 1, max_page + 1))
                | dim | color(Color::GrayDark));
        } else {
            foot.push_back(text(std::format(" {} rows ", n))
                           | dim | color(Color::GrayDark));
        }
        rows.push_back(hbox(foot));
    }

    return vbox(rows);
}

// ============================================================
// Top-level renderer
// ============================================================

[[nodiscard]] inline Element RenderCollapsedContent(
    const CollapsedContentOptions& opts) {
    auto agg = compute_aggregates(opts.entries);
    auto header = RenderCollapsedSummary(opts, agg);

    Elements content = {header};

    if (!opts.group_collapsed) {
        content.push_back(separator());
        content.push_back(RenderFileTable(opts));
    }

    return vbox(content) | borderLight | color(Color::BlueLight);
}

// ============================================================
// Interactive Component
// ============================================================

[[nodiscard]] inline Component CollapsedContentMessage(
    CollapsedContentOptions options) {
    struct State {
        CollapsedContentOptions opts;
        int selected_row{-1};     // absolute index; -1 means none
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);

    return Renderer([s] { return RenderCollapsedContent(s->opts); })
        | CatchEvent([s](Event event) -> bool {
              auto& o = s->opts;
              const int n = static_cast<int>(o.entries.size());
              const int psize = std::max(1, o.table_page_size);
              const int max_page = std::max(0, (n + psize - 1) / psize - 1);

              // Global collapse
              if (event == Event::Character(' ')) {
                  o.group_collapsed = !o.group_collapsed;
                  if (o.on_toggle) o.on_toggle();
                  return true;
              }

              if (o.group_collapsed) {
                  if (event == Event::Return) {
                      o.group_collapsed = false;
                      if (o.on_toggle) o.on_toggle();
                      return true;
                  }
                  return false;
              }

              // Page control
              if (event == Event::ArrowRight || event == Event::Character('l')) {
                  if (o.table_page < max_page) {
                      o.table_page++;
                      s->selected_row = o.table_page * psize;
                      if (o.on_page_change) o.on_page_change(o.table_page);
                      return true;
                  }
              }
              if (event == Event::ArrowLeft || event == Event::Character('h')) {
                  if (o.table_page > 0) {
                      o.table_page--;
                      s->selected_row = o.table_page * psize;
                      if (o.on_page_change) o.on_page_change(o.table_page);
                      return true;
                  }
              }

              if (n == 0) return false;

              // Row selection
              const int start = o.table_page * psize;
              const int end   = std::min(n, start + psize);
              if (event == Event::Tab || event == Event::ArrowDown ||
                  event == Event::Character('j')) {
                  if (s->selected_row < start || s->selected_row >= end - 1) {
                      s->selected_row = start;
                  } else {
                      s->selected_row++;
                  }
                  return true;
              }
              if (event == Event::ArrowUp || event == Event::Character('k')) {
                  if (s->selected_row <= start) {
                      s->selected_row = end - 1;
                  } else {
                      s->selected_row--;
                  }
                  return true;
              }

              // Preview toggle on selected
              if (event == Event::Return) {
                  int sel = s->selected_row;
                  if (sel < start || sel >= end) sel = start;
                  if (o.expanded_entry && *o.expanded_entry == sel) {
                      o.expanded_entry = std::nullopt;
                  } else {
                      o.expanded_entry = sel;
                  }
                  if (o.on_toggle_entry) o.on_toggle_entry(sel);
                  return true;
              }
              return false;
          });
}

} // namespace cc::ui::messages::collapsed_content
