module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.render_placeholder;

import cc.state.app_state;

export namespace cc::hooks::render_placeholder {

struct RenderPlaceholderState {
    std::string text;
    bool visible{true};
    bool animated{false};
};

struct RenderPlaceholderOptions {
    std::string default_text{"Type a message..."};
    bool show_when_empty{true};
    bool animate{false};
};

class RenderPlaceholderHook {
public:
    explicit RenderPlaceholderHook(const RenderPlaceholderOptions& opts = {})
        : options_(opts) {
        state_.text = opts.default_text;
        state_.visible = opts.show_when_empty;
        state_.animated = opts.animate;
    }

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const RenderPlaceholderState& state() const { return state_; }

    void set_text(std::string text) {
        state_.text = std::move(text);
        notify();
    }

    void set_visible(bool visible) {
        state_.visible = visible;
        notify();
    }

    void on_change(std::function<void(const RenderPlaceholderState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    RenderPlaceholderState state_;
    RenderPlaceholderOptions options_;
    std::vector<std::function<void(const RenderPlaceholderState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::render_placeholder
