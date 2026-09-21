/// @file message_image.cppm
/// @brief Image message component — displays image attachments in user messages.
///
/// Visual layout (user-side, right-aligned):
///   ┌──────────────────────────────────────┐
///   │  🖼 [Image #42]                      │
///   │  1024×768  PNG · 256 KB              │
///   │  📁 photo.png                        │
///   └──────────────────────────────────────┘
///
/// TS REF: src/components/messages/UserImageMessage.tsx
///   TS renders just `[Image #N]` as a clickable hyperlink.  The CPP port
///   adds supplementary metadata (dimensions, file size, source) below the
///   label for better UX — the label itself is TS-faithful.
///
/// Features:
///   - Terminal hyperlinks (OSC 8) when supported → click to open file
///   - Source type indicators: file / URL / base64 / clipboard
///   - Dimensions, file size, and MIME type display
///   - Alt text support
///   - Integrates with image_store for stored image lookup
// ────────────────────────────────────────────────────────────────────────
module;

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.message_image;

import cc.ui.messages.message_timestamp;
import cc.utils.image_store;
import cc.utils.hyperlink;

export namespace cc::ui::messages::image {

using namespace ftxui;
namespace fs = std::filesystem;
using cc::utils::image_store::StoredImage;
using cc::utils::supports_hyperlinks;

// ============================================================
// Types
// ============================================================

/// Image source type
enum class ImageSource : std::uint8_t {
    File,       // Local file path
    URL,        // Remote URL
    Base64,     // Inline base64 data
    Clipboard,  // Pasted from clipboard
    StoreId,    // Reference to image_store by id
};

/// Data for an image message
struct ImageMessageData {
    std::string source;              // Path, URL, identifier, or image_store id
    ImageSource source_type = ImageSource::File;
    std::optional<std::string> alt_text;
    std::optional<size_t> width;
    std::optional<size_t> height;
    std::optional<size_t> file_size;
    std::string media_type;          // e.g., "image/png"
    std::string file_name;           // Display filename (for list preview)
    std::optional<std::string> image_id;  // Image store id, if applicable
    bool add_margin = false;         // True when image starts a new user turn
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
};

// ============================================================
// Helpers
// ============================================================

/// Format file size for display
[[nodiscard]] inline std::string format_size(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

/// Source type icon
[[nodiscard]] inline const char* source_icon(ImageSource s) {
    switch (s) {
        case ImageSource::File:      return "📁";
        case ImageSource::URL:       return "🌐";
        case ImageSource::Base64:    return "📋";
        case ImageSource::Clipboard: return "📎";
        case ImageSource::StoreId:   return "🖼️";
    }
    return "🖼️";
}

/// Try to resolve image metadata from image_store if source_type is StoreId.
/// Populates missing fields in-place.
inline void resolve_from_store(ImageMessageData& data) {
    if (data.source_type != ImageSource::StoreId) return;

    auto img = cc::utils::image_store::get_image(data.source);
    if (!img) return;

    const StoredImage& s = *img;
    if (!data.width)  data.width  = s.width;
    if (!data.height) data.height = s.height;
    if (!data.file_size) data.file_size = s.size_bytes;
    if (data.media_type.empty()) data.media_type = s.mime_type;
    if (data.file_name.empty()) {
        data.file_name = fs::path(s.path).filename().string();
    }
    if (data.source.empty()) data.source = s.path;
    if (!data.image_id) data.image_id = s.id;
}

// ============================================================
// Stateless Element renderer
// ============================================================

/// Render a stateless image message element (for list previews etc.)
/// TS REF: src/components/messages/UserImageMessage.tsx
///   TS: `const label = imageId ? \`[Image #${imageId}]\` : "[Image]"`
///   rendered as <Text>{label}</Text> optionally wrapped in <Link url={...}>.
///
/// TS VISUAL PARITY (2026-07-04): TS renders just 1 line for image attachments.
/// The CPP port previously added 3 extra lines (meta, source, alt/base64) which
/// made the card too tall and caused "间距很大" (user report).  We now keep
/// the card compact: label + inline meta on one line, source only for files.
[[nodiscard]] inline Element render(const ImageMessageData& data) {
    // --- Primary label: [Image #N] (TS-faithful) ---
    // TS REF: UserImageMessage.tsx L26
    std::string label = "[Image";
    if (data.image_id) {
        label += " #" + *data.image_id;
    }
    label += "]";

    // Build inline meta suffix: "png · 1024×768 · 17 KB" (dim, same line)
    std::string meta_suffix;
    {
        if (!data.media_type.empty()) {
            auto mt = data.media_type;
            if (mt.starts_with("image/")) mt = mt.substr(6);
            meta_suffix += mt;
        }
        if (data.width && data.height) {
            if (!meta_suffix.empty()) meta_suffix += " · ";
            meta_suffix += std::to_string(*data.width) + "\xC3\x97" +
                           std::to_string(*data.height);
        }
        if (data.file_size) {
            if (!meta_suffix.empty()) meta_suffix += " · ";
            meta_suffix += format_size(*data.file_size);
        }
    }

    Element label_el;
    if ((data.source_type == ImageSource::File ||
         data.source_type == ImageSource::StoreId) &&
        !data.source.empty() && supports_hyperlinks()) {
        // TS REF: UserImageMessage.tsx L30 — clickable Link when supported
        std::string file_url = "file://" + data.source;
        label_el = hbox({
            text("\xf0\x9f\x96\xbc ") | color(Color::BlueLight),
            text(label) | bold | color(Color::Cyan) | hyperlink(file_url),
            meta_suffix.empty() ? text("")
                : text("  " + meta_suffix) | dim | color(Color::GrayLight),
        });
    } else {
        label_el = hbox({
            text("\xf0\x9f\x96\xbc ") | color(Color::BlueLight),
            text(label) | bold | color(Color::BlueLight),
            meta_suffix.empty() ? text("")
                : text("  " + meta_suffix) | dim | color(Color::GrayLight),
        });
    }

    // Build the card: always starts with the label line (1 row).
    Elements card_parts;
    card_parts.push_back(std::move(label_el));

    // Source line: show filename/path for all source types except Base64.
    // Clipboard pastes show 📎 + filename so users know which paste it is.
    // File images show 📁 + path.
    if (data.source_type != ImageSource::Base64) {
        std::string display_source;
        if (data.source_type == ImageSource::Clipboard) {
            // For clipboard, prefer file_name over source (source is a
            // temp path the user doesn't care about).
            display_source = data.file_name;
        } else {
            display_source = data.source;
            if (display_source.empty() && !data.file_name.empty()) {
                display_source = data.file_name;
            }
        }
        if (display_source.size() > 50) {
            display_source = "\xe2\x80\xa6" + display_source.substr(display_source.size() - 49);
        }
        if (!display_source.empty()) {
            std::string file_url = "file://" + data.source;
            card_parts.push_back(
                hbox({
                    text("  ") | dim,
                    text(std::string(source_icon(data.source_type)) + " ") | dim,
                    text(display_source) | color(Color::Cyan) |
                        (supports_hyperlinks() ? hyperlink(file_url) : nothing),
                })
            );
        }
    }

    // NOTE: deliberately NO "Alt:" line.  The alt_text field was being abused
    // to stuff base64 PNG data (repl_screen.cppm L825-831) for a planned
    // "ASCII-art thumbnail" feature that was never implemented.  Showing raw
    // base64 prefix "Alt: iVBORw0KGgo..." is worse than useless — it wastes a
    // line and confuses users.  Removed 2026-07-04 per spacing bug report.

    return vbox(std::move(card_parts));
}

// ============================================================
// Interactive Component
// ============================================================

class ImageMessageComponent : public ComponentBase {
  public:
    using OnClickFn = std::function<void()>;
    using OnCopyFn  = std::function<void(std::string_view)>;

    explicit ImageMessageComponent(ImageMessageData data,
                                   OnClickFn on_click = nullptr,
                                   OnCopyFn  on_copy  = nullptr)
        : data_(std::move(data)),
          on_click_(std::move(on_click)),
          on_copy_(std::move(on_copy)) {}

    Element Render() override {
        auto body = render(data_);

        // Right-side header: timestamp + role tag
        auto header_right = hbox({
            text(" ") | size(WIDTH, EQUAL, 1),
            text(render_timestamp(data_.timestamp)) | dim | color(Color::GrayDark),
            text(" 👤 You") | color(Color::Blue),
        });

        // User message is right-aligned
        auto content = vbox({
            hbox({
                filler(),
                header_right,
            }),
            hbox({
                filler(),
                vbox({
                    std::move(body)
                        | (hovered_ ? bgcolor(Color::DarkBlue) : nothing)
                        | borderLight | color(Color::Blue),
                }) | flex_shrink,
            }),
        });

        // When add_margin is true, add top margin (image starts a new turn)
        if (data_.add_margin) {
            return vbox({
                separatorEmpty(),
                std::move(content),
            });
        }
        return content;
    }

    bool OnEvent(Event event) override {
        if (event == Event::Return) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        // Ctrl+C to copy image source/path
        if (event == Event::Special({3})) {
            if (on_copy_) on_copy_(data_.source);
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Released) {
            hovered_ = !hovered_;
            if (on_click_) on_click_();
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    ImageMessageData data_;
    OnClickFn on_click_;
    OnCopyFn  on_copy_;
    bool hovered_ = false;
};

/// Factory function to create an interactive image message component.
[[nodiscard]] inline Component MakeImageMessage(
    ImageMessageData data,
    ImageMessageComponent::OnClickFn on_click = nullptr,
    ImageMessageComponent::OnCopyFn  on_copy  = nullptr) {

    // Resolve store metadata before rendering
    resolve_from_store(data);
    return Make<ImageMessageComponent>(std::move(data),
                                        std::move(on_click),
                                        std::move(on_copy));
}

/// Convenience: render a stateless image message bubble (for previews).
[[nodiscard]] inline Element RenderImageBubble(const ImageMessageData& data) {
    ImageMessageData d = data;
    resolve_from_store(d);
    return render(d) | borderLight | color(Color::Blue);
}

} // namespace cc::ui::messages::image
