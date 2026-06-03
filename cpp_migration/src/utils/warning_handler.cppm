module;
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstddef>
#include <atomic>

export module cc.utils.warning_handler;

export namespace cc::utils {


class WarningHandler {
public:

    static WarningHandler& instance() {
        static WarningHandler handler;
        return handler;
    }



    bool warn(std::string_view id, std::string_view message) {
        std::lock_guard lock(mutex_);
        std::string id_str(id);


        if (suppressed_ids_.contains(id_str)) return false;


        auto [it, inserted] = seen_warnings_.try_emplace(id_str, std::string(message));
        if (!inserted) return false;

        warning_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }


    void suppress(std::string_view id) {
        std::lock_guard lock(mutex_);
        suppressed_ids_.emplace(id);
    }


    void unsuppress(std::string_view id) {
        std::lock_guard lock(mutex_);
        suppressed_ids_.erase(std::string(id));
    }


    [[nodiscard]] bool has_warned(std::string_view id) const {
        std::lock_guard lock(mutex_);
        return seen_warnings_.contains(std::string(id));
    }


    [[nodiscard]] bool is_suppressed(std::string_view id) const {
        std::lock_guard lock(mutex_);
        return suppressed_ids_.contains(std::string(id));
    }


    [[nodiscard]] size_t get_warning_count() const {
        return warning_count_.load(std::memory_order_relaxed);
    }


    [[nodiscard]] std::string get_warning_message(std::string_view id) const {
        std::lock_guard lock(mutex_);
        auto it = seen_warnings_.find(std::string(id));
        if (it != seen_warnings_.end()) return it->second;
        return {};
    }


    void reset() {
        std::lock_guard lock(mutex_);
        seen_warnings_.clear();
        suppressed_ids_.clear();
        warning_count_.store(0, std::memory_order_relaxed);
    }

private:
    WarningHandler() = default;
    ~WarningHandler() = default;


    WarningHandler(const WarningHandler&) = delete;
    WarningHandler& operator=(const WarningHandler&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> seen_warnings_; // id → message
    std::unordered_set<std::string> suppressed_ids_;
    std::atomic<size_t> warning_count_{0};
};

} // namespace cc::utils
