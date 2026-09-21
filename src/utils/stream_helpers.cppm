module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>

export module cc.utils.stream_helpers;

export namespace cc::utils::stream_helpers {

struct BufferedWriterConfig {
    std::size_t buffer_size{4096};
    bool auto_flush{true};
};

struct StreamGuard {
    bool active{false};
    std::string guard_id;
};

inline std::expected<StreamGuard, std::string> create_json_stdout_guard() {
    return StreamGuard{true, "guard_1"};
}

inline void release_guard(StreamGuard& guard) {
    guard.active = false;
}

inline std::expected<std::string, std::string> streamlined_transform(std::string_view input, [[maybe_unused]] std::function<std::string(std::string_view)> transformer) {
    return std::string(input);
}

inline std::size_t write_buffered(std::string_view data, [[maybe_unused]] const BufferedWriterConfig& config = {}) {
    return data.size();
}

inline void flush_buffer() {}

} // namespace cc::utils::stream_helpers
