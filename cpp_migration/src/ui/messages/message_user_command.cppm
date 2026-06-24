// User command message - displays slash commands executed by the user
//
// Matches UserCommandMessage.tsx:
//   ┌─ green $ prefix, inline command + args, copy button
//   │  $ /commit "feat: xyz"                          [📋 copy]
//   └─ optional output block below, up to N lines
module;

#include <climits>
#include <functional>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.message_user_command;

export namespace cc::ui::messages::user_command {
using namespace ftxui;

/// Data for a user command message
struct UserCommandData {
    std::string command_name;              // e.g., "help", "config", "model"
    std::vector<std::string> args;         // Command arguments
    std::optional<std::string> output;     // Command output/result
    bool success = true;                   // Whether command succeeded
    bool is_skill_format = false;          // Render as "Skill(name)" chip
};

/// ─── Stateless element renderer (kept for non-interactive contexts) ───
[[nodiscard]] inline Element render(const UserCommandData& data) {
    Elements elements;

    if (data.is_skill_format) {
        elements.push_back(hbox({
            text("❯ ") | color(Color::GrayDark),
            text("Skill(") | dim,
            text(data.command_name) | bold,
            text(")") | dim,
        }));
    } else {
        // Command line:  $ /name arg1 arg2 ...
        Elements cmd_parts;
        cmd_parts.push_back(text("$ ") | bold | color(Color::Green));
        cmd_parts.push_back(text("/" + data.command_name)
                                | bold | color(Color::Green));

        for (const auto& arg : data.args) {
            cmd_parts.push_back(text(" " + arg) | color(Color::White));
        }
        elements.push_back(hbox(cmd_parts));
    }

    // Output
    if (data.output && !data.output->empty()) {
        auto output_color = data.success ? Color::GrayLight : Color::Red;

        // Split output into lines, limit display
        std::string_view out = *data.output;
        int lines = 0;
        size_t pos = 0;
        while (pos < out.size() && lines < 10) {
            auto nl = out.find('\n', pos);
            std::string line;
            if (nl == std::string_view::npos) {
                line = std::string(out.substr(pos));
                pos = out.size();
            } else {
                line = std::string(out.substr(pos, nl - pos));
                pos = nl + 1;
            }
            elements.push_back(text("  " + line) | color(output_color) | dim);
            ++lines;
        }
        if (pos < out.size()) {
            elements.push_back(text("  ...") | dim);
        }
    }

    return vbox(elements);
}

// ─── Interactive component (copy button + expand) ──────────────────────

class UserCommandMessageComponent : public ComponentBase {
  public:
    using OnCopyFn = std::function<void(std::string_view)>;

    explicit UserCommandMessageComponent(UserCommandData data,
                                         OnCopyFn on_copy = nullptr)
        : data_(std::move(data)), on_copy_(std::move(on_copy))
    {
        copy_btn_ = Button(" copy", [this] {
            if (on_copy_) on_copy_(BuildFullCommand());
        }) | size(WIDTH, EQUAL, 12) | bold;
        Add(copy_btn_);

        toggle_btn_ = Button(" show all", [this] {
            show_all_output_ = !show_all_output_;
            // Update label text via re-creating button is too heavy; we rely on
            // the Render() call using ->Render() directly with conditional.
        }) | dim;
        Add(toggle_btn_);
    }

    Element OnRender() override {
        Elements header_parts;
        if (data_.is_skill_format) {
            header_parts.push_back(hbox({
                text("❯ ") | color(Color::GrayDark),
                text("Skill(") | dim,
                text(data_.command_name) | bold,
                text(")") | dim,
            }));
        } else {
            header_parts.push_back(text("$ ") | bold | color(Color::Green));
            header_parts.push_back(text("/" + data_.command_name)
                                       | bold | color(Color::Green));
            for (const auto& a : data_.args)
                header_parts.push_back(text(" " + a) | color(Color::White));
        }

        auto full_cmd = BuildFullCommand();
        auto header = hbox({
            hbox(std::move(header_parts)) | flex,
            filler(),
            copy_btn_->Render(),
        }) | color(Color::GreenLight);

        // Output section
        Elements output_rows;
        if (data_.output && !data_.output->empty()) {
            auto output_color = data_.success ? Color::GrayLight : Color::Red;
            std::string_view out = *data_.output;
            const int max_lines = show_all_output_ ? INT32_MAX : 10;
            size_t pos = 0;
            int lines = 0;
            bool truncated = false;
            while (pos < out.size() && lines < max_lines) {
                auto nl = out.find('\n', pos);
                std::string line;
                if (nl == std::string_view::npos) {
                    line = std::string(out.substr(pos));
                    pos = out.size();
                } else {
                    line = std::string(out.substr(pos, nl - pos));
                    pos = nl + 1;
                }
                output_rows.push_back(text("  " + line) | color(output_color) | dim);
                ++lines;
            }
            if (pos < out.size()) {
                truncated = true;
                // Toggle element (inline Element form; event handled in OnEvent)
                std::string label = show_all_output_ ? " ▲ collapse" : " ▼ show all";
                output_rows.push_back(text(label) | dim);
            }
            (void)truncated;
        }

        Elements all;
        all.push_back(std::move(header));
        if (!output_rows.empty()) {
            all.push_back(separatorEmpty());
            all.push_back(vbox(std::move(output_rows)));
        }
        return vbox(std::move(all)) | borderLight | color(Color::GreenLight);
    }

    bool OnEvent(Event event) override {
        if (event == Event::Special({3})) {   // Ctrl+C
            if (on_copy_) on_copy_(BuildFullCommand());
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    std::string BuildFullCommand() const {
        std::string s = "/" + data_.command_name;
        for (const auto& a : data_.args) {
            s.push_back(' ');
            s += a;
        }
        return s;
    }

    UserCommandData data_;
    OnCopyFn on_copy_;
    bool show_all_output_ = false;
    Component copy_btn_;
    Component toggle_btn_;
};

[[nodiscard]] inline Component MakeUserCommandMessage(
    UserCommandData data,
    UserCommandMessageComponent::OnCopyFn on_copy = nullptr)
{
    return Make<UserCommandMessageComponent>(std::move(data), std::move(on_copy));
}

} // namespace cc::ui::messages::user_command
