// Image message - displays image attachment placeholder in terminal
module;

#include <string>
#include <optional>
#include <cstdint>
#include <format>

#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_image;

export namespace cc::ui::messages::image {
using namespace ftxui;

/// Image source type
enum class ImageSource : std::uint8_t {
    File,       // Local file path
    URL,        // Remote URL
    Base64,     // Inline base64 data
    Clipboard   // Pasted from clipboard
};

/// Data for an image message
struct ImageMessageData {
    std::string source;              // Path, URL, or identifier
    ImageSource source_type = ImageSource::File;
    std::optional<std::string> alt_text;
    std::optional<size_t> width;
    std::optional<size_t> height;
    std::optional<size_t> file_size;
    std::string media_type;          // e.g., "image/png"
};

/// Format file size for display
[[nodiscard]] inline std::string format_size(size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

/// Render an image message (terminal placeholder since we can't display images inline)
[[nodiscard]] inline Element render(const ImageMessageData& data) {
    Elements info_parts;
    
    // Source type indicator
    std::string type_icon;
    switch (data.source_type) {
        case ImageSource::File:      type_icon = "📁"; break;
        case ImageSource::URL:       type_icon = "🌐"; break;
        case ImageSource::Base64:    type_icon = "📋"; break;
        case ImageSource::Clipboard: type_icon = "📎"; break;
    }
    
    info_parts.push_back(text(type_icon + " ") | dim);
    info_parts.push_back(text("Image") | bold);
    
    // Dimensions
    if (data.width && data.height) {
        info_parts.push_back(
            text(std::format(" ({}x{})", *data.width, *data.height)) | dim);
    }
    
    // File size
    if (data.file_size) {
        info_parts.push_back(text(" " + format_size(*data.file_size)) | dim);
    }
    
    // Media type
    if (!data.media_type.empty()) {
        info_parts.push_back(text(" [" + data.media_type + "]") | color(Color::GrayDark));
    }
    
    auto header = hbox(info_parts);
    
    Elements elements = {header};
    
    // Source path
    std::string source_display = data.source;
    if (source_display.size() > 60) {
        source_display = "..." + source_display.substr(source_display.size() - 57);
    }
    elements.push_back(text("  " + source_display) | dim | color(Color::Cyan));
    
    // Alt text
    if (data.alt_text && !data.alt_text->empty()) {
        elements.push_back(text("  Alt: " + *data.alt_text) | dim);
    }
    
    return vbox(elements) | borderLight | color(Color::Blue);
}

} // namespace cc::ui::messages::image
