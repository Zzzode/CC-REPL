/// @file declared_cursor.cppm
/// @brief Declared cursor support for IME / accessibility.
///
/// Faithful C++ port of the TS `useDeclaredCursor` hook +
/// `CursorDeclarationContext` system.  Components "declare" where the text
/// cursor should be parked after each frame, and the real terminal cursor
/// is positioned there so IME preedit text renders inline and screen
/// readers / magnifiers can follow the input.
///
/// Unlike the React hook pattern (which relies on context + layout effects),
/// the FTXUI port is a DOM decorator node: wrap any element with
/// `declared_cursor(active, rel_x, rel_y, shape)` and, during the render
/// pass, the node will call `screen.SetCursor()` with the absolute screen
/// coordinates of the cursor (box origin + relative offset).
///
/// The "conditional clear" sibling-handoff safety from TS is NOT needed in
/// FTXUI because every component re-renders every frame in tree order —
/// the last `SetCursor` call wins deterministically, and inactive callers
/// simply skip the call rather than actively clearing.
module;

#include <algorithm>  // for std::clamp
#include <memory>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

export module cc.ui.common.declared_cursor;

export namespace cc::ui::common::declared_cursor {
using namespace ftxui;

namespace detail {

// Node that resets the screen cursor to Hidden before rendering children.
//
// Apply this at the root of the render tree to ensure the terminal cursor
// starts hidden each frame — then any `declared_cursor` decorator on a child
// can override the physical cursor position and shape.  The prompt input keeps
// that native cursor hidden and draws its own visible caret, matching TS Ink.
class CursorResetNode : public Node {
public:
    explicit CursorResetNode(Element child)
        : Node(unpack(std::move(child))) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(Screen& screen) override {
        // Reset to hidden BEFORE children render, so active declared cursors
        // can override the position/shape.  FTXUI emits cursor movement escape
        // sequences even for a hidden cursor, so park it at the neutral
        // bottom-right position instead of (0, 0) to avoid cursor-guide jumps.
        screen.SetCursor(Screen::Cursor{
            /*x=*/std::max(0, screen.dimx() - 1),
            /*y=*/std::max(0, screen.dimy() - 1),
            Screen::Cursor::Shape::Hidden,
        });
        Node::Render(screen);
    }
};

// Custom DOM node that captures its box (like `reflect`) and, during
// Render(), sets the screen cursor to the declared position if active.
//
// This is the FTXUI equivalent of the TS `useDeclaredCursor` hook combined
// with the `CursorDeclarationContext` setter — it turns a relative cursor
// position into an absolute screen coordinate and applies it.
class DeclaredCursorNode : public Node {
public:
    DeclaredCursorNode(Element child,
                       bool active,
                       int rel_x,
                       int rel_y,
                       Screen::Cursor::Shape shape)
        : Node(unpack(std::move(child)))
        , active_(active)
        , rel_x_(rel_x)
        , rel_y_(rel_y)
        , shape_(shape) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(Screen& screen) override {
        Node::Render(screen);
        if (active_) {
            // Clamp to box bounds so a stale declaration can't send the
            // cursor off-screen (e.g. after a resize before the next frame).
            const int abs_x = std::clamp(box_.x_min + rel_x_,
                                         box_.x_min,
                                         std::max(box_.x_min, box_.x_max));
            const int abs_y = std::clamp(box_.y_min + rel_y_,
                                         box_.y_min,
                                         std::max(box_.y_min, box_.y_max));
            screen.SetCursor(Screen::Cursor{abs_x, abs_y, shape_});
        }
    }

private:
    bool active_;
    int rel_x_;
    int rel_y_;
    Screen::Cursor::Shape shape_;
};

}  // namespace detail

/// @brief Declare the terminal cursor position relative to this element's box.
///
/// When `active` is true, parks the real terminal cursor at
/// `(box.x_min + rel_x, box.y_min + rel_y)` with the given shape after
/// rendering.  This enables IME preedit text to appear at the insertion
/// point and lets screen readers / screen magnifiers follow the input.
///
/// @param active  Whether this declaration is currently active.  When false,
///                the cursor is left untouched (some other component may own
///                it, or it stays at its default position).
/// @param rel_x   Horizontal offset (columns) from the element's left edge.
/// @param rel_y   Vertical offset (lines) from the element's top edge.
/// @param shape   Terminal cursor shape to use when active.
///
/// @ingroup dom
[[nodiscard]] inline Decorator declared_cursor(
    bool active,
    int rel_x,
    int rel_y,
    Screen::Cursor::Shape shape = Screen::Cursor::BarBlinking) {
    return [=](Element child) -> Element {
        return std::make_shared<detail::DeclaredCursorNode>(
            std::move(child), active, rel_x, rel_y, shape);
    };
}

/// @brief Convenience overload: declare cursor at a specific box.
///
/// When the cursor box is already known (e.g. captured via `reflect` on a
/// cursor glyph), this variant uses that box's top-left as the target.
[[nodiscard]] inline Decorator declared_cursor(bool active,
                                               const Box& cursor_box,
                                               Screen::Cursor::Shape shape =
                                                   Screen::Cursor::BarBlinking) {
    return declared_cursor(active, cursor_box.x_min, cursor_box.y_min, shape);
}

/// @brief Reset the terminal cursor to hidden at the start of each frame.
///
/// Wrap the root of your render tree with this decorator to ensure the cursor
/// starts hidden each frame.  Any `declared_cursor` decorator applied to
/// descendants can override this with a declared physical cursor position.
/// Prompt input keeps the native cursor hidden and renders its own caret.
[[nodiscard]] inline Decorator cursor_reset() {
    return [](Element child) -> Element {
        return std::make_shared<detail::CursorResetNode>(std::move(child));
    };
}

}  // namespace cc::ui::common::declared_cursor
