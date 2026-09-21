module;

#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.classifier_approvals;

export namespace cc::utils::classifier_approvals {

enum class ApprovalStatus { Approved, Denied, Pending, Expired };

struct ClassifierResult {
    std::string classifier_id;
    ApprovalStatus status;
    std::optional<std::string> reason;
    double confidence{0.0};
};

namespace detail {
    inline std::vector<std::string>& registered_hooks() {
        static std::vector<std::string> s_hooks;
        return s_hooks;
    }
    inline std::vector<ClassifierResult>& recent() {
        static std::vector<ClassifierResult> s_recent;
        return s_recent;
    }
    inline std::mutex& mutex() {
        static std::mutex s_mutex;
        return s_mutex;
    }
}

inline std::expected<ClassifierResult, std::string> check_approval(std::string_view action, std::string_view context) {
    ClassifierResult result{"default", ApprovalStatus::Approved, std::nullopt, 1.0};
    (void)action; (void)context;
    std::lock_guard lock(detail::mutex());
    detail::recent().push_back(result);
    if (detail::recent().size() > 100) {
        detail::recent().erase(detail::recent().begin());
    }
    return result;
}

inline void register_approval_hook(std::string_view hook_id) {
    std::lock_guard lock(detail::mutex());
    detail::registered_hooks().emplace_back(hook_id);
}

inline bool is_pre_approved(std::string_view action) {
    std::lock_guard lock(detail::mutex());
    // Actions are pre-approved if a hook has been registered for them
    for (const auto& hook : detail::registered_hooks()) {
        if (hook == action) return true;
    }
    return false;
}

inline std::vector<ClassifierResult> get_recent_approvals() {
    std::lock_guard lock(detail::mutex());
    return detail::recent();
}

} // namespace cc::utils::classifier_approvals
