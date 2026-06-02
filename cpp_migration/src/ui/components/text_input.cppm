module;

#include <string>
#include <vector>
#include <functional>
#include <deque>
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

export module ui.components.text_input;

export namespace ui::components {

struct Suggestion {
    std::string text;
    std::string description;
    std::string category; // "command", "file", "history"
};

struct TextInputOptions {
    std::string placeholder = "Type your message here...";
    std::string prefix = "▶ ";
    bool multiline = false;
    bool show_history = true;
    size_t max_history_size = 1000;
    std::function<void(std::string)> on_submit;
    std::function<void(std::string)> on_change;
    std::function<std::vector<Suggestion>(std::string)> get_suggestions;
};

class TextInputImpl {
public:
    TextInputImpl(const TextInputOptions& options)
        : options_(options),
          cursor_pos_(0),
          history_index_(std::string::npos),
          showing_suggestions_(false),
          selected_suggestion_(0) {}

    void set_text(const std::string& text) {
        text_ = text;
        cursor_pos_ = text_.size();
    }

    std::string get_text() const { return text_; }

    void add_to_history(const std::string& entry) {
        if (!entry.empty()) {
            // Remove duplicates if present
            auto it = std::find(history_.begin(), history_.end(), entry);
            if (it != history_.end()) {
                history_.erase(it);
            }
            history_.push_back(entry);
            if (history_.size() > options_.max_history_size) {
                history_.pop_front();
            }
        }
        history_index_ = std::string::npos;
    }

    void clear() {
        text_.clear();
        cursor_pos_ = 0;
        history_index_ = std::string::npos;
    }

    void clear_history() {
        history_.clear();
        history_index_ = std::string::npos;
    }

    ftxui::Element Render() {
        using namespace ftxui;

        // Render the input line with cursor
        Elements elements;

        // Prefix
        elements.push_back(text(options_.prefix) | color(Color::Green) | bold);

        if (text_.empty()) {
            if (options_.placeholder.empty()) {
                elements.push_back(text(" ") | inverted | color(Color::White));
            } else {
                elements.push_back(text(options_.placeholder) | dim);
            }
            auto input_line = hbox(elements) | flex;
            return input_line;
        }

        // Text before cursor
        std::string before = text_.substr(0, cursor_pos_);
        if (!before.empty()) {
            elements.push_back(text(before));
        }

        // Cursor
        if (cursor_pos_ < text_.size()) {
            std::string cursor_str;
            cursor_str += text_[cursor_pos_];
            elements.push_back(text(cursor_str) | inverted | color(Color::White));

            // Text after cursor
            std::string after = text_.substr(cursor_pos_ + 1);
            if (!after.empty()) {
                elements.push_back(text(after));
            }
        } else {
            elements.push_back(text(" ") | inverted | color(Color::White));
        }

        auto input_line = hbox(elements) | flex;

        // Render suggestions dropdown if needed
        if (showing_suggestions_ && !suggestions_.empty()) {
            Elements suggestion_elements;
            for (size_t i = 0; i < suggestions_.size(); ++i) {
                auto& s = suggestions_[i];
                Element el = hbox({
                    text(s.text) | color(Color::Cyan),
                    text("  ") | flex,
                    text(s.description) | dim | color(Color::GrayLight)
                });
                if (i == static_cast<size_t>(selected_suggestion_)) {
                    el = el | inverted | bgcolor(Color::RGB(50, 50, 50));
                }
                suggestion_elements.push_back(el);
            }

            return vbox({
                input_line,
                separator(),
                vbox(suggestion_elements) | border | bgcolor(Color::RGB(30, 30, 30))
            });
        }

        return input_line;
    }

    bool HandleEvent(ftxui::Event event) {
        using namespace ftxui;

        // Character input
        if (event.is_character()) {
            char c = event.character()[0];
            if (c >= 32 && c < 127) {
                text_.insert(cursor_pos_, 1, c);
                cursor_pos_++;
                update_suggestions();
                if (options_.on_change) {
                    options_.on_change(text_);
                }
                return true;
            }
        }

        // Navigation
        if (event == Event::ArrowLeft) {
            if (cursor_pos_ > 0) {
                cursor_pos_--;
            }
            return true;
        }
        if (event == Event::ArrowRight) {
            if (cursor_pos_ < text_.size()) {
                cursor_pos_++;
            }
            return true;
        }
        if (event == Event::Home) {
            cursor_pos_ = 0;
            return true;
        }
        if (event == Event::End) {
            cursor_pos_ = text_.size();
            return true;
        }

        // History navigation
        if (event == Event::ArrowUp) {
            navigate_history_up();
            return true;
        }
        if (event == Event::ArrowDown) {
            navigate_history_down();
            return true;
        }

        // Deletion
        if (event == Event::Backspace) {
            if (cursor_pos_ > 0) {
                text_.erase(cursor_pos_ - 1, 1);
                cursor_pos_--;
                update_suggestions();
                if (options_.on_change) {
                    options_.on_change(text_);
                }
            }
            return true;
        }
        if (event == Event::Delete) {
            if (cursor_pos_ < text_.size()) {
                text_.erase(cursor_pos_, 1);
                update_suggestions();
                if (options_.on_change) {
                    options_.on_change(text_);
                }
            }
            return true;
        }

        // Suggestion navigation
        if (showing_suggestions_ && !suggestions_.empty()) {
            if (event == Event::Tab) {
                selected_suggestion_ = (selected_suggestion_ + 1) % static_cast<int>(suggestions_.size());
                return true;
            }
            if (event == Event::TabReverse) {
                selected_suggestion_ = (selected_suggestion_ - 1 + static_cast<int>(suggestions_.size())) % static_cast<int>(suggestions_.size());
                return true;
            }
            if (event == Event::Return) {
                accept_suggestion();
                return true;
            }
        }

        // Search (Ctrl+R)
        if (event == Event::Special({18}) || event == Event::Character('\x12')) {
            start_history_search();
            return true;
        }

        // Submit
        if (event == Event::Return) {
            submit();
            return true;
        }

        // Escape to cancel suggestions/search
        if (event == Event::Escape) {
            showing_suggestions_ = false;
            search_mode_ = false;
            return true;
        }

        return false;
    }

private:
    void navigate_history_up() {
        if (history_.empty()) return;

        if (history_index_ == std::string::npos) {
            history_index_ = history_.size() - 1;
        } else if (history_index_ > 0) {
            history_index_--;
        }

        text_ = history_[history_index_];
        cursor_pos_ = text_.size();
        if (options_.on_change) {
            options_.on_change(text_);
        }
    }

    void navigate_history_down() {
        if (history_.empty() || history_index_ == std::string::npos) return;

        if (history_index_ < history_.size() - 1) {
            history_index_++;
            text_ = history_[history_index_];
        } else {
            history_index_ = std::string::npos;
            text_.clear();
        }
        cursor_pos_ = text_.size();
        if (options_.on_change) {
            options_.on_change(text_);
        }
    }

    void update_suggestions() {
        if (options_.get_suggestions && !text_.empty()) {
            suggestions_ = options_.get_suggestions(text_);
            showing_suggestions_ = !suggestions_.empty();
            selected_suggestion_ = 0;
        } else {
            showing_suggestions_ = false;
        }
    }

    void accept_suggestion() {
        if (showing_suggestions_ && !suggestions_.empty()) {
            text_ = suggestions_[selected_suggestion_].text;
            cursor_pos_ = text_.size();
            showing_suggestions_ = false;
            if (options_.on_change) {
                options_.on_change(text_);
            }
        }
    }

    void start_history_search() {
        search_mode_ = true;
        showing_suggestions_ = true;
        suggestions_.clear();
        // Search through history for current text
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if (it->find(text_) != std::string::npos) {
                suggestions_.push_back({*it, "History", "history"});
                if (suggestions_.size() >= 10) break;
            }
        }
        showing_suggestions_ = !suggestions_.empty();
        selected_suggestion_ = 0;
    }

    void submit() {
        if (!text_.empty()) {
            add_to_history(text_);
            if (options_.on_submit) {
                options_.on_submit(text_);
            }
            text_.clear();
            cursor_pos_ = 0;
            showing_suggestions_ = false;
            search_mode_ = false;
        }
    }

    TextInputOptions options_;
    std::string text_;
    size_t cursor_pos_;
    std::deque<std::string> history_;
    size_t history_index_;
    std::vector<Suggestion> suggestions_;
    bool showing_suggestions_;
    bool search_mode_ = false;
    int selected_suggestion_;
};

ftxui::Component TextInput(const TextInputOptions& options = {}) {
    using namespace ftxui;

    auto impl = std::make_shared<TextInputImpl>(options);

    return Renderer([impl] {
        return impl->Render();
    }) | CatchEvent([impl](Event e) {
        return impl->HandleEvent(e);
    });
}

} // namespace ui::components
