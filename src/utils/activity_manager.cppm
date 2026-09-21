module;

#include <functional>
#include <set>
#include <string>

export module cc.utils.activity_manager;

export namespace cc::utils::activity_manager {

enum class ActivityType : unsigned char {
    User,
    Cli,
};

struct ActiveTimeRecord {
    double seconds = 0.0;
    ActivityType type = ActivityType::User;
};

struct ActivityStates {
    bool is_user_active = false;
    bool is_cli_active = false;
    std::size_t active_operation_count = 0;
};

class ActivityManager {
public:
    using NowFn = std::function<long long()>;
    using RecordFn = std::function<void(double, ActivityType)>;

    explicit ActivityManager(NowFn get_now, RecordFn record_active_time)
        : get_now_(std::move(get_now)), record_active_time_(std::move(record_active_time)), last_cli_recorded_time_(get_now_()) {}

    void record_user_activity() {
        if (!is_cli_active_ && last_user_activity_time_ != 0) {
            const auto now = get_now_();
            const double elapsed = static_cast<double>(now - last_user_activity_time_) / 1000.0;
            if (elapsed > 0 && elapsed < user_activity_timeout_ms_ / 1000.0 && record_active_time_) {
                record_active_time_(elapsed, ActivityType::User);
            }
        }
        last_user_activity_time_ = get_now_();
    }

    void start_cli_activity(const std::string& operation_id) {
        if (active_operations_.contains(operation_id)) {
            end_cli_activity(operation_id);
        }
        const bool was_empty = active_operations_.empty();
        active_operations_.insert(operation_id);
        if (was_empty) {
            is_cli_active_ = true;
            last_cli_recorded_time_ = get_now_();
        }
    }

    void end_cli_activity(const std::string& operation_id) {
        active_operations_.erase(operation_id);
        if (active_operations_.empty()) {
            const auto now = get_now_();
            const double elapsed = static_cast<double>(now - last_cli_recorded_time_) / 1000.0;
            if (elapsed > 0 && record_active_time_) {
                record_active_time_(elapsed, ActivityType::Cli);
            }
            last_cli_recorded_time_ = now;
            is_cli_active_ = false;
        }
    }

    [[nodiscard]] ActivityStates get_activity_states() const {
        const auto now = get_now_();
        const double since_user_activity = static_cast<double>(now - last_user_activity_time_) / 1000.0;
        return ActivityStates{
            .is_user_active = since_user_activity < user_activity_timeout_ms_ / 1000.0,
            .is_cli_active = is_cli_active_,
            .active_operation_count = active_operations_.size(),
        };
    }

private:
    static constexpr double user_activity_timeout_ms_ = 5000.0;
    NowFn get_now_;
    RecordFn record_active_time_;
    std::set<std::string> active_operations_;
    long long last_user_activity_time_ = 0;
    long long last_cli_recorded_time_ = 0;
    bool is_cli_active_ = false;
};

} // namespace cc::utils::activity_manager
