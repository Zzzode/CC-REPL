module;
#include <atomic>
export module cc.services.rate_limit.mock;

export namespace cc::services::rate_limit {

// Import PolicyAction equivalent for mock
enum class MockPolicyAction { Allow, Warn, Block };

// Mock rate limiter for testing purposes
class MockRateLimiter {
public:
    // Set the rate limit (requests per minute)
    auto set_limit(int requests_per_minute) -> void {
        limit_ = requests_per_minute;
        current_count_.store(0);
    }

    // Check if a request is allowed
    auto check() -> MockPolicyAction {
        int count = current_count_.fetch_add(1);
        if (count >= limit_) {
            return MockPolicyAction::Block;
        }
        // Warn at 80% capacity
        if (count >= static_cast<int>(limit_ * 0.8)) {
            return MockPolicyAction::Warn;
        }
        return MockPolicyAction::Allow;
    }

    // Reset the counter
    auto reset() -> void {
        current_count_.store(0);
    }

private:
    int limit_{60};
    std::atomic<int> current_count_{0};
};

} // namespace cc::services::rate_limit
