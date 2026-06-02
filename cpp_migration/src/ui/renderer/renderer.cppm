// SPDX-License-Identifier: MIT
// cc.ui.renderer - Thin rendering abstraction layer over FTXUI
// Migrated from: src/ink/renderer.ts, output.ts, optimizer.ts, render-border.ts,
//   render-node-to-output.ts, render-to-screen.ts, screen.ts, frame.ts,
//   dom.ts, reconciler.ts, root.ts, instances.ts

module;

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <expected>

export module cc.ui.renderer;

export namespace cc::ui::renderer {

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

struct OutputCell {
    char32_t ch = U' ';
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool dim = false;
    bool inverse = false;
    bool strikethrough = false;
};

using OutputLine = std::vector<OutputCell>;

struct ScreenBuffer {
    std::vector<OutputLine> lines;
    int width = 0;
    int height = 0;
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = true;
};

struct RenderFrame {
    ScreenBuffer buffer;
    std::chrono::steady_clock::time_point timestamp;
    uint64_t frame_id = 0;
};

struct DirtyRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RenderConfig {
    int max_fps = 60;
    bool use_alternate_screen = false;
    bool enable_mouse = false;
    bool enable_focus_tracking = true;
    int min_height = 1;
};

// ---------------------------------------------------------------------------
// Class: Renderer — manages the render loop
// ---------------------------------------------------------------------------

class Renderer {
public:
    explicit Renderer(RenderConfig config = {})
        : config_(std::move(config)) {}

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }

    void request_render() {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_ = true;
    }

    void set_render_callback(std::function<ScreenBuffer()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        render_callback_ = std::move(callback);
    }

    double get_fps() const {
        // Stub: return configured max as nominal fps
        return static_cast<double>(config_.max_fps);
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    uint64_t get_frame_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_count_;
    }

private:
    RenderConfig config_;
    mutable std::mutex mutex_;
    bool running_ = false;
    bool dirty_ = false;
    uint64_t frame_count_ = 0;
    std::function<ScreenBuffer()> render_callback_;
};

// ---------------------------------------------------------------------------
// Class: ScreenOptimizer — minimizes terminal output by diffing buffers
// ---------------------------------------------------------------------------

class ScreenOptimizer {
public:
    std::vector<DirtyRegion> optimize(
        [[maybe_unused]] const ScreenBuffer& prev,
        const ScreenBuffer& curr) {

        // Stub: mark entire screen as dirty
        std::vector<DirtyRegion> regions;
        if (curr.height > 0 && curr.width > 0) {
            regions.push_back(DirtyRegion{0, 0, curr.width, curr.height});
        }
        return regions;
    }

    std::string generate_diff_output(
        [[maybe_unused]] const ScreenBuffer& prev,
        const ScreenBuffer& curr) {

        // Stub: render entire current buffer as string
        std::string result;
        for (const auto& line : curr.lines) {
            for (const auto& cell : line) {
                if (cell.ch <= 0x7F) {
                    result += static_cast<char>(cell.ch);
                } else {
                    result += '?';
                }
            }
            result += '\n';
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

inline std::string render_border(std::string_view content, int width) {
    std::string result;
    // Top border
    result += '+';
    for (int i = 0; i < width - 2; ++i) result += '-';
    result += "+\n";

    // Content line(s)
    result += '|';
    int padding = width - 2 - static_cast<int>(content.size());
    result += content;
    for (int i = 0; i < padding; ++i) result += ' ';
    result += "|\n";

    // Bottom border
    result += '+';
    for (int i = 0; i < width - 2; ++i) result += '-';
    result += '+';

    return result;
}

inline std::string render_to_string(const ScreenBuffer& buffer) {
    std::string result;
    for (const auto& line : buffer.lines) {
        for (const auto& cell : line) {
            if (cell.ch <= 0x7F) {
                result += static_cast<char>(cell.ch);
            } else {
                result += '?';
            }
        }
        result += '\n';
    }
    return result;
}

inline ScreenBuffer create_empty_buffer(int width, int height) {
    ScreenBuffer buffer;
    buffer.width = width;
    buffer.height = height;
    buffer.cursor_x = 0;
    buffer.cursor_y = 0;
    buffer.cursor_visible = true;
    buffer.lines.resize(static_cast<std::size_t>(height));
    for (auto& line : buffer.lines) {
        line.resize(static_cast<std::size_t>(width));
    }
    return buffer;
}

inline ScreenBuffer merge_buffers(
    const ScreenBuffer& base,
    const ScreenBuffer& overlay,
    int offset_x,
    int offset_y) {

    ScreenBuffer result = base;

    for (int oy = 0; oy < overlay.height; ++oy) {
        int target_y = oy + offset_y;
        if (target_y < 0 || target_y >= result.height) continue;

        const auto& src_line = overlay.lines[static_cast<std::size_t>(oy)];
        auto& dst_line = result.lines[static_cast<std::size_t>(target_y)];

        for (int ox = 0; ox < overlay.width; ++ox) {
            int target_x = ox + offset_x;
            if (target_x < 0 || target_x >= result.width) continue;

            dst_line[static_cast<std::size_t>(target_x)] =
                src_line[static_cast<std::size_t>(ox)];
        }
    }

    return result;
}

} // namespace cc::ui::renderer
