// C++23 Module: Multi-line paste detection and processing with bracketed paste mode support
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.paste_handler;


export namespace cc::hooks {


enum class PasteSource {
    bracketed,
    detected,
    manual,
};


struct PasteEvent {
    std::string content;
    PasteSource source{PasteSource::bracketed};
    bool is_bracketed{false};
    std::size_t char_count{0};
    std::size_t line_count{0};


    auto compute_stats() -> void {
        char_count = content.size();
        line_count = 1;
        for (char c : content) {
            if (c == '\n') ++line_count;
        }
    }
};


struct PasteConfig {
    std::size_t max_size{1048576};
    bool strip_trailing_newline{true};
    bool auto_indent{false};
    std::size_t confirm_threshold{1000};
    std::string indent_string{"    "};
};


using PasteConfirmFn = std::function<bool(const PasteEvent&)>;

using PasteCallback = std::function<void(const PasteEvent&)>;


class PasteHandler {
public:
    explicit PasteHandler(PasteConfig config = {})
        : config_(std::move(config)) {}


    auto on_paste_start() -> void {
        pasting_ = true;
        paste_buffer_.clear();
        paste_start_time_ = std::chrono::steady_clock::now();
    }


    auto on_paste_data(const std::uint8_t* bytes, std::size_t len) -> void {
        if (!pasting_) return;


        auto remaining = config_.max_size - paste_buffer_.size();
        auto to_append = std::min(len, remaining);
        paste_buffer_.append(reinterpret_cast<const char*>(bytes), to_append);

        if (paste_buffer_.size() >= config_.max_size) {
            truncated_ = true;
        }
    }


    auto on_paste_data(std::string_view data) -> void {
        on_paste_data(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }


    [[nodiscard]] auto on_paste_end() -> PasteEvent {
        pasting_ = false;

        PasteEvent event{
            .content = std::move(paste_buffer_),
            .source = PasteSource::bracketed,
            .is_bracketed = true,
        };
        event.compute_stats();

        paste_buffer_.clear();
        truncated_ = false;


        for (const auto& cb : callbacks_) {
            cb(event);
        }

        return event;
    }


    [[nodiscard]] auto should_confirm(const PasteEvent& event) const -> bool {
        return event.char_count > config_.confirm_threshold;
    }

    /**
     * Process pasted content by applying configured transformations.
     * - Trim trailing newlines.
     * - Auto-indent pasted blocks.
     * - Enforce size limits.
     */
    [[nodiscard]] auto process_paste(const PasteEvent& event,
                                      std::string_view current_indent = "") const -> std::string {
        std::string result = event.content;


        if (config_.strip_trailing_newline) {
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
        }


        if (config_.auto_indent && !current_indent.empty() && event.line_count > 1) {
            result = apply_indent(result, current_indent);
        }

        return result;
    }


    auto set_config(PasteConfig config) -> void { config_ = std::move(config); }


    [[nodiscard]] auto config() const -> const PasteConfig& { return config_; }


    [[nodiscard]] auto is_pasting() const -> bool { return pasting_; }


    [[nodiscard]] auto is_truncated() const -> bool { return truncated_; }


    [[nodiscard]] auto buffer_size() const -> std::size_t { return paste_buffer_.size(); }


    auto on_paste(PasteCallback callback) -> void {
        callbacks_.push_back(std::move(callback));
    }


    auto set_confirm_fn(PasteConfirmFn fn) -> void {
        confirm_fn_ = std::move(fn);
    }


    [[nodiscard]] auto request_confirm(const PasteEvent& event) -> bool {
        if (confirm_fn_) return confirm_fn_(event);
        return true;
    }


    [[nodiscard]] static auto create_detected_paste(std::string_view content) -> PasteEvent {
        PasteEvent event{
            .content = std::string(content),
            .source = PasteSource::detected,
            .is_bracketed = false,
        };
        event.compute_stats();
        return event;
    }

private:
    PasteConfig config_;
    std::string paste_buffer_;
    bool pasting_{false};
    bool truncated_{false};
    std::chrono::steady_clock::time_point paste_start_time_;
    std::vector<PasteCallback> callbacks_;
    PasteConfirmFn confirm_fn_;


    [[nodiscard]] static auto apply_indent(std::string_view text,
                                            std::string_view indent) -> std::string {
        std::string result;
        result.reserve(text.size() + text.size() / 40 * indent.size());

        bool at_line_start = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (at_line_start && text[i] != '\n') {
                result.append(indent);
                at_line_start = false;
            }
            result.push_back(text[i]);
            if (text[i] == '\n') {
                at_line_start = true;
            }
        }
        return result;
    }
};

} // namespace cc::hooks
