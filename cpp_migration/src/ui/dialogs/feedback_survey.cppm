module;
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <chrono>
#include <expected>
#include <cstdint>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.feedback_survey;

export namespace cc::ui::feedback_survey {
using namespace ftxui;
using clock_t = std::chrono::steady_clock;

// ============================================================
// Types
// ============================================================

/// Feedback response options (maps to digit input 0-3)
enum class FeedbackResponse {
    dismissed = 0,
    bad = 1,
    fine = 2,
    good = 3,
};

/// Transcript share response
enum class TranscriptShareResponse {
    yes,
    no,
    dont_ask_again,
};

/// Survey state machine
enum class SurveyState {
    closed,
    open,
    thanks,
    transcript_prompt,
    submitting,
    submitted,
};

/// Configuration for survey display timing
struct FeedbackSurveyConfig {
    std::chrono::milliseconds min_time_before_feedback{600000};
    std::chrono::milliseconds min_time_between_feedback{3600000};
    std::chrono::milliseconds min_time_between_global_feedback{100000000};
    int min_user_turns_before_feedback = 5;
    int min_user_turns_between_feedback = 10;
    std::chrono::milliseconds hide_thanks_after{3000};
    std::vector<std::string> on_for_models{"*"};
    double probability = 0.005;
};

/// Props for the feedback survey view
struct FeedbackSurveyViewProps {
    std::function<void(FeedbackResponse)> on_select;
    std::string input_value;
    std::function<void(std::string)> set_input_value;
    std::optional<std::string> message;
};

/// Props for the main feedback survey component
struct FeedbackSurveyProps {
    std::function<void()> on_close;
    std::function<void(FeedbackResponse)> on_submit;
    std::function<void()> on_skip;
    bool show_skip_button = false;
};

/// Result from the feedback survey hook logic
struct FeedbackSurveyResult {
    SurveyState state = SurveyState::closed;
    std::optional<FeedbackResponse> last_response;
};

// ============================================================
// Utilities
// ============================================================

/// Convert digit character to FeedbackResponse
[[nodiscard]] inline std::expected<FeedbackResponse, std::string>
digit_to_response(char digit) {
    switch (digit) {
        case '0': return FeedbackResponse::dismissed;
        case '1': return FeedbackResponse::bad;
        case '2': return FeedbackResponse::fine;
        case '3': return FeedbackResponse::good;
        default: return std::unexpected("Invalid digit: must be 0-3");
    }
}

/// Convert response enum to display label
[[nodiscard]] inline std::string response_label(FeedbackResponse r) {
    switch (r) {
        case FeedbackResponse::dismissed: return "Dismiss";
        case FeedbackResponse::bad: return "Bad";
        case FeedbackResponse::fine: return "Fine";
        case FeedbackResponse::good: return "Good";
    }
    return "?";
}

/// Default message shown in the survey prompt
inline constexpr std::string_view kDefaultMessage =
    "How is Claude doing this session? (optional)";

// ============================================================
// Element Rendering
// ============================================================

/// Render the response options bar
[[nodiscard]] inline Element RenderResponseOptions() {
    return hbox({
        text("1") | color(Color::Cyan), text(": Bad  "),
        text("2") | color(Color::Cyan), text(": Fine  "),
        text("3") | color(Color::Cyan), text(": Good  "),
        text("0") | color(Color::Cyan), text(": Dismiss"),
    });
}

/// Render the feedback survey view element
[[nodiscard]] inline Element RenderFeedbackSurveyView(
    std::string_view message) {

    auto msg = message.empty()
        ? std::string{kDefaultMessage}
        : std::string{message};

    return vbox({
        hbox({
            text("\u25CF ") | color(Color::Cyan),
            text(msg) | bold,
        }),
        hbox({
            text("  "),
            RenderResponseOptions(),
        }),
    }) | flex;
}

/// Render the "thanks" state after submission
[[nodiscard]] inline Element RenderThanksView() {
    return hbox({
        text("\u2713 ") | color(Color::Green),
        text("Thanks for your feedback!") | dim,
    });
}

/// Render the transcript share prompt
[[nodiscard]] inline Element RenderTranscriptPrompt() {
    return vbox({
        text("Would you like to share this conversation transcript?") | bold,
        text("This helps us improve Claude.") | dim,
        text(""),
        hbox({
            text("1") | color(Color::Cyan), text(": Yes  "),
            text("2") | color(Color::Cyan), text(": No  "),
            text("3") | color(Color::Cyan), text(": Don't ask again"),
        }),
    });
}

/// Render the full survey based on current state
[[nodiscard]] inline Element RenderFeedbackSurvey(
    SurveyState state, std::string_view message) {

    switch (state) {
        case SurveyState::closed:
            return text("");
        case SurveyState::open:
            return RenderFeedbackSurveyView(message);
        case SurveyState::thanks:
            return RenderThanksView();
        case SurveyState::transcript_prompt:
            return RenderTranscriptPrompt();
        case SurveyState::submitting:
            return hbox({
                text("\u25CF ") | color(Color::Cyan),
                text("Submitting...") | dim,
            });
        case SurveyState::submitted:
            return hbox({
                text("\u2713 ") | color(Color::Green),
                text("Transcript shared. Thank you!") | dim,
            });
    }
    return text("");
}

// ============================================================
// Interactive Component
// ============================================================

/// Create the feedback survey interactive component
[[nodiscard]] inline Component FeedbackSurveyComponent(
    FeedbackSurveyProps props) {

    struct State {
        FeedbackSurveyProps props;
        SurveyState state = SurveyState::open;
        std::optional<FeedbackResponse> last_response;
    };

    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderFeedbackSurvey(state->state, "");
    }) | CatchEvent([state](Event event) -> bool {
        if (state->state == SurveyState::open) {
            // Handle digit input for responses
            if (event.is_character() && event.character().size() == 1) {
                char ch = event.character()[0];
                auto resp = digit_to_response(ch);
                if (resp.has_value()) {
                    state->last_response = resp.value();
                    if (resp.value() == FeedbackResponse::dismissed) {
                        state->state = SurveyState::closed;
                        if (state->props.on_close) state->props.on_close();
                    } else {
                        state->state = SurveyState::thanks;
                        if (state->props.on_submit)
                            state->props.on_submit(resp.value());
                    }
                    return true;
                }
            }
            // Escape to dismiss
            if (event == Event::Escape) {
                state->state = SurveyState::closed;
                if (state->props.on_close) state->props.on_close();
                return true;
            }
        }
        return false;
    });
}

/// Create feedback survey with skip button support (overload)
[[nodiscard]] inline Component FeedbackSurveyWithSkip(
    std::function<void()> on_close,
    std::function<void(FeedbackResponse)> on_submit,
    std::function<void()> on_skip) {

    FeedbackSurveyProps props;
    props.on_close = std::move(on_close);
    props.on_submit = std::move(on_submit);
    props.on_skip = std::move(on_skip);
    props.show_skip_button = true;
    return FeedbackSurveyComponent(std::move(props));
}

} // namespace cc::ui::feedback_survey
