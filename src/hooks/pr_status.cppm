module;
#include <chrono>
#include <functional>
#include <optional>
#include <string>

export module cc.hooks.pr_status;

export namespace cc::hooks {

enum class PrReviewState {
  approved,
  changes_requested,
  commented,
  dismissed,
  pending
};

struct PrStatus {
  std::optional<int> number;
  std::optional<std::string> url;
  std::optional<PrReviewState> review_state;
  std::chrono::system_clock::time_point last_updated;
};

using PrStatusChangeCallback = std::function<void(const PrStatus&)>;

class PrStatusHook {
public:
  PrStatusHook() = default;

  auto enable(bool enabled = true) -> void {
    enabled_ = enabled;
  }

  auto is_enabled() const -> bool {
    return enabled_;
  }

  auto set_poll_interval(std::chrono::milliseconds interval) -> void {
    poll_interval_ = interval;
  }

  auto get_status() const -> const PrStatus& {
    return status_;
  }

  auto refresh() -> void {
    last_refresh_ = std::chrono::steady_clock::now();
  }

  auto on_status_change(PrStatusChangeCallback callback) -> void {
    status_callback_ = std::move(callback);
  }

  auto update_status(PrStatus new_status) -> void {
    status_ = std::move(new_status);
    if (status_callback_) {
      status_callback_(status_);
    }
  }

  auto get_last_refresh() const -> std::chrono::steady_clock::time_point {
    return last_refresh_;
  }

private:
  bool enabled_ = true;
  PrStatus status_;
  std::chrono::milliseconds poll_interval_{60000};
  std::chrono::steady_clock::time_point last_refresh_;
  PrStatusChangeCallback status_callback_;
};

}
