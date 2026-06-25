/// @file prompt_dialog.cppm
/// @brief Generic PromptDialog with FREE-TEXT and SELECT modes.
///
/// TS contract (src/components/PromptDialog/PromptDialog.tsx):
///   - Two mutually-exclusive modes selected at construction:
///       * FREE-TEXT when `options` is empty -> delegates to a TextInput.
///       * SELECT when `options` is non-empty  -> vertical list with focused
///         row highlight, j/k/ArrowUp/ArrowDown movement (with wrap), numeric
///         1..9 shortcuts, right-side description column and an optional
///         subtitle (tool_input_summary) rendered as the DialogFrame subtitle.
///   - Callbacks:
///       * on_response(nullopt)   -> Escape / cancel the free-text response.
///       * on_response(string)    -> Enter from free-text.
///       * on_select(option_key)  -> SELECT: Enter or 1..9 shortcut commit.
///       * on_abort()             -> Ctrl+C (distinct code path from Esc).
///
/// Keyboard:
///   ArrowDown / ArrowUp / j / k   -> move focused index (with wrap).
///   1..9                          -> commit options[n-1].key immediately.
///   Enter                         -> commit options[selected_index].key
///                                   (SELECT) or submit free text.
///   Escape                        -> on_response(nullopt).
///   Ctrl+C                        -> on_abort() (FTXUI: Special({0x03})).
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

export module cc.ui.dialogs.prompt_dialog;

export namespace cc::ui::dialogs::prompt_dialog {

using namespace ftxui;

// =========================================================================
// Public types
// =========================================================================

/// A single selectable option in SELECT mode. Mirrors the TS PromptOption
/// interface: { label: string, key: string, description?: string }.
struct PromptOption {
    std::string label;
    std::string key;
    std::string description; // may be empty
};

/// Complete payload / props for the PromptDialog.
///
/// The renderer chooses between SELECT vs FREE-TEXT mode based purely on
/// whether `options` is empty (FREE-TEXT) or non-empty (SELECT). Tests at
/// tests/test_prompt_dialog.cpp lines 125-143 rely on the presence of the
/// `options`, `tool_input_summary`, `on_abort` and `selected_index`
/// fields, so they MUST remain as plain named members.
struct PromptDialogPayload {
    // -- Text prompt (both modes) --
    std::string title;

    // -- SELECT mode only --
    std::vector<PromptOption> options;
    int selected_index = 0;

    // -- Optional chrome / extra context --
    /// Short one-line summary of the tool input that triggered this prompt.
    /// When present it is rendered as the DialogFrame subtitle (right under
    /// the title).
    std::optional<std::string> tool_input_summary;

    /// Placeholder text used in FREE-TEXT mode for the input box.
    std::optional<std::string> placeholder;

    // -- Callbacks --
    /// Ctrl+C pressed. Distinct from Esc / on_response(nullopt).
    std::function<void()> on_abort;

    /// FREE-TEXT submit / cancel.  nullopt means "user pressed Escape".
    /// SELECT mode also invokes this on Escape (key == nullopt).
    std::function<void(std::optional<std::string>)> on_response;

    /// SELECT mode only: fires the option key (Enter / 1..9 shortcut).
    std::function<void(const std::string& option_key)> on_select;
};

// =========================================================================
// Internal helpers
// =========================================================================

namespace detail {

/// Clamp/wrap an index into [0, n).  SELECT mode uses wrap per the TS spec.
[[nodiscard]] inline int wrap_index(int idx, int n) {
    if (n <= 0) return 0;
    int m = idx % n;
    if (m < 0) m += n;
    return m;
}

/// Render a single SELECT row.  Focused rows are inverted + bold, every row
/// is prefixed with `N. ` where N == 1..9 (for the numeric shortcut).
[[nodiscard]] inline Element RenderSelectRow(const PromptOption& opt,
                                             int one_based_index,
                                             bool focused)
{
    auto prefix = text(std::to_string(one_based_index) + ". ") | dim;
    auto label  = text(opt.label);
    if (focused) label = label | bold;

    auto description = opt.description.empty()
        ? text("")
        : hbox({
              filler(),
              text(opt.description) | dim,
          });

    auto row = hbox({
        prefix,
        label,
        description | xflex,
    });

    if (focused) row = row | inverted | color(Color::Cyan);
    return row;
}

/// Render the optional subtitle line under the window title.
[[nodiscard]] inline Element RenderSubtitle(
    const std::optional<std::string>& tool_input_summary)
{
    if (!tool_input_summary || tool_input_summary->empty()) return text("");
    return paragraph(std::string{*tool_input_summary})
         | color(Color::Yellow) | dim;
}

} // namespace detail

// =========================================================================
// SELECT mode renderer
// =========================================================================

/// Render the dialog body for SELECT mode (options non-empty).
[[nodiscard]] inline Element RenderSelectMode(
    const std::shared_ptr<PromptDialogPayload>& p)
{
    const int n = static_cast<int>(p->options.size());
    const int sel = detail::wrap_index(p->selected_index, n);

    Elements body;
    auto subtitle_el = detail::RenderSubtitle(p->tool_input_summary);
    if (subtitle_el != text("")) {
        body.push_back(subtitle_el);
        body.push_back(separator());
    }

    Elements rows;
    rows.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        rows.push_back(detail::RenderSelectRow(
            p->options[static_cast<std::size_t>(i)],
            /*one_based_index=*/i + 1,
            /*focused=*/i == sel));
    }
    body.push_back(vbox(std::move(rows)) | xflex);

    body.push_back(separator());
    body.push_back(hbox({
        text("Enter") | bold,
        text(" commit · "),
        text("Esc") | bold,
        text(" cancel · "),
        text("1-9") | bold,
        text(" shortcut · "),
        text("j/k ↑/↓") | bold,
        text(" navigate"),
    }) | dim);

    const std::string title = p->title.empty() ? "Select an option" : p->title;
    return window(
        text(" " + title + " ") | bold | color(Color::Cyan),
        vbox(std::move(body)) | xflex
    ) | color(Color::Cyan) | size(WIDTH, LESS_THAN, 92);
}

// =========================================================================
// FREE-TEXT mode renderer
// =========================================================================

/// Render the dialog body for FREE-TEXT mode (options empty).
/// We keep a lightweight internal text buffer here so tests can exercise
/// the prompt without pulling in the full BaseTextInput component.
[[nodiscard]] inline Element RenderFreeTextMode(
    const std::shared_ptr<PromptDialogPayload>& p,
    const std::shared_ptr<std::string>& buffer,
    const std::shared_ptr<int>& cursor)
{
    Elements body;
    auto subtitle_el = detail::RenderSubtitle(p->tool_input_summary);
    if (subtitle_el != text("")) {
        body.push_back(subtitle_el);
        body.push_back(separator());
    }

    // Build the text-input visual: buffer + cursor marker.
    std::string display = *buffer;
    const int c = std::max(0, std::min(static_cast<int>(display.size()), *cursor));
    display.insert(static_cast<std::size_t>(c), "│");

    std::string hint =
        p->placeholder && !p->placeholder->empty()
            ? std::string{*p->placeholder}
            : "Type your response…";

    body.push_back(hbox({
        text(display) | color(Color::White),
        text("  ") | dim,
        text(hint) | dim,
    }) | xflex);

    body.push_back(separator());
    body.push_back(hbox({
        text("Enter") | bold,
        text(" submit · "),
        text("Esc") | bold,
        text(" cancel"),
    }) | dim);

    const std::string title = p->title.empty() ? "Response" : p->title;
    return window(
        text(" " + title + " ") | bold | color(Color::Cyan),
        vbox(std::move(body)) | xflex
    ) | color(Color::Cyan) | size(WIDTH, LESS_THAN, 92);
}

// =========================================================================
// Public factory
// =========================================================================

/// Build the interactive PromptDialog component.
///
/// Mode dispatch:
///   - !payload.options.empty() -> SELECT mode.
///   -  payload.options.empty() -> FREE-TEXT mode.
[[nodiscard]] inline Component MakePromptDialog(PromptDialogPayload payload)
{
    auto p = std::make_shared<PromptDialogPayload>(std::move(payload));

    // Clamp the initial selected_index into a valid range.
    if (!p->options.empty()) {
        p->selected_index = detail::wrap_index(
            p->selected_index, static_cast<int>(p->options.size()));
    }

    // ---- FREE-TEXT mode -----------------------------------------------
    if (p->options.empty()) {
        auto buffer = std::make_shared<std::string>();
        auto cursor = std::make_shared<int>(0);

        auto submit = [p, buffer] {
            if (p->on_response) p->on_response(std::string{*buffer});
        };
        auto cancel = [p] {
            if (p->on_response) p->on_response(std::nullopt);
        };
        auto abort = [p] {
            if (p->on_abort) p->on_abort();
        };

        return Renderer([p, buffer, cursor] {
            return RenderFreeTextMode(p, buffer, cursor);
        }) | CatchEvent([p, buffer, cursor, submit, cancel, abort](Event event) -> bool {
            // Ctrl+C -> on_abort (distinct from Esc path)
            if (event == Event::Special({0x03})) {
                abort();
                return true;
            }
            if (event == Event::Return) {
                submit();
                return true;
            }
            if (event == Event::Escape) {
                cancel();
                return true;
            }
            if (event == Event::Backspace) {
                const int c = *cursor;
                if (c > 0) {
                    buffer->erase(static_cast<std::size_t>(c - 1), 1);
                    *cursor = c - 1;
                }
                return true;
            }
            if (event == Event::Delete) {
                const std::size_t c = static_cast<std::size_t>(*cursor);
                if (c < buffer->size()) {
                    buffer->erase(c, 1);
                }
                return true;
            }
            if (event == Event::ArrowLeft) {
                *cursor = std::max(0, *cursor - 1);
                return true;
            }
            if (event == Event::ArrowRight) {
                *cursor = std::min(static_cast<int>(buffer->size()), *cursor + 1);
                return true;
            }
            if (event.is_character()) {
                const char ch = event.character()[0];
                // Ignore low control bytes other than the explicit ones above.
                if (ch < 0x20) return true;
                const std::size_t c = static_cast<std::size_t>(*cursor);
                buffer->insert(c, 1, ch);
                *cursor = static_cast<int>(c + 1);
                return true;
            }
            return false;
        });
    }

    // ---- SELECT mode ---------------------------------------------------
    const int n = static_cast<int>(p->options.size());

    auto commit_index = [p](int idx) {
        if (idx < 0 || idx >= static_cast<int>(p->options.size())) return;
        if (p->on_select) {
            p->on_select(p->options[static_cast<std::size_t>(idx)].key);
        } else if (p->on_response) {
            // Fallback: callers that only wire on_response still work.
            p->on_response(p->options[static_cast<std::size_t>(idx)].key);
        }
    };
    auto cancel = [p] {
        if (p->on_response) p->on_response(std::nullopt);
    };
    auto abort = [p] {
        if (p->on_abort) p->on_abort();
    };

    return Renderer([p] { return RenderSelectMode(p); })
         | CatchEvent([p, n, commit_index, cancel, abort](Event event) -> bool {
        // Ctrl+C -> on_abort (distinct from Esc path)
        if (event == Event::Special({0x03})) {
            abort();
            return true;
        }

        // Movement with wrap.
        if (event == Event::ArrowUp || event == Event::Character('k') ||
            event == Event::Character('K'))
        {
            p->selected_index = detail::wrap_index(p->selected_index - 1, n);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j') ||
            event == Event::Character('J'))
        {
            p->selected_index = detail::wrap_index(p->selected_index + 1, n);
            return true;
        }

        // Numeric 1..9 shortcuts (commit immediately).
        if (event.is_character()) {
            const char ch = event.character()[0];
            if (ch >= '1' && ch <= '9') {
                const int idx = static_cast<int>(ch - '1');
                if (idx < n) {
                    p->selected_index = idx;
                    commit_index(idx);
                    return true;
                }
            }
        }

        // Enter commits the focused option.
        if (event == Event::Return) {
            const int sel = detail::wrap_index(p->selected_index, n);
            commit_index(sel);
            return true;
        }

        // Escape => on_response(nullopt).
        if (event == Event::Escape) {
            cancel();
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::dialogs::prompt_dialog
