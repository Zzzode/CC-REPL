module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <cstdint>

export module cc.tools.file_read_helpers;

export namespace cc::tools::file_read_helpers {

struct ReadLimits {
    size_t max_file_size{10 * 1024 * 1024};
    size_t max_line_count{10000};
    size_t max_line_length{2000};
};

struct ImageProcessResult {
    std::string format;
    uint32_t width;
    uint32_t height;
    size_t size_bytes;
    std::optional<std::string> base64_data;
};

inline ReadLimits get_default_limits() {
    return {};
}

inline std::expected<ImageProcessResult, std::string> process_image(
    [[maybe_unused]] std::string_view path) {
    return ImageProcessResult{"png", 0, 0, 0, std::nullopt};
}

inline bool is_image_file([[maybe_unused]] std::string_view path) {
    return false;
}

inline bool exceeds_limits(
    [[maybe_unused]] std::string_view path,
    [[maybe_unused]] const ReadLimits& limits) {
    return false;
}

inline std::expected<std::string, std::string> read_with_limits(
    [[maybe_unused]] std::string_view path,
    [[maybe_unused]] const ReadLimits& limits) {
    return std::string{""};
}

} // namespace cc::tools::file_read_helpers
