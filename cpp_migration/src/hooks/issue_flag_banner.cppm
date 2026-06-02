module;
#include <string>
#include <functional>
#include <optional>
#include <vector>

export module cc.hooks.issue_flag_banner;

import cc.state.app_state;

export namespace cc::hooks::issue_flag_banner {

enum class BannerSeverity { info, warning, error, critical };

struct IssueBanner {
    std::string id;
    std::string message;
    BannerSeverity severity{BannerSeverity::info};
    std::optional<std::string> action_label;
    std::optional<std::string> action_url;
    bool dismissible{true};
    bool dismissed{false};
};

struct IssueFlagBannerState {
    std::vector<IssueBanner> banners;
    bool has_critical{false};
};

struct IssueFlagBannerOptions {
    bool show_on_startup{true};
    std::size_t max_banners{5};
};

class IssueFlagBannerHook {
public:
    explicit IssueFlagBannerHook(const IssueFlagBannerOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const IssueFlagBannerState& state() const { return state_; }

    void add_banner(IssueBanner banner) {
        if (state_.banners.size() >= options_.max_banners) return;
        if (banner.severity == BannerSeverity::critical) state_.has_critical = true;
        state_.banners.push_back(std::move(banner));
        notify();
    }

    void dismiss(std::string_view id) {
        for (auto& b : state_.banners) {
            if (b.id == id && b.dismissible) {
                b.dismissed = true;
                break;
            }
        }
        update_critical();
        notify();
    }

    void clear_all() {
        state_.banners.clear();
        state_.has_critical = false;
        notify();
    }

    [[nodiscard]] std::vector<IssueBanner> visible_banners() const {
        std::vector<IssueBanner> result;
        for (const auto& b : state_.banners) {
            if (!b.dismissed) result.push_back(b);
        }
        return result;
    }

    void on_change(std::function<void(const IssueFlagBannerState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void update_critical() {
        state_.has_critical = false;
        for (const auto& b : state_.banners) {
            if (!b.dismissed && b.severity == BannerSeverity::critical) {
                state_.has_critical = true;
                break;
            }
        }
    }

    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    IssueFlagBannerState state_;
    IssueFlagBannerOptions options_;
    std::vector<std::function<void(const IssueFlagBannerState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::issue_flag_banner
