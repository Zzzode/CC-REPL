module;
#include <string>
#include <chrono>
#include <atomic>
#include <mutex>

export module cc.bridge.status_util;

export namespace cc::bridge {

// Bridge connection status enumeration
enum class BridgeStatus {
    Disconnected,
    Connecting,
    Connected,
    Error,
    Reconnecting
};

// Message statistics
struct MessageStats {
    int sent;
    int received;
    int errors;
};

namespace detail {
    inline std::atomic<BridgeStatus>& get_status_ref() {
        static std::atomic<BridgeStatus> status{BridgeStatus::Disconnected};
        return status;
    }

    inline std::chrono::steady_clock::time_point& get_connected_since() {
        static std::chrono::steady_clock::time_point since;
        return since;
    }

    inline MessageStats& get_stats_ref() {
        static MessageStats stats{0, 0, 0};
        return stats;
    }

    inline std::mutex& get_stats_mutex() {
        static std::mutex mutex;
        return mutex;
    }
}

// Get the current bridge connection status
BridgeStatus get_bridge_status() {
    return detail::get_status_ref().load();
}

// Format the bridge status with ANSI color codes for terminal display
std::string format_bridge_status(BridgeStatus status) {
    switch (status) {
        case BridgeStatus::Disconnected:
            return "\033[31m● Disconnected\033[0m";
        case BridgeStatus::Connecting:
            return "\033[33m◌ Connecting...\033[0m";
        case BridgeStatus::Connected:
            return "\033[32m● Connected\033[0m";
        case BridgeStatus::Error:
            return "\033[31m✗ Error\033[0m";
        case BridgeStatus::Reconnecting:
            return "\033[33m↻ Reconnecting...\033[0m";
    }
    return "Unknown";
}

// Get the bridge uptime (time since last successful connection)
std::chrono::seconds get_bridge_uptime() {
    if (get_bridge_status() != BridgeStatus::Connected) {
        return std::chrono::seconds{0};
    }

    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
        now - detail::get_connected_since()
    );
}

// Get message statistics (sent, received, errors)
MessageStats get_message_stats() {
    std::lock_guard lock(detail::get_stats_mutex());
    return detail::get_stats_ref();
}

// Set the bridge status (used internally by the bridge connection manager)
void set_bridge_status(BridgeStatus status) {
    auto old_status = detail::get_status_ref().exchange(status);

    // Track connection time when transitioning to Connected
    if (status == BridgeStatus::Connected && old_status != BridgeStatus::Connected) {
        detail::get_connected_since() = std::chrono::steady_clock::now();
    }
}

// Increment message counters (thread-safe)
void record_message_sent() {
    std::lock_guard lock(detail::get_stats_mutex());
    ++detail::get_stats_ref().sent;
}

void record_message_received() {
    std::lock_guard lock(detail::get_stats_mutex());
    ++detail::get_stats_ref().received;
}

void record_message_error() {
    std::lock_guard lock(detail::get_stats_mutex());
    ++detail::get_stats_ref().errors;
}

} // namespace cc::bridge
