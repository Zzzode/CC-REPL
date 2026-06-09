/// @file attachment_message.cppm
/// @brief Attachment card grid — supports file/image/table/text/audio types
/// with type icons, size formatting, ASCII thumbnail art, pagination for >10
/// items, and per-card interactions (preview / open / delete).
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <array>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.attachment_message;

export namespace cc::ui::messages::attachment_message {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Broad attachment class used for icons, coloring, and thumbnail strategy
enum class AttachmentKind : std::uint8_t {
    File,        // Generic file (📄)
    Image,       // PNG/JPG/GIF etc. (🖼️)
    Table,       // CSV, parquet preview, resultsets (📊)
    Text,        // Plain text snippets, notes (📝)
    Audio,       // WAV/MP3 etc. (🎵)
    PDF,         // PDF reference
    Video,       // Video file
    Archive,     // zip/tar/gz
    Memory,      // nested memory / knowledge entry
    ToolArtifact,// tool/result artifact
};

/// A single attachment entry
struct AttachmentItem {
    std::string id;               // Stable identifier
    std::string name;             // File / card name
    std::string path;             // Full filesystem path (empty if virtual)
    std::string mime_or_ext;      // "image/png", ".md", etc.
    AttachmentKind kind{AttachmentKind::File};

    std::size_t byte_count{0};    // 0 → hide
    std::optional<int> line_count;

    // Optional ASCII thumbnail rows for Image/Table kind.
    // Row length should be kept ≤ ~30 cols for sane card widths.
    std::vector<std::string> ascii_thumbnail;

    // Short one-line caption shown under thumbnail (e.g. image resolution).
    std::string caption;

    bool can_delete{true};
    bool can_preview{true};
    bool can_open{true};
};

/// Options for the attachment grid
struct AttachmentGridOptions {
    std::vector<AttachmentItem> items;
    std::string title;                     // Optional header title
    int cols{2};                           // 1 / 2 / 3 column layout
    int page_size{10};                     // Paginate when >10 per UI5 spec
    int page{0};                           // Current page (0-based)
    int thumbnail_rows{3};                 // Rows of ▓ art
    std::optional<int> selected_idx;       // Currently focused item index
    bool show_controls{true};

    // Callbacks — index is absolute index in items array
    std::function<void(std::size_t idx)> on_preview;   // 'p' / Return on focused
    std::function<void(std::size_t idx)> on_open;      // 'o'
    std::function<void(std::size_t idx)> on_delete;    // 'd'
    std::function<void(int page)> on_page_change;
};

// ============================================================
// Helpers
// ============================================================

inline std::string format_bytes(std::size_t n) {
    if (n >= 1024ULL * 1024 * 1024) {
        return std::format("{:.2f}GB", n / (1024.0 * 1024.0 * 1024.0));
    }
    if (n >= 1024ULL * 1024) {
        return std::format("{:.2f}MB", n / (1024.0 * 1024.0));
    }
    if (n >= 1024) return std::format("{:.1f}KB", n / 1024.0);
    return std::format("{}B", n);
}

inline const char* kind_icon(AttachmentKind k) {
    switch (k) {
        case AttachmentKind::File:         return "📄";
        case AttachmentKind::Image:        return "🖼️";
        case AttachmentKind::Table:        return "📊";
        case AttachmentKind::Text:         return "📝";
        case AttachmentKind::Audio:        return "🎵";
        case AttachmentKind::PDF:          return "📕";
        case AttachmentKind::Video:        return "🎬";
        case AttachmentKind::Archive:      return "🗜️";
        case AttachmentKind::Memory:       return "🧠";
        case AttachmentKind::ToolArtifact: return "🧰";
    }
    return "📄";
}

inline Color kind_color(AttachmentKind k) {
    switch (k) {
        case AttachmentKind::File:         return Color::BlueLight;
        case AttachmentKind::Image:        return Color::Magenta;
        case AttachmentKind::Table:        return Color::GreenLight;
        case AttachmentKind::Text:         return Color::White;
        case AttachmentKind::Audio:        return Color::Cyan;
        case AttachmentKind::PDF:          return Color::Red;
        case AttachmentKind::Video:        return Color::Yellow;
        case AttachmentKind::Archive:      return Color::Orange1;
        case AttachmentKind::Memory:       return Color::DeepPink1Bis;
        case AttachmentKind::ToolArtifact: return Color::SteelBlue;
    }
    return Color::White;
}

/// Generate ASCII ▓-art rows of W x H that vary deterministically based on id.
inline std::vector<std::string> generate_default_thumbnail(
    AttachmentKind kind, std::string_view seed, int rows, int cols) {
    // Cheap deterministic PRNG from seed bytes.
    std::uint32_t h = 2166136261u;
    for (char c : seed) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    static constexpr std::array<char, 6> shades = {' ', '░', '▒', '▓', '█', '▓'};
    std::vector<std::string> out(rows);
    for (int y = 0; y < rows; ++y) {
        out[y].reserve(cols);
        for (int x = 0; x < cols; ++x) {
            h = h * 1103515245u + 12345u;
            int s = (h >> 13) & 0x7;
            // Bias by kind: images dense, tables lighter, etc.
            switch (kind) {
                case AttachmentKind::Image: s = std::min<int>(5, (s & 3) + 2); break;
                case AttachmentKind::Table: s = s & 2; break;
                case AttachmentKind::Audio: {
                    // Vertical waveform-ish bars
                    int bar = (h >> 8) % cols;
                    s = (std::abs(x - bar) < 2) ? 5 : (s & 1);
                    break;
                }
                default: s = s & 3; break;
            }
            out[y].push_back(shades[s % shades.size()]);
        }
    }
    return out;
}

// ============================================================
// Single card rendering
// ============================================================

/// Render one attachment card (non-interactive Element)
[[nodiscard]] inline Element RenderAttachmentCard(
    const AttachmentItem& a, bool focused, int thumbnail_rows) {

    // Thumbnail block
    Elements thumb_rows;
    const auto& art = a.ascii_thumbnail.empty()
        ? a.kind == AttachmentKind::Image ||
          a.kind == AttachmentKind::Table ||
          a.kind == AttachmentKind::Audio
            ? (std::vector<std::string>)generate_default_thumbnail(
                  a.kind, a.id.empty() ? a.name : a.id, thumbnail_rows, 28)
            : a.ascii_thumbnail
        : a.ascii_thumbnail;
    for (const auto& row : art) thumb_rows.push_back(text(row) | dim);
    if (thumb_rows.empty()) {
        // Text-style fallback: one-line "icon" glyph block
        thumb_rows.push_back(
            text(std::string(28, ' ')) | color(Color::GrayDark));
    }

    // Name + size
    Elements meta;
    meta.push_back(hbox({
        text(kind_icon(a.kind)),
        text(" "),
        text(a.name) | bold | color(kind_color(a.kind))
            | size(WIDTH, EQUAL, 22), // clamp width
    }));

    std::string right_side;
    if (a.byte_count > 0) {
        right_side += format_bytes(a.byte_count);
    }
    if (a.line_count) {
        if (!right_side.empty()) right_side += " · ";
        right_side += std::format("{} lines", *a.line_count);
    }
    if (!right_side.empty()) {
        meta.push_back(text("  " + right_side) | dim | color(Color::GrayLight));
    }

    if (!a.mime_or_ext.empty() || !a.caption.empty()) {
        std::string line;
        if (!a.mime_or_ext.empty()) line += a.mime_or_ext;
        if (!a.caption.empty()) {
            if (!line.empty()) line += " · ";
            line += a.caption;
        }
        meta.push_back(text("  " + line) | dim | color(Color::GrayDark));
    }

    // Action hint row
    Elements actions;
    if (a.can_preview) actions.push_back(text("[p]vw ") | dim);
    if (a.can_open)    actions.push_back(text("[o]pn ") | dim);
    if (a.can_delete)  actions.push_back(text("[d]el ") | dim);
    meta.push_back(hbox(actions) | color(Color::GrayDark));

    auto card = vbox({
        vbox(thumb_rows) | border | color(kind_color(a.kind)) | center,
        separator(),
        vbox(meta),
    });

    if (focused) {
        card = card | borderRounded | inverted;
    } else {
        card = card | borderRounded | color(Color::GrayDark);
    }
    return card;
}

// ============================================================
// Top-level grid renderer
// ============================================================

/// Render the full attachment grid. Returns Element.
[[nodiscard]] inline Element RenderAttachmentGrid(const AttachmentGridOptions& opts) {
    const std::size_t total = opts.items.size();
    const int page_size = std::max(1, opts.page_size);
    const int max_page = std::max(0, (static_cast<int>(total) + page_size - 1) / page_size - 1);
    const int page = std::clamp(opts.page, 0, max_page);
    const std::size_t start = static_cast<std::size_t>(page * page_size);
    const std::size_t end   = std::min(total, start + page_size);

    Elements all;

    // --- Header ---
    {
        Elements h;
        h.push_back(text("📎 ") | color(Color::Cyan));
        if (!opts.title.empty()) {
            h.push_back(text(opts.title) | bold | color(Color::Cyan));
        } else {
            h.push_back(text(std::format("Attachments ({})", total))
                        | bold | color(Color::Cyan));
        }
        h.push_back(filler());
        if (total > static_cast<std::size_t>(page_size)) {
            h.push_back(text(std::format(" page {}/{}", page + 1, max_page + 1))
                        | dim);
            h.push_back(text("  [←/→] page") | dim | color(Color::GrayDark));
        }
        all.push_back(hbox(h));
        all.push_back(separator());
    }

    if (total == 0) {
        all.push_back(text("  (no attachments)") | dim);
        return vbox(all);
    }

    // --- Grid rows ---
    const int cols = std::max(1, opts.cols);
    Elements card_row;
    for (std::size_t i = start; i < end; ++i) {
        const bool focused = opts.selected_idx.has_value()
            && *opts.selected_idx == static_cast<int>(i);
        auto card = RenderAttachmentCard(opts.items[i], focused, opts.thumbnail_rows);
        card_row.push_back(card | flex);
        if ((i - start + 1) % static_cast<std::size_t>(cols) == 0) {
            all.push_back(hflow(card_row));
            card_row.clear();
        }
    }
    if (!card_row.empty()) {
        // Pad row with empty fillers so hflow wraps uniformly.
        while (card_row.size() < static_cast<std::size_t>(cols)) {
            card_row.push_back(filler());
        }
        all.push_back(hflow(card_row));
    }

    // --- Footer controls ---
    if (opts.show_controls) {
        Elements foot;
        foot.push_back(text(" [Tab/↑/↓] select ") | dim);
        foot.push_back(text("[p]review ") | dim);
        foot.push_back(text("[o]pen ") | dim);
        foot.push_back(text("[d]elete") | dim);
        foot.push_back(filler());
        if (total > static_cast<std::size_t>(end)) {
            foot.push_back(
                text(std::format(" …{} more on next page", total - end))
                | color(Color::GrayDark) | dim);
        }
        all.push_back(separator());
        all.push_back(hbox(foot));
    }

    return vbox(all);
}

// ============================================================
// Interactive Component
// ============================================================

[[nodiscard]] inline Component AttachmentGrid(AttachmentGridOptions options) {
    struct State {
        AttachmentGridOptions opts;
    };
    auto s = std::make_shared<State>();
    s->opts = std::move(options);

    return Renderer([s] { return RenderAttachmentGrid(s->opts); })
        | CatchEvent([s](Event event) -> bool {
              auto& o = s->opts;
              const int total = static_cast<int>(o.items.size());
              const int page_size = std::max(1, o.page_size);
              const int max_page  = std::max(0, (total + page_size - 1) / page_size - 1);

              if (total == 0) return false;

              // Selection
              const int sel = o.selected_idx.value_or(-1);
              const int start = o.page * page_size;
              const int end   = std::min(total, start + page_size);

              if (event == Event::Tab || event == Event::ArrowDown ||
                  event == Event::Character('j')) {
                  int next;
                  if (sel < start || sel >= end - 1) next = start;
                  else next = sel + 1;
                  o.selected_idx = next;
                  return true;
              }
              if (event == Event::ArrowUp || event == Event::Character('k')) {
                  int prev;
                  if (sel <= start) prev = end - 1;
                  else prev = sel - 1;
                  o.selected_idx = prev;
                  return true;
              }

              // Pagination
              if (event == Event::ArrowRight || event == Event::Character('l')) {
                  if (o.page < max_page) {
                      o.page++;
                      if (o.on_page_change) o.on_page_change(o.page);
                      return true;
                  }
              }
              if (event == Event::ArrowLeft || event == Event::Character('h')) {
                  if (o.page > 0) {
                      o.page--;
                      if (o.on_page_change) o.on_page_change(o.page);
                      return true;
                  }
              }

              // Actions on selected
              const int act = o.selected_idx.value_or(-1);
              if (act < 0 || act >= total) return false;
              const auto& item = o.items[act];

              if (event == Event::Return) {
                  if (item.can_preview && o.on_preview)  o.on_preview(act);
                  else if (item.can_open && o.on_open)   o.on_open(act);
                  return true;
              }
              if (event == Event::Character('p') || event == Event::Character('P')) {
                  if (item.can_preview && o.on_preview) { o.on_preview(act); return true; }
              }
              if (event == Event::Character('o') || event == Event::Character('O')) {
                  if (item.can_open && o.on_open) { o.on_open(act); return true; }
              }
              if (event == Event::Character('d') || event == Event::Character('D')) {
                  if (item.can_delete && o.on_delete) {
                      o.on_delete(act);
                      // Recompute selection after removal; host may re-sync.
                      if (o.selected_idx && *o.selected_idx >= (int)o.items.size()) {
                          o.selected_idx = o.items.empty()
                              ? std::nullopt
                              : std::optional<int>(0);
                      }
                      return true;
                  }
              }
              return false;
          });
}

} // namespace cc::ui::messages::attachment_message
