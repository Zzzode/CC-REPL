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
#include <deque>
#include <sstream>
#include <format>

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
        if (render_callback_) {
            last_frame_ = RenderFrame{
                .buffer = render_callback_(),
                .timestamp = std::chrono::steady_clock::now(),
                .frame_id = ++frame_count_,
            };
            frame_times_.push_back(last_frame_->timestamp);
            auto cutoff = last_frame_->timestamp - std::chrono::seconds(1);
            while (!frame_times_.empty() && frame_times_.front() < cutoff) {
                frame_times_.pop_front();
            }
            dirty_ = false;
        }
    }

    void set_render_callback(std::function<ScreenBuffer()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        render_callback_ = std::move(callback);
    }

    double get_fps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_times_.size() < 2) return 0.0;
        auto elapsed = std::chrono::duration<double>(frame_times_.back() - frame_times_.front()).count();
        if (elapsed <= 0.0) return 0.0;
        return static_cast<double>(frame_times_.size() - 1) / elapsed;
    }

    bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    uint64_t get_frame_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_count_;
    }

    [[nodiscard]] std::optional<RenderFrame> last_frame() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_frame_;
    }

private:
    RenderConfig config_;
    mutable std::mutex mutex_;
    bool running_ = false;
    bool dirty_ = false;
    uint64_t frame_count_ = 0;
    std::function<ScreenBuffer()> render_callback_;
    std::deque<std::chrono::steady_clock::time_point> frame_times_;
    std::optional<RenderFrame> last_frame_;
};

// ---------------------------------------------------------------------------
// Class: ScreenOptimizer — minimizes terminal output by diffing buffers
// ---------------------------------------------------------------------------

class ScreenOptimizer {
public:
    [[nodiscard]] static auto cell_equal(const OutputCell& a, const OutputCell& b) -> bool {
        return a.ch == b.ch &&
               a.fg_r == b.fg_r && a.fg_g == b.fg_g && a.fg_b == b.fg_b &&
               a.bg_r == b.bg_r && a.bg_g == b.bg_g && a.bg_b == b.bg_b &&
               a.bold == b.bold && a.italic == b.italic && a.underline == b.underline &&
               a.dim == b.dim && a.inverse == b.inverse && a.strikethrough == b.strikethrough;
    }

    std::vector<DirtyRegion> optimize(
        const ScreenBuffer& prev,
        const ScreenBuffer& curr) {
        std::vector<DirtyRegion> regions;
        if (curr.height <= 0 || curr.width <= 0) return regions;
        if (prev.width != curr.width || prev.height != curr.height) {
            regions.push_back(DirtyRegion{0, 0, curr.width, curr.height});
            return regions;
        }

        for (int y = 0; y < curr.height; ++y) {
            const OutputLine* curr_line = y < static_cast<int>(curr.lines.size()) ? &curr.lines[y] : nullptr;
            const OutputLine* prev_line = y < static_cast<int>(prev.lines.size()) ? &prev.lines[y] : nullptr;
            int run_start = -1;
            for (int x = 0; x < curr.width; ++x) {
                OutputCell curr_cell = curr_line && x < static_cast<int>(curr_line->size()) ? (*curr_line)[x] : OutputCell{};
                OutputCell prev_cell = prev_line && x < static_cast<int>(prev_line->size()) ? (*prev_line)[x] : OutputCell{};
                bool changed = !cell_equal(curr_cell, prev_cell);
                if (changed && run_start < 0) run_start = x;
                if ((!changed || x == curr.width - 1) && run_start >= 0) {
                    int end = changed && x == curr.width - 1 ? x + 1 : x;
                    regions.push_back(DirtyRegion{run_start, y, end - run_start, 1});
                    run_start = -1;
                }
            }
        }
        return regions;
    }

    std::string generate_diff_output(
        const ScreenBuffer& prev,
        const ScreenBuffer& curr) {
        auto regions = optimize(prev, curr);
        if (regions.empty()) return {};
        std::string result;
        for (const auto& region : regions) {
            result += std::format("\033[{};{}H", region.y + 1, region.x + 1);
            if (region.y >= static_cast<int>(curr.lines.size())) continue;
            const auto& line = curr.lines[region.y];
            for (int x = region.x; x < region.x + region.width && x < static_cast<int>(line.size()); ++x) {
                append_utf8(result, line[x].ch);
            }
        }
        return result;
    }

private:
    static void append_utf8(std::string& output, char32_t ch) {
        if (ch <= 0x7F) {
            output.push_back(static_cast<char>(ch));
        } else if (ch <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | ((ch >> 6) & 0x1F)));
            output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else if (ch <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | ((ch >> 12) & 0x0F)));
            output.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | ((ch >> 18) & 0x07)));
            output.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
};

namespace detail {
inline void append_utf8(std::string& output, char32_t ch) {
    if (ch <= 0x7F) {
        output.push_back(static_cast<char>(ch));
    } else if (ch <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((ch >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((ch >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | ((ch >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
}
} // namespace detail

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
            detail::append_utf8(result, cell.ch);
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
