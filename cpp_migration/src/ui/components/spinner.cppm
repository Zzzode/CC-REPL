module;

#include <vector>
#include <string>
#include <chrono>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module ui.components.spinner;

export namespace ui::components {

enum class SpinnerMode {
    Thinking,
    Processing,
    Idle
};

struct SpinnerOptions {
    SpinnerMode mode = SpinnerMode::Processing;
    bool reduced_motion = false;
    std::string message;
    bool verbose = false;
};

namespace detail {

std::vector<std::string> GetDefaultCharacters() {
    // Use cross-platform default
    return {"·", "✢", "*", "✶", "✻", "✽"};
}

std::vector<std::string> GetSpinnerFrames() {
    auto chars = GetDefaultCharacters();
    std::vector<std::string> frames;
    frames.reserve(chars.size() * 2);
    
    for (const auto& c : chars) {
        frames.push_back(c);
    }
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
        frames.push_back(*it);
    }
    
    return frames;
}

class SpinnerImpl {
public:
    SpinnerImpl(const SpinnerOptions& options)
        : options_(options)
        , frames_(GetSpinnerFrames())
        , start_time_(std::chrono::steady_clock::now())
    {}
    
    ftxui::Element Render() {
        using namespace ftxui;
        
        if (options_.mode == SpinnerMode::Idle) {
            return hbox({
                text("✓") | color(Color::Green),
                text(" Idle") | color(Color::GrayDark)
            });
        }
        
        if (options_.reduced_motion) {
            auto element = text("●") | color(Color::GrayLight);
            if (!options_.message.empty()) {
                return hbox({
                    element,
                    text(" " + options_.message) | dim
                });
            }
            return element;
        }
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
        int frame_index = (elapsed / 120) % frames_.size();
        
        auto spinner = text(frames_[frame_index]) | color(Color::CyanLight);
        
        if (options_.message.empty()) {
            return spinner;
        }
        
        return hbox({
            spinner,
            text(" " + options_.message) | color(Color::White)
        });
    }
    
private:
    SpinnerOptions options_;
    std::vector<std::string> frames_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace detail

ftxui::Component Spinner(const SpinnerOptions& options = {}) {
    using namespace ftxui;
    
    auto impl = std::make_shared<detail::SpinnerImpl>(options);
    
    return Renderer([impl] {
        return impl->Render();
    });
}

ftxui::Element SpinnerElement(const SpinnerOptions& options = {}) {
    using namespace ftxui;
    
    if (options.mode == SpinnerMode::Idle) {
        return hbox({
            text("✓") | color(Color::Green),
            text(" Idle") | color(Color::GrayDark)
        });
    }
    
    if (options.reduced_motion) {
        auto element = text("●") | color(Color::GrayLight);
        if (!options.message.empty()) {
            return hbox({
                element,
                text(" " + options.message) | dim
            });
        }
        return element;
    }
    
    // For static element, just show first frame
    auto spinner = text(detail::GetDefaultCharacters()[0]) | color(Color::CyanLight);
    
    if (options.message.empty()) {
        return spinner;
    }
    
    return hbox({
        spinner,
        text(" " + options.message) | color(Color::White)
    });
}

} // namespace ui::components
