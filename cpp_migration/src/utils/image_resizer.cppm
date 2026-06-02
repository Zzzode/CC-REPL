module;
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

export module cc.utils.image_resizer;

export namespace cc::utils {

struct ImageDimensions {
    int width;
    int height;
};

// Get image dimensions from raw data (supports PNG and JPEG headers)
std::optional<ImageDimensions> get_image_dimensions(std::span<uint8_t> data) {
    if (data.size() < 24) return std::nullopt;

    // PNG: check signature and read IHDR chunk
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        if (data.size() < 24) return std::nullopt;
        // Width at offset 16, Height at offset 20 (big-endian)
        int width = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        int height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
        return ImageDimensions{width, height};
    }

    // JPEG: scan for SOF0/SOF2 marker
    if (data[0] == 0xFF && data[1] == 0xD8) {
        std::size_t offset = 2;
        while (offset + 8 < data.size()) {
            if (data[offset] != 0xFF) break;
            uint8_t marker = data[offset + 1];
            // SOF0 or SOF2
            if (marker == 0xC0 || marker == 0xC2) {
                int height = (data[offset + 5] << 8) | data[offset + 6];
                int width = (data[offset + 7] << 8) | data[offset + 8];
                return ImageDimensions{width, height};
            }
            // Skip to next marker
            int len = (data[offset + 2] << 8) | data[offset + 3];
            offset += 2 + len;
        }
    }

    // GIF: width/height at offset 6-9 (little-endian)
    if (data.size() >= 10 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        int width = data[6] | (data[7] << 8);
        int height = data[8] | (data[9] << 8);
        return ImageDimensions{width, height};
    }

    return std::nullopt;
}

// Determine if an image needs resizing
bool should_resize(ImageDimensions dims, int max_pixels) {
    return static_cast<long long>(dims.width) * dims.height > max_pixels;
}

// Calculate target dimensions maintaining aspect ratio
ImageDimensions calculate_resize(ImageDimensions dims, int max_pixels) {
    if (!should_resize(dims, max_pixels)) return dims;

    double aspect = static_cast<double>(dims.width) / static_cast<double>(dims.height);
    double target_height = std::sqrt(static_cast<double>(max_pixels) / aspect);
    double target_width = target_height * aspect;

    return ImageDimensions{
        static_cast<int>(target_width),
        static_cast<int>(target_height)
    };
}

// Resize image data. Encoded image data is returned unchanged; dimensions are calculated separately.
std::vector<uint8_t> resize_image(std::span<uint8_t> data, ImageDimensions target) {
    (void)target;
    return std::vector<uint8_t>(data.begin(), data.end());
}

} // namespace cc::utils
