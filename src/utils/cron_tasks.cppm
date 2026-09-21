module;
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.cron_tasks;

export namespace cc::utils {

// Forward-declare CronScheduler interface to avoid module import
// (In real build, this would import cc.utils.cron_scheduler)

namespace detail {
    struct TaskRecord {
        std::string status;
        std::optional<std::chrono::system_clock::time_point> last_run;
    };

    inline std::map<std::string, TaskRecord>& task_registry() {
        static std::map<std::string, TaskRecord> registry;
        return registry;
    }
} // namespace detail

// Register default background maintenance tasks
// In real implementation, this takes a CronScheduler& parameter
void register_default_cron_tasks() {
    auto& registry = detail::task_registry();

    // Log rotation - daily at 3 AM
    registry["log_rotation"] = detail::TaskRecord{"scheduled", std::nullopt};

    // Session cleanup - every 6 hours
    registry["session_cleanup"] = detail::TaskRecord{"scheduled", std::nullopt};

    // Cache purge - daily at 4 AM
    registry["cache_purge"] = detail::TaskRecord{"scheduled", std::nullopt};

    // Analytics flush - every 30 minutes
    registry["analytics_flush"] = detail::TaskRecord{"scheduled", std::nullopt};

    // Stale lock cleanup - every hour
    registry["stale_lock_cleanup"] = detail::TaskRecord{"scheduled", std::nullopt};
}

// Get the current status of a cron task
std::string get_task_status(std::string_view task_id) {
    auto& registry = detail::task_registry();
    auto it = registry.find(std::string(task_id));
    if (it != registry.end()) {
        return it->second.status;
    }
    return "unknown";
}

// Get the last run time of a cron task
std::optional<std::chrono::system_clock::time_point> get_last_run_time(std::string_view task_id) {
    auto& registry = detail::task_registry();
    auto it = registry.find(std::string(task_id));
    if (it != registry.end()) {
        return it->second.last_run;
    }
    return std::nullopt;
}

} // namespace cc::utils
