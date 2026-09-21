module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>

export module cc.hooks.skill_improvement_survey;

import cc.state.app_state;

export namespace cc::hooks::skill_improvement_survey {

enum class SurveyRating { very_poor = 1, poor = 2, neutral = 3, good = 4, excellent = 5 };

struct SurveyResponse {
    std::string skill_name;
    SurveyRating rating;
    std::optional<std::string> feedback_text;
    std::chrono::system_clock::time_point submitted_at;
};

struct SkillImprovementSurveyState {
    bool survey_pending{false};
    std::optional<std::string> current_skill;
    std::vector<SurveyResponse> responses;
    bool dismissed{false};
};

struct SkillImprovementSurveyOptions {
    std::chrono::minutes delay_before_show{5};
    bool require_text_feedback{false};
    std::size_t min_interactions_before_survey{3};
};

class SkillImprovementSurveyHook {
public:
    explicit SkillImprovementSurveyHook(const SkillImprovementSurveyOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const SkillImprovementSurveyState& state() const { return state_; }

    void trigger_survey(std::string skill_name) {
        state_.current_skill = std::move(skill_name);
        state_.survey_pending = true;
        state_.dismissed = false;
        notify();
    }

    void submit(SurveyRating rating, std::optional<std::string> feedback = std::nullopt) {
        if (!state_.current_skill) return;
        SurveyResponse response{
            .skill_name = *state_.current_skill,
            .rating = rating,
            .feedback_text = std::move(feedback),
            .submitted_at = std::chrono::system_clock::now()
        };
        state_.responses.push_back(std::move(response));
        state_.survey_pending = false;
        state_.current_skill = std::nullopt;
        notify();
    }

    void dismiss() {
        state_.survey_pending = false;
        state_.dismissed = true;
        state_.current_skill = std::nullopt;
        notify();
    }

    [[nodiscard]] std::size_t total_responses() const { return state_.responses.size(); }

    void on_change(std::function<void(const SkillImprovementSurveyState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    SkillImprovementSurveyState state_;
    SkillImprovementSurveyOptions options_;
    std::vector<std::function<void(const SkillImprovementSurveyState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::skill_improvement_survey
