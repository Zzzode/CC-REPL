/// @file teleport_dialogs.cppm
/// @brief Teleport session UI — progress display, repo mismatch dialog, error, stash.
/// Migrated from TeleportProgress.tsx, TeleportRepoMismatchDialog.tsx,
/// TeleportError.tsx, TeleportStash.tsx, TeleportResumeWrapper.tsx.
module;

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <algorithm>
#include <cstdint>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.teleport_dialogs;

export namespace cc::ui::teleport_dialogs {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Teleport progress steps (matches TS TeleportProgressStep)
enum class TeleportProgressStep {
    validating,
    fetching_logs,
    fetching_branch,
    checking_out,
};

/// Convert step to display label
[[nodiscard]] inline std::string step_label(TeleportProgressStep step) {
    switch (step) {
        case TeleportProgressStep::validating: return "Validating session";
        case TeleportProgressStep::fetching_logs: return "Fetching session logs";
        case TeleportProgressStep::fetching_branch: return "Getting branch info";
        case TeleportProgressStep::checking_out: return "Checking out branch";
    }
    return "?";
}

/// Teleport result status
enum class TeleportResultStatus {
    success,
    error,
    cancelled,
};

/// Teleport result (matches TS TeleportResult)
struct TeleportResult {
    TeleportResultStatus status = TeleportResultStatus::success;
    std::optional<std::string> error_message;
    std::optional<std::string> session_id;
    std::optional<std::string> branch_name;
};

/// Props for teleport progress display
struct TeleportProgressProps {
    TeleportProgressStep current_step = TeleportProgressStep::validating;
    std::optional<std::string> session_id;
};

/// Props for repo mismatch dialog
struct TeleportRepoMismatchProps {
    std::string target_repo;
    std::vector<std::string> available_paths;
    std::function<void(std::string)> on_select_path;
    std::function<void()> on_cancel;
};

/// Props for teleport error display
struct TeleportErrorProps {
    std::string error_message;
    std::optional<std::string> session_id;
    std::function<void()> on_dismiss;
};

/// Stash operation type
enum class StashAction {
    stash_and_continue,
    abort,
};

/// Props for the stash dialog
struct TeleportStashProps {
    std::string branch_name;
    int dirty_file_count = 0;
    std::function<void(StashAction)> on_action;
};

// ============================================================
// Element Rendering
// ============================================================

/// Spinner frames for teleport progress
inline constexpr std::array<std::string_view, 4> kSpinnerFrames = {
    "\u25D0", "\u25D3", "\u25D1", "\u25D2"
};

/// Render a step in the progress list
[[nodiscard]] inline Element RenderProgressStep(
    TeleportProgressStep step,
    TeleportProgressStep current,
    int spinner_frame) {

    auto all_steps = {
        TeleportProgressStep::validating,
        TeleportProgressStep::fetching_logs,
        TeleportProgressStep::fetching_branch,
        TeleportProgressStep::checking_out,
    };

    int step_idx = 0, current_idx = 0, i = 0;
    for (auto s : all_steps) {
        if (s == step) step_idx = i;
        if (s == current) current_idx = i;
        ++i;
    }

    Element indicator;
    if (step_idx < current_idx) {
        // Completed
        indicator = text(" \u2713 ") | color(Color::Green);
    } else if (step_idx == current_idx) {
        // Current (animated)
        auto frame = std::string{kSpinnerFrames[spinner_frame % 4]};
        indicator = text(" " + frame + " ") | color(Color::Cyan);
    } else {
        // Pending
        indicator = text(" \u25CB ") | dim;
    }

    auto label_el = text(step_label(step));
    if (step_idx == current_idx) label_el = label_el | bold;
    if (step_idx > current_idx) label_el = label_el | dim;

    return hbox({indicator, label_el});
}

/// Render the full teleport progress view
[[nodiscard]] inline Element RenderTeleportProgress(
    const TeleportProgressProps& props, int spinner_frame) {

    auto title_frame = std::string{kSpinnerFrames[spinner_frame % 4]};

    Elements items;
    items.push_back(hbox({
        text(title_frame + " ") | bold | color(Color::Cyan),
        text("Teleporting session\u2026") | bold | color(Color::Cyan),
    }));
    items.push_back(text(""));

    if (props.session_id) {
        items.push_back(text("  " + *props.session_id) | dim);
        items.push_back(text(""));
    }

    const auto steps = {
        TeleportProgressStep::validating,
        TeleportProgressStep::fetching_logs,
        TeleportProgressStep::fetching_branch,
        TeleportProgressStep::checking_out,
    };

    for (auto step : steps) {
        items.push_back(RenderProgressStep(step, props.current_step, spinner_frame));
    }

    return vbox(items);
}

/// Render the repo mismatch dialog
[[nodiscard]] inline Element RenderRepoMismatchDialog(
    const TeleportRepoMismatchProps& props,
    int selected,
    std::optional<std::string> error_msg,
    bool validating) {

    auto make_option = [](std::string_view label, bool is_selected) {
        auto prefix = is_selected
            ? text("\u276F ") | color(Color::Cyan)
            : text("  ");
        auto el = text(std::string{label});
        if (is_selected) el = el | bold;
        return hbox({prefix, el});
    };

    Elements items;
    items.push_back(paragraph("The teleported session was working in repository:"));
    items.push_back(hbox({
        text("  "),
        text(props.target_repo) | bold | color(Color::Cyan),
    }));
    items.push_back(text(""));
    items.push_back(text("Select the local path for this repository:"));
    items.push_back(text(""));

    for (int i = 0; i < static_cast<int>(props.available_paths.size()); ++i) {
        items.push_back(make_option(props.available_paths[i], i == selected));
    }
    items.push_back(make_option("Cancel",
        selected == static_cast<int>(props.available_paths.size())));

    if (error_msg) {
        items.push_back(text(""));
        items.push_back(text("  " + *error_msg) | color(Color::Red));
    }

    if (validating) {
        items.push_back(text(""));
        items.push_back(text("  Validating...") | dim);
    }

    auto body = vbox(items);
    return window(
        text(" Repository Mismatch ") | bold | color(Color::Yellow),
        body
    ) | color(Color::Yellow) | size(WIDTH, LESS_THAN, 75);
}

/// Render the teleport error view
[[nodiscard]] inline Element RenderTeleportError(
    const TeleportErrorProps& props) {

    Elements items;
    items.push_back(hbox({
        text(" \u2717 ") | color(Color::Red),
        text("Teleport Failed") | bold | color(Color::Red),
    }));
    items.push_back(text(""));
    items.push_back(paragraph("  " + props.error_message) | dim);

    if (props.session_id) {
        items.push_back(text(""));
        items.push_back(hbox({
            text("  Session: ") | dim,
            text(*props.session_id) | dim,
        }));
    }

    items.push_back(text(""));
    items.push_back(text("  Press any key to dismiss") | dim);

    return vbox(items) | border | color(Color::Red);
}

/// Render the stash prompt dialog
[[nodiscard]] inline Element RenderStashDialog(
    const TeleportStashProps& props, int selected) {

    auto make_option = [](std::string_view label, bool is_selected) {
        auto prefix = is_selected
            ? text("\u276F ") | color(Color::Cyan)
            : text("  ");
        auto el = text(std::string{label});
        if (is_selected) el = el | bold;
        return hbox({prefix, el});
    };

    auto content = vbox({
        paragraph("Your working tree has uncommitted changes that would "
                  "conflict with the teleport checkout."),
        text(""),
        hbox({
            text("  Branch: "),
            text(props.branch_name) | bold | color(Color::Cyan),
        }),
        hbox({
            text("  Dirty files: "),
            text(std::to_string(props.dirty_file_count)) | bold,
        }),
        text(""),
        make_option("Stash changes and continue", selected == 0),
        make_option("Abort teleport", selected == 1),
    });

    return window(
        text(" Uncommitted Changes ") | bold | color(Color::Yellow),
        content
    ) | color(Color::Yellow) | size(WIDTH, LESS_THAN, 65);
}

// ============================================================
// Interactive Components
// ============================================================

/// Create the teleport progress component (animated)
[[nodiscard]] inline Component TeleportProgressComponent(
    TeleportProgressProps props) {

    auto shared_props = std::make_shared<TeleportProgressProps>(std::move(props));
    auto frame = std::make_shared<int>(0);

    // The caller should tick this component via screen refresh
    return Renderer([shared_props, frame] {
        (*frame)++;
        return RenderTeleportProgress(*shared_props, *frame);
    });
}

/// Create the repo mismatch dialog component
[[nodiscard]] inline Component TeleportRepoMismatchDialog(
    TeleportRepoMismatchProps props) {

    struct State {
        TeleportRepoMismatchProps props;
        int selected = 0;
        std::optional<std::string> error_msg;
        bool validating = false;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    int option_count = static_cast<int>(state->props.available_paths.size()) + 1;

    return Renderer([state] {
        return RenderRepoMismatchDialog(
            state->props, state->selected, state->error_msg, state->validating);
    }) | CatchEvent([state, option_count](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(option_count - 1, state->selected + 1);
            return true;
        }
        if (event == Event::Return) {
            if (state->selected == option_count - 1) {
                // Cancel
                if (state->props.on_cancel) state->props.on_cancel();
            } else {
                // Select path
                if (state->props.on_select_path) {
                    state->props.on_select_path(
                        state->props.available_paths[state->selected]);
                }
            }
            return true;
        }
        if (event == Event::Escape) {
            if (state->props.on_cancel) state->props.on_cancel();
            return true;
        }
        return false;
    });
}

/// Create the teleport error dialog component
[[nodiscard]] inline Component TeleportErrorDialog(TeleportErrorProps props) {
    auto shared_props = std::make_shared<TeleportErrorProps>(std::move(props));

    return Renderer([shared_props] {
        return RenderTeleportError(*shared_props);
    }) | CatchEvent([shared_props](Event event) -> bool {
        // Any key dismisses
        if (event.is_character() || event == Event::Return ||
            event == Event::Escape) {
            if (shared_props->on_dismiss) shared_props->on_dismiss();
            return true;
        }
        return false;
    });
}

/// Create the stash dialog component
[[nodiscard]] inline Component TeleportStashDialog(TeleportStashProps props) {
    struct State {
        TeleportStashProps props;
        int selected = 0;
    };
    constexpr int kStashOptionCount = 2;

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderStashDialog(state->props, state->selected);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected = std::max(0, state->selected - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected = std::min(kStashOptionCount - 1,
                                       state->selected + 1);
            return true;
        }
        if (event == Event::Return) {
            auto action = (state->selected == 0)
                ? StashAction::stash_and_continue
                : StashAction::abort;
            if (state->props.on_action) state->props.on_action(action);
            return true;
        }
        if (event == Event::Escape) {
            if (state->props.on_action) {
                state->props.on_action(StashAction::abort);
            }
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::teleport_dialogs
