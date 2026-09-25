// markdown_component_impl.cpp - impl unit for cc.ui.visual.markdown
// (RFC 0001 Phase C batch 6). Holds MarkdownComponentBase's ctor, Render()
// and OnEvent() in THIS ONE TU so those bodies emit once as strong symbols
// (Render is the key function - out-of-line, non-inline). Under clang named
// modules the exported class's vtable/typeinfo itself is owned by the
// interface unit (markdown.cppm.o); the goal is one strong copy with no weak
// duplicates. Also holds the MarkdownComponent() factory.
module;

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

module cc.ui.visual.markdown;

import std;

namespace cc::ui {

MarkdownComponentBase::MarkdownComponentBase(MarkdownComponentOptions opts)
    : opts_(std::move(opts)), scroll_offset_(0) {}

Element MarkdownComponentBase::Render() {
        auto element = render_markdown(opts_.content, opts_.render_options);

        // For long content, add scroll viewport
        // FTXUI's `vbox` + `yframe` + `vscroll_indicator` handles scrolling
        auto scrolled = element | yframe | flex;
        if (opts_.show_scrollbar) {
            scrolled = scrolled | vscroll_indicator;
        }

        return scrolled;
    }

bool MarkdownComponentBase::OnEvent(Event event) {
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            scroll_offset_++;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            if (scroll_offset_ > 0) scroll_offset_--;
            return true;
        }
        if (event == Event::PageDown) {
            scroll_offset_ += opts_.visible_lines;
            return true;
        }
        if (event == Event::PageUp) {
            scroll_offset_ = std::max(0, scroll_offset_ - opts_.visible_lines);
            return true;
        }
        if (event == Event::Home) {
            scroll_offset_ = 0;
            return true;
        }
        if (event == Event::End) {
            scroll_offset_ = 100000; // effectively bottom
            return true;
        }
        return false;
    }

/// Create an interactive Markdown viewer component
[[nodiscard]] Component MarkdownComponent(
    MarkdownComponentOptions opts) {
    return Make<MarkdownComponentBase>(std::move(opts));
}

} // namespace cc::ui
