module;
#include <chrono>
#include <optional>
#include <string>
export module cc.services.policy.types;

export namespace cc::services::policy {

// Policy limit definition
struct PolicyLimit {
    std::string resource;
    int max_value{0};
    std::chrono::seconds window{3600};
};

// Policy violation record
struct PolicyViolation {
    std::string rule;
    std::string message;
    std::optional<std::chrono::seconds> retry_after;
};

// Action to take when policy is evaluated
enum class PolicyAction { Allow, Warn, Block };

} // namespace cc::services::policy
