/// @file diff_dialog.cppm
/// @brief Full-screen diff dialog — mirrors diff/DiffDialog.tsx +
/// diff/DiffFileList.tsx. Left file list with per-file stats, right diff
/// detail, top toolbar, bottom status, full j/k/Enter/q/Esc/Tab key nav.
///
/// Reuses:
///   • cc.ui.components.diff_view        (unified-diff rendering, parsing)
///   • cc.ui.structured_diff             (summary bar, block-aware rendering)
///   • cc.utils.file_edit                 (Myers algorithm — NOT rewritten)
///
/// Integration:
///   • The dialog is exposed as a factory component (DiffDialog).
///   • In repl_screen, add ReplMode::DiffView and branch on it in the
///     overlay renderer. See comment block at the bottom of this file for
///     the suggested DialogRouter integration snippet.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <algorithm>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.dialogs.diff_dialog;

import cc.types.types;
import cc.utils.file_edit;
import cc.ui.components.diff_view;
import cc.ui.structured_diff;

export namespace cc::ui::dialogs::diff_dialog {
using namespace ftxui;
namespace dv = cc::ui::components::diff_view;
namespace sd = cc::ui::structured_diff;

// ============================================================
// Types
// ============================================================

/// Single file entry in the diff dialog. Matches the `DiffFile` shape used
/// by hooks/useDiffData.ts (TS source of truth).
struct DiffFileEntry {
    std::string path;
    int lines_added = 0;
    int lines_removed = 0;
    bool is_binary = false;
    bool is_large_file = false;
    bool is_truncated = false;
    bool is_new_file = false;
    bool is_untracked = false;
    std::string old_content;   // may be empty (for new / untracked files)
    std::string new_content;   // may be empty (for deleted files)
    // Cached structured hunks (lazily built)
    mutable std::optional<std::vector<sd::StructuredPatchHunk>> cached_hunks;
    // Cached FileDiff for unified rendering
    mutable std::optional<dv::FileDiff> cached_file_diff;
};

struct DiffDialogStats {
    int files_changed = 0;
    int lines_added = 0;
    int lines_removed = 0;
};

struct DiffDialogOptions {
    std::vector<DiffFileEntry> files;
    std::string title = "Uncommitted changes";   // "Turn N" or header text
    std::string subtitle;                         // prompt preview or subtitle

    // Callbacks
    std::function<void(int file_index)> on_file_open;   // Enter on file
    std::function<void(const DiffFileEntry&,
                       const std::vector<sd::StructuredPatchHunk>&)>
        on_copy_patch;
    std::function<void(const std::vector<DiffFileEntry>&)> on_save_patch;
    std::function<void()> on_search_toggle;
    std::function<void()> on_close;
};

// ============================================================
// Helpers
// ============================================================

[[nodiscard]] inline const std::vector<sd::StructuredPatchHunk>&
get_hunks(const DiffFileEntry& f) {
    if (!f.cached_hunks) {
        auto phunks = cc::utils::file_edit::compute_structured_patch(
            f.old_content, f.new_content, 3);
        std::vector<sd::StructuredPatchHunk> shunks;
        int adds = 0, dels = 0;
        for (const auto& ph : phunks) {
            sd::StructuredPatchHunk sh;
            sh.old_start = ph.old_start;
            sh.old_lines = ph.old_lines;
            sh.new_start = ph.new_start;
            sh.new_lines = ph.new_lines;
            int ol = ph.old_start, nl = ph.new_start;
            for (const auto& l : ph.lines) {
                if (l.empty()) continue;
                sd::StructuredDiffLine sdl;
                char p = l[0];
                std::string content = l.size() > 1 ? l.substr(1) : "";
                if (p == '+') {
                    sdl.type = sd::StructuredDiffLine::Type::Added;
                    sdl.new_line_num = nl++; ++adds;
                } else if (p == '-') {
                    sdl.type = sd::StructuredDiffLine::Type::Removed;
                    sdl.old_line_num = ol++; ++dels;
                } else {
                    sdl.type = sd::StructuredDiffLine::Type::Context;
                    sdl.old_line_num = ol++; sdl.new_line_num = nl++;
                }
                sdl.content = std::move(content);
                sh.lines.push_back(std::move(sdl));
            }
            shunks.push_back(std::move(sh));
        }
        // (void) adds/dels: stats are carried on DiffFileEntry already.
        sd::annotate_word_changes(shunks);
        f.cached_hunks = std::move(shunks);
    }
    return *f.cached_hunks;
}

[[nodiscard]] inline const dv::FileDiff& get_file_diff(const DiffFileEntry& f) {
    if (!f.cached_file_diff) {
        dv::FileDiff fd = dv::BuildFileDiffFromContents(
            f.path, f.path, f.old_content, f.new_content, 3);
        if (f.is_binary) fd.is_binary = true;
        if (f.is_new_file) fd.status = "added";
        if (f.is_untracked) fd.status = "added";
        f.cached_file_diff = std::move(fd);
    }
    return *f.cached_file_diff;
}

// Compute summary stats across all files.
[[nodiscard]] inline DiffDialogStats compute_stats(
    const std::vector<DiffFileEntry>& files) {
    DiffDialogStats s;
    s.files_changed = static_cast<int>(files.size());
    for (const auto& f : files) {
        s.lines_added += f.lines_added;
        s.lines_removed += f.lines_removed;
    }
    return s;
}

// ============================================================
// File List (left pane)
// ============================================================

/// Render one entry in the left file list. Corresponds to FileItem in
/// DiffFileList.tsx + FileStats (shows +adds / -dels / "untracked" /
/// "Binary file" / "Large file modified" labels).
[[nodiscard]] inline Element RenderFileListItem(const DiffFileEntry& f,
                                                 bool selected) {
    // Pointer indicator
    auto pointer = selected ? text("▶ ") | color(Color::Cyan) | bold
                            : text("  ");

    // Status badge
    std::string badge = " M ";
    Color badge_color = Color::Yellow;
    if (f.is_new_file || f.is_untracked) {
        badge = " A "; badge_color = Color::Green;
    } else if (f.lines_added == 0 && f.lines_removed > 0) {
        badge = " D "; badge_color = Color::Red;
    } else if (f.is_binary) {
        badge = " B "; badge_color = Color::Magenta;
    }
    auto badge_el = text(badge) | color(badge_color) | bold;

    // Path label
    auto path = text(f.path) | color(selected ? Color::White : Color::GrayLight);

    // Stats on the right
    Element stats;
    if (f.is_untracked) {
        stats = text("untracked") | color(Color::GrayDark) | dim | italic;
    } else if (f.is_binary) {
        stats = text("binary") | color(Color::GrayDark) | dim | italic;
    } else if (f.is_large_file) {
        stats = text("large file modified") | color(Color::GrayDark) | dim | italic;
    } else {
        Elements sp;
        if (f.lines_added > 0) {
            sp.push_back(text(std::format("+{}", f.lines_added))
                         | color(Color::Green) | (selected ? bold : dim));
        }
        if (f.lines_added > 0 && f.lines_removed > 0) {
            sp.push_back(text(" "));
        }
        if (f.lines_removed > 0) {
            sp.push_back(text(std::format("-{}", f.lines_removed))
                         | color(Color::Red) | (selected ? bold : dim));
        }
        if (f.is_truncated) {
            sp.push_back(text(" (truncated)") | dim | color(Color::GrayDark));
        }
        stats = hbox(std::move(sp));
    }

    auto row = hbox({
        pointer,
        badge_el,
        path | flex,
        text("  "),
        stats,
    });

    if (selected) {
        row = row | bgcolor(Color::RGB(40, 40, 60)) | inverted;
    }
    return row;
}

/// Render the complete left file list panel.
[[nodiscard]] inline Element RenderFileList(const DiffDialogOptions& opts,
                                             int selected_index) {
    Elements els;
    els.push_back(text(" Files (j/k navigate, Enter open)") | bold | dim);
    els.push_back(separator());

    if (opts.files.empty()) {
        els.push_back(text(" No changed files") | dim | center);
    }

    // Windowed rendering: show up to ~80 items (matches TS DiffFileList's
    // MAX_VISIBLE_FILES windowing). We show all items for simplicity; the
    // FTXUI yframe will clip and scroll.
    for (int i = 0; i < static_cast<int>(opts.files.size()); ++i) {
        els.push_back(RenderFileListItem(opts.files[i], i == selected_index));
    }

    return vbox(els) | yframe | flex | size(WIDTH, EQUAL, 48);
}

// ============================================================
// Toolbar (top) & Status Bar (bottom)
// ============================================================

/// Toolbar buttons: Search / Expand All / Collapse / Copy / Save Patch.
/// Uses text-only buttons for accessibility and to match Ink TUI style.
[[nodiscard]] inline Element RenderToolbar(bool search_active,
                                           bool /*expand_all_active*/) {
    auto make_button = [](std::string_view label, char accel, bool active) {
        auto base = hbox({
            text("["),
            text(std::string(1, accel)) | color(active ? Color::Yellow
                                                       : Color::Cyan) | bold,
            text("] "),
            text(std::string(label)) | (active ? bold : color(Color::GrayLight)),
        });
        return base;
    };
    return hbox({
        text(" "),
        make_button("Search", '/', search_active),
        text("   "),
        make_button("ExpandAll", 'E', false),
        text("   "),
        make_button("Collapse", 'C', false),
        text("   "),
        make_button("Copy", 'y', false),
        text("   "),
        make_button("SavePatch", 's', false),
        filler(),
        text("Tab ↔ focus "),
    });
}

/// Bottom status bar with overall stats + keyboard legend.
[[nodiscard]] inline Element RenderStatusBar(const DiffDialogStats& s,
                                              const std::string& hint) {
    Elements left;
    left.push_back(text(" ") | dim);
    left.push_back(text(std::format("{} file{} changed",
                                   s.files_changed,
                                   s.files_changed == 1 ? "" : "s")) | dim);
    if (s.lines_added > 0) {
        left.push_back(text(" "));
        left.push_back(text(std::format("+{}", s.lines_added))
                       | color(Color::Green) | bold);
    }
    if (s.lines_removed > 0) {
        left.push_back(text(" "));
        left.push_back(text(std::format("-{}", s.lines_removed))
                       | color(Color::Red) | bold);
    }
    if (!hint.empty()) {
        left.push_back(text("  "));
        left.push_back(text(hint) | color(Color::Yellow) | dim);
    }

    Elements right;
    right.push_back(text("[j/k] nav ") | dim);
    right.push_back(text("[Enter] view ") | dim);
    right.push_back(text("[/] search ") | dim);
    right.push_back(text("[q/Esc] close ") | dim);

    return hbox({
        hbox(std::move(left)),
        filler(),
        hbox(std::move(right)),
    }) | bgcolor(Color::RGB(20, 20, 25));
}

// ============================================================
// Main Rendering
// ============================================================

/// Render the dialog body (called by both the Component and plain Element
/// factory). Exposed here so non-interactive consumers can still preview.
[[nodiscard]] inline Element RenderDiffDialogBody(const DiffDialogOptions& opts,
                                                   int selected_index,
                                                   bool detail_focus,
                                                   bool search_active,
                                                   const std::string& hint) {
    int file_count = static_cast<int>(opts.files.size());
    auto stats = compute_stats(opts.files);

    // Header
    Element header;
    {
        Elements h;
        h.push_back(text(" ") | dim);
        h.push_back(text(opts.title) | bold | color(Color::Cyan));
        if (!opts.subtitle.empty()) {
            h.push_back(text(" " + opts.subtitle) | dim);
        }
        h.push_back(filler());
        header = hbox(std::move(h));
    }

    // Toolbar
    auto toolbar = RenderToolbar(search_active, false);

    // Content: left file list + right diff detail
    Element left_panel = RenderFileList(opts, selected_index);
    Element right_panel;

    if (file_count == 0) {
        right_panel = vbox({
            text("") | center,
            text(" (no files to display)") | dim | center,
            text("") | center,
        }) | flex | border;
    } else {
        int idx = std::clamp(selected_index, 0, file_count - 1);
        const auto& f = opts.files[idx];
        const auto& hunks = get_hunks(f);

        // Build structured diff options
        sd::DiffDetailViewProps props;
        props.file_path = f.path;
        props.hunks = hunks;
        props.is_binary = f.is_binary;
        props.is_large_file = f.is_large_file;
        props.is_truncated = f.is_truncated;
        props.is_untracked = f.is_untracked;

        sd::StructuredDiffOptions sd_opts;
        sd_opts.diff = props;
        sd_opts.visible_height = 60;
        sd_opts.max_display_lines = 5000;

        auto summary = sd::RenderSummaryBar(1, f.lines_added, f.lines_removed);
        auto body = sd::RenderStructuredDiff(sd_opts);

        right_panel = vbox({
            summary,
            separator(),
            body,
        }) | flex | border;
    }

    // Bottom status
    auto status = RenderStatusBar(stats, hint);

    // Focus border highlight on file list / detail
    if (!detail_focus) {
        left_panel = left_panel
            | borderStyled(Color::Cyan)
            | size(WIDTH, EQUAL, 50);
    } else {
        left_panel = left_panel | border;
    }

    auto content = hbox({
        left_panel,
        right_panel | focusPositionRelative(detail_focus ? 1.f : 0.f, 0.f) | flex,
    }) | flex;

    // Wrap as full-screen modal window.
    auto windowed = Window({
        .title = header,
        .inner = vbox({
            toolbar,
            separator(),
            content,
            separator(),
            status,
        }),
    }) | size(WIDTH, EQUAL, 95) | size(HEIGHT, EQUAL, 90);

    return windowed;
}

// ============================================================
// Interactive Component
// ============================================================

enum class FocusPanel : std::uint8_t {
    FileList,
    Detail,
};

/// Interactive diff dialog component.
[[nodiscard]] inline Component DiffDialog(DiffDialogOptions options) {
    struct State {
        DiffDialogOptions opts;
        int selected_index = 0;
        FocusPanel focus = FocusPanel::FileList;
        bool search_active = false;
        std::string hint;
    };
    auto state = std::make_shared<State>();
    state->opts = std::move(options);

    return Renderer([state] {
        return RenderDiffDialogBody(
            state->opts,
            state->selected_index,
            state->focus == FocusPanel::Detail,
            state->search_active,
            state->hint);
    }) | CatchEvent([state](Event event) -> bool {
        const int n = static_cast<int>(state->opts.files.size());

        // Close
        if (event == Event::Character('q') || event == Event::Escape) {
            if (state->opts.on_close) state->opts.on_close();
            return true;
        }

        // Tab / Shift+Tab switch focus
        if (event == Event::Tab) {
            state->focus = (state->focus == FocusPanel::FileList)
                           ? FocusPanel::Detail : FocusPanel::FileList;
            return true;
        }
        if (event == Event::TabReverse) {
            state->focus = (state->focus == FocusPanel::FileList)
                           ? FocusPanel::Detail : FocusPanel::FileList;
            return true;
        }

        // Search toggle
        if (event == Event::Character('/')) {
            state->search_active = !state->search_active;
            state->hint = state->search_active ? "search active" : "";
            if (state->opts.on_search_toggle) state->opts.on_search_toggle();
            return true;
        }

        // Toolbar accelerators (global, regardless of focus)
        if (event == Event::Character('y') || event == Event::Character('Y')) {
            if (n > 0 && state->opts.on_copy_patch) {
                int idx = std::clamp(state->selected_index, 0, n - 1);
                state->opts.on_copy_patch(state->opts.files[idx],
                                          get_hunks(state->opts.files[idx]));
                state->hint = "patch copied";
            }
            return true;
        }
        if (event == Event::Character('s') || event == Event::Character('S')) {
            if (state->opts.on_save_patch) {
                state->opts.on_save_patch(state->opts.files);
                state->hint = "patch saved";
            }
            return true;
        }
        if (event == Event::Character('E')) {
            state->hint = "expanded";
            return true;
        }
        if (event == Event::Character('C')) {
            state->hint = "collapsed";
            return true;
        }

        if (state->focus == FocusPanel::FileList) {
            if (event == Event::ArrowUp || event == Event::Character('k')) {
                state->selected_index = std::max(0, state->selected_index - 1);
                state->hint.clear();
                return true;
            }
            if (event == Event::ArrowDown || event == Event::Character('j')) {
                state->selected_index = std::max(0, std::min(n - 1,
                    state->selected_index + 1));
                state->hint.clear();
                return true;
            }
            if (event == Event::PageDown) {
                state->selected_index = std::max(0, std::min(n - 1,
                    state->selected_index + 20));
                return true;
            }
            if (event == Event::PageUp) {
                state->selected_index = std::max(0, state->selected_index - 20);
                return true;
            }
            if (event == Event::Home) {
                state->selected_index = 0; return true;
            }
            if (event == Event::End) {
                state->selected_index = std::max(0, n - 1); return true;
            }
            if (event == Event::Return || event == Event::ArrowRight) {
                state->focus = FocusPanel::Detail;
                if (state->opts.on_file_open) {
                    state->opts.on_file_open(state->selected_index);
                }
                return true;
            }
        } else {
            // Detail-panel nav: structural diff is passive/scrollable,
            // we delegate scrolling to yframe. Left arrow goes back to list.
            if (event == Event::ArrowLeft) {
                state->focus = FocusPanel::FileList;
                return true;
            }
            // Forward detail Enter -> same as file open callback
            if (event == Event::Return) {
                if (state->opts.on_file_open) {
                    state->opts.on_file_open(state->selected_index);
                }
                return true;
            }
        }

        return false;
    });
}

// ===========================================================================
// DialogRouter integration — suggested code block for repl_screen.cppm.
// (Pasted here for the Phase 4 UI7 audit trail; the repl_screen agent may
//  copy this snippet into their ReplMode switch.)
//
//  1. Add to ReplMode enum:
//
//     enum class ReplMode : std::uint8_t {
//         ...
//         DiffView,         // Full-screen diff dialog (UI7)
//     };
//
//  2. Add to ReplScreenState:
//
//     struct DiffOverlayState {
//       bool open = false;
//       std::vector<DiffFileEntry> files;
//       std::string title;
//       std::string subtitle;
//       int selected_file = 0;
//     };
//     std::optional<DiffOverlayState> diff_overlay;
//
//  3. In the overlay branch of RenderReplScreen():
//
//     if (state.mode == ReplMode::DiffView && state.diff_overlay) {
//       DiffDialogOptions dlg;
//       dlg.files = state.diff_overlay->files;
//       dlg.title = state.diff_overlay->title;
//       dlg.subtitle = state.diff_overlay->subtitle;
//       dlg.on_close = [&]{ state.mode = ReplMode::Normal;
//                           state.diff_overlay.reset(); };
//       ... wire up other callbacks ...
//       overlays.push_back(DiffDialog(std::move(dlg)));
//     }
//
//  4. assistant_text_message / message_response.cppm wire-up:
//       • import cc.ui.dialogs.diff_dialog;
//       • When a code block in an assistant message has a "diff" or
//         "patch" language tag, use RenderStructuredDiff() instead of
//         RenderCodeBlock() — the factory is exposed by
//         cc.ui.structured_diff module.
// ===========================================================================

} // namespace cc::ui::dialogs::diff_dialog
