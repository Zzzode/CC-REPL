/// @file worktree_exit_dialog.cppm
/// @brief Worktree exit confirmation dialog
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.worktree_exit_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
struct WorktreeExitInfo { std::string worktree_path; std::string branch; bool has_uncommitted_changes{false}; std::vector<std::string> modified_files; };
[[nodiscard]] inline Element render_worktree_exit(const WorktreeExitInfo& info) {
    std::vector<Element> elements;
    elements.push_back(text("Exit Worktree?") | bold);
    elements.push_back(separator());
    elements.push_back(text("Branch: " + info.branch));
    elements.push_back(text("Path: " + info.worktree_path) | dim);
    if (info.has_uncommitted_changes) {
        elements.push_back(text("WARNING: Uncommitted changes!") | color(Color::Yellow));
        for (const auto& f : info.modified_files) elements.push_back(text("  " + f) | dim);
    }
    return vbox(elements) | border;
}
} // namespace
