module;
#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>

export module cc.hooks.swarm_permission_poller;

export namespace cc::hooks {

// Permission response from the team leader
enum class PermissionResponseType {
    allow,      // Leader approved the tool use
    reject,     // Leader rejected the tool use
    timeout     // No response within the deadline
};

// A single permission update received from the leader
struct PermissionUpdate {
    std::string request_id;      // Matches the pending request ID
    PermissionResponseType type;
    std::optional<std::string> reason;   // Optional rejection reason
    std::chrono::system_clock::time_point received_at;
};

// A pending permission request awaiting leader response
struct PendingPermissionRequest {
    std::string request_id;
    std::string tool_name;
    std::string tool_input_json;
    std::chrono::system_clock::time_point created_at;
    std::chrono::milliseconds timeout{30000};  // Default 30s timeout
};

// Callbacks for permission responses
using PermissionAllowCallback = std::function<void(const PermissionUpdate& update)>;
using PermissionRejectCallback = std::function<void(const PermissionUpdate& update)>;

// Default poll interval
inline constexpr std::chrono::milliseconds default_poll_interval{500};

/// Swarm Permission Poller Hook
///
/// Polls for permission responses from the team leader when running
/// as a worker agent in a swarm. When a response is received, it calls
/// the appropriate callback (on_allow/on_reject) to continue execution.
///
/// Used in conjunction with the worker-side integration in tool permission
/// checking, which creates pending requests that this hook monitors.
class SwarmPermissionPoller {
public:
    SwarmPermissionPoller() = default;

    explicit SwarmPermissionPoller(
        std::chrono::milliseconds poll_interval,
        PermissionAllowCallback on_allow = nullptr,
        PermissionRejectCallback on_reject = nullptr
    )   : poll_interval_(poll_interval)
        , on_allow_(std::move(on_allow))
        , on_reject_(std::move(on_reject))
    {}

    // Set the allow callback
    auto set_on_allow(PermissionAllowCallback cb) -> void {
        on_allow_ = std::move(cb);
    }

    // Set the reject callback
    auto set_on_reject(PermissionRejectCallback cb) -> void {
        on_reject_ = std::move(cb);
    }

    // Add a pending request to monitor
    auto add_pending_request(PendingPermissionRequest request) -> void {
        pending_requests_.push_back(std::move(request));
    }

    // Remove a pending request (after it's been resolved)
    auto remove_pending_request(std::string_view request_id) -> bool {
        auto it = std::find_if(pending_requests_.begin(), pending_requests_.end(),
            [request_id](const auto& r) { return r.request_id == request_id; });
        if (it == pending_requests_.end()) return false;
        pending_requests_.erase(it);
        return true;
    }

    // Check if we're a swarm worker (determines if polling should be active)
    [[nodiscard]] auto is_swarm_worker() const -> bool {
        return is_worker_;
    }

    // Set swarm worker status
    auto set_is_swarm_worker(bool is_worker) -> void {
        is_worker_ = is_worker;
    }

    // Check if enough time has passed for the next poll
    [[nodiscard]] auto should_poll() const -> bool {
        if (!is_worker_ || pending_requests_.empty()) return false;
        auto elapsed = std::chrono::steady_clock::now() - last_poll_time_;
        return elapsed >= poll_interval_;
    }

    // Execute a poll cycle: check for responses and handle timeouts.
    // In production, this reads from IPC/disk; here we process any
    // queued responses and check timeouts.
    auto poll() -> void {
        last_poll_time_ = std::chrono::steady_clock::now();

        // Process any received responses
        process_responses();

        // Check for timed-out requests
        check_timeouts();
    }

    // Feed a response into the poller (simulates receiving from IPC/disk)
    auto feed_response(PermissionUpdate update) -> void {
        received_responses_.push_back(std::move(update));
    }

    // Validate and parse raw permission updates (filters malformed entries)
    [[nodiscard]] auto parse_permission_updates(
        const std::vector<PermissionUpdate>& raw
    ) -> std::vector<PermissionUpdate> {
        std::vector<PermissionUpdate> valid;
        for (const auto& entry : raw) {
            if (entry.request_id.empty()) continue;  // Skip malformed
            valid.push_back(entry);
        }
        return valid;
    }

    // Get pending request count
    [[nodiscard]] auto pending_count() const -> std::size_t {
        return pending_requests_.size();
    }

    // Get all pending requests
    [[nodiscard]] auto pending_requests() const
        -> const std::vector<PendingPermissionRequest>& {
        return pending_requests_;
    }

    // Get the agent name (for logging/display)
    auto set_agent_name(std::string name) -> void { agent_name_ = std::move(name); }
    [[nodiscard]] auto agent_name() const -> std::string_view { return agent_name_; }

    // Get the team name
    auto set_team_name(std::string name) -> void { team_name_ = std::move(name); }
    [[nodiscard]] auto team_name() const -> std::string_view { return team_name_; }

private:
    std::chrono::milliseconds poll_interval_{default_poll_interval};
    std::chrono::steady_clock::time_point last_poll_time_;
    bool is_worker_{false};
    std::string agent_name_;
    std::string team_name_;

    std::vector<PendingPermissionRequest> pending_requests_;
    std::vector<PermissionUpdate> received_responses_;

    PermissionAllowCallback on_allow_;
    PermissionRejectCallback on_reject_;

    // Process received responses against pending requests
    auto process_responses() -> void {
        for (auto resp_it = received_responses_.begin();
             resp_it != received_responses_.end(); ) {

            auto req_it = std::find_if(
                pending_requests_.begin(), pending_requests_.end(),
                [&](const auto& r) { return r.request_id == resp_it->request_id; });

            if (req_it != pending_requests_.end()) {
                // Matched - dispatch callback
                if (resp_it->type == PermissionResponseType::allow && on_allow_) {
                    on_allow_(*resp_it);
                } else if (resp_it->type == PermissionResponseType::reject && on_reject_) {
                    on_reject_(*resp_it);
                }
                pending_requests_.erase(req_it);
                resp_it = received_responses_.erase(resp_it);
            } else {
                ++resp_it;
            }
        }
    }

    // Check for timed-out pending requests
    auto check_timeouts() -> void {
        auto now = std::chrono::system_clock::now();
        for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->created_at);
            if (elapsed >= it->timeout) {
                PermissionUpdate timeout_update{
                    .request_id = it->request_id,
                    .type = PermissionResponseType::timeout,
                    .reason = "Permission request timed out",
                    .received_at = now
                };
                if (on_reject_) on_reject_(timeout_update);
                it = pending_requests_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

} // namespace cc::hooks
