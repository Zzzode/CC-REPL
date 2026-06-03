// Analytics Service Module
module;
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
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
    std::filesystem::path sink_path_;
    std::ofstream sink_;

    static std::filesystem::path default_sink_path();
    static std::string json_escape(std::string_view value);
    static std::string serialize_event(const AnalyticsEvent& event);
    Result<void> open_sink();
};

// Constructor
AnalyticsService::AnalyticsService()
    : sink_path_(default_sink_path()) {}

// Log an event
Result<void> AnalyticsService::log_event(const AnalyticsEvent& event) {
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto modified_event = event;
        if (!user_id_.empty()) {
            modified_event.properties["user_id"] = user_id_;
        }

        pending_events_.push_back(std::move(modified_event));
        should_flush = pending_events_.size() >= 50;
    }
    if (should_flush) {
        return flush();
    }
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
    if (pending_events_.empty()) return {};
    auto opened = open_sink();
    if (!opened) return opened;
    for (const auto& event : pending_events_) {
        sink_ << serialize_event(event) << '\n';
    }
    sink_.flush();
    pending_events_.clear();
    return {};
}

std::filesystem::path AnalyticsService::default_sink_path() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "cc-repl" / "analytics.ndjson";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "state" / "cc-repl" / "analytics.ndjson";
    }
    return std::filesystem::temp_directory_path() / "cc-repl" / "analytics.ndjson";
}

std::string AnalyticsService::json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(ch));
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

std::string AnalyticsService::serialize_event(const AnalyticsEvent& event) {
    auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.timestamp.time_since_epoch()).count();
    std::string props = "{";
    bool first = true;
    for (const auto& [key, value] : event.properties) {
        if (!first) props += ',';
        first = false;
        props += std::format(R"("{}":"{}")", json_escape(key), json_escape(value));
    }
    props += "}";
    return std::format(
        R"({{"name":"{}","ts":{},"properties":{}}})",
        json_escape(event.name),
        ts_ms,
        props);
}

Result<void> AnalyticsService::open_sink() {
    if (sink_.is_open()) return {};
    std::error_code ec;
    std::filesystem::create_directories(sink_path_.parent_path(), ec);
    if (ec) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::io_error,
            "Failed to create analytics directory: " + ec.message()));
    }
    sink_.open(sink_path_, std::ios::app);
    if (!sink_.is_open()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::io_error,
            "Failed to open analytics sink: " + sink_path_.string()));
    }
    return {};
}

} // namespace cc::services::analytics
