// Analytics Service Module
module;
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.services.analytics.index;

import cc.utils.error;

export namespace cc::services::analytics {

using cc::utils::Result;

// Analytics event
struct AnalyticsEvent {
    std::string name;
    std::chrono::system_clock::time_point timestamp;
    std::unordered_map<std::string, std::string> properties;
};

// Analytics service
class AnalyticsService {
public:
    AnalyticsService();
    
    // Log an event
    Result<void> log_event(const AnalyticsEvent& event);
    
    // Log an event with simple parameters
    Result<void> log_event(const std::string& name, const std::unordered_map<std::string, std::string>& properties = {});
    
    // Set user ID for session
    void set_user_id(const std::string& user_id);
    
    // Flush pending events
    Result<void> flush();
    
private:
    std::string user_id_;
    std::vector<AnalyticsEvent> pending_events_;
    std::mutex mutex_;
};

// Constructor
AnalyticsService::AnalyticsService() {}

// Log an event
Result<void> AnalyticsService::log_event(const AnalyticsEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto modified_event = event;
    if (!user_id_.empty()) {
        modified_event.properties["user_id"] = user_id_;
    }
    
    pending_events_.push_back(std::move(modified_event));
    
    // In real implementation, send to analytics sink (Datadog, etc.)
    return {};
}

// Log an event with simple params
Result<void> AnalyticsService::log_event(const std::string& name, const std::unordered_map<std::string, std::string>& properties) {
    AnalyticsEvent event;
    event.name = name;
    event.timestamp = std::chrono::system_clock::now();
    event.properties = properties;
    return log_event(event);
}

// Set user ID
void AnalyticsService::set_user_id(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_id_ = user_id;
}

// Flush pending events
Result<void> AnalyticsService::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // In real implementation, send pending events
    pending_events_.clear();
    
    return {};
}

} // namespace cc::services::analytics
