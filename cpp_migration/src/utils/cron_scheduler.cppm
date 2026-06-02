module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module cc.utils.cron_scheduler;

export namespace cc::utils {

// CronExpression is simplified here to avoid cross-module deps at build time
struct CronScheduleExpr {
    int minute = -1;  // -1 = wildcard
    int hour = -1;
    int day = -1;
    int month = -1;
    int weekday = -1;
    int step_minute = 0;
};

struct CronJob {
    std::string id;
    std::string name;
    CronScheduleExpr schedule;
    std::function<void()> handler;
};

class CronScheduler {
public:
    CronScheduler() = default;
    ~CronScheduler() { stop(); }

    // Non-copyable, non-movable
    CronScheduler(const CronScheduler&) = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;

    void add_job(CronJob job) {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }

    void remove_job(std::string_view id) {
        std::lock_guard lock(mutex_);
        std::erase_if(jobs_, [id](const CronJob& j) { return j.id == id; });
    }

    void start() {
        if (running_.exchange(true)) return; // Already running

        worker_ = std::thread([this] {
            while (running_) {
                std::unique_lock lock(mutex_);
                // Wait for 1 minute or until stopped
                cv_.wait_for(lock, std::chrono::seconds(30), [this] { return !running_.load(); });

                if (!running_) break;

                auto now = std::chrono::system_clock::now();
                auto t = std::chrono::system_clock::to_time_t(now);
                std::tm tm{};
                localtime_r(&t, &tm);

                // Check each job
                for (auto& job : jobs_) {
                    if (matches(job.schedule, tm)) {
                        // Run handler outside the lock
                        auto handler = job.handler;
                        lock.unlock();
                        if (handler) handler();
                        lock.lock();
                    }
                }
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Get next N scheduled job executions
    std::vector<std::pair<CronJob, std::chrono::system_clock::time_point>> get_next_jobs(std::size_t n) {
        std::lock_guard lock(mutex_);
        std::vector<std::pair<CronJob, std::chrono::system_clock::time_point>> result;

        auto now = std::chrono::system_clock::now();
        for (auto& job : jobs_) {
            if (result.size() >= n) break;
            // Approximate next run (simplified)
            auto next = now + std::chrono::minutes(1);
            result.emplace_back(job, next);
        }

        return result;
    }

private:
    bool matches(const CronScheduleExpr& sched, const std::tm& tm) {
        if (sched.minute != -1 && tm.tm_min != sched.minute) return false;
        if (sched.step_minute > 0 && tm.tm_min % sched.step_minute != 0) return false;
        if (sched.hour != -1 && tm.tm_hour != sched.hour) return false;
        if (sched.day != -1 && tm.tm_mday != sched.day) return false;
        if (sched.month != -1 && (tm.tm_mon + 1) != sched.month) return false;
        if (sched.weekday != -1 && tm.tm_wday != sched.weekday) return false;
        return true;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::vector<CronJob> jobs_;
};

} // namespace cc::utils
