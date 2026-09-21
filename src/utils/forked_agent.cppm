// Forked and in-process agent management
// Sources: forkedAgent.ts, standaloneAgent.ts, inProcessTeammateHelpers.ts
module;

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.forked_agent;

import cc.utils.async;
import cc.utils.json;

export namespace cc::utils::agent {

using cc::utils::json::JsonVal;

// =========================================================================
// ForkedAgentOptions - Configuration for spawning a child agent process
// =========================================================================
struct ForkedAgentOptions {
    std::string agent_name;
    std::string script_path;
    std::vector<std::string> args;
    std::optional<std::string> working_directory;
    std::optional<uint32_t> timeout_ms;
};

/// Overload: create ForkedAgentOptions with name and script only
[[nodiscard]] inline ForkedAgentOptions make_forked_agent_options(
    std::string agent_name,
    std::string script_path
) {
    return ForkedAgentOptions{
        .agent_name = std::move(agent_name),
        .script_path = std::move(script_path),
        .args = {},
        .working_directory = std::nullopt,
        .timeout_ms = std::nullopt,
    };
}

// =========================================================================
// MessageHandler - Callback type for receiving messages from agents
// =========================================================================
using MessageHandler = std::function<void(const JsonVal&)>;

// =========================================================================
// ForkedAgent - Manages a child process agent with IPC messaging
// =========================================================================
class ForkedAgent {
public:
    explicit ForkedAgent(ForkedAgentOptions options);
    ~ForkedAgent();

    ForkedAgent(const ForkedAgent&) = delete;
    ForkedAgent& operator=(const ForkedAgent&) = delete;
    ForkedAgent(ForkedAgent&&) noexcept;
    ForkedAgent& operator=(ForkedAgent&&) noexcept;

    /// Start the child process
    [[nodiscard]] async::Task<std::expected<void, std::string>> start();

    /// Stop the child process gracefully (with optional force kill timeout)
    [[nodiscard]] async::Task<std::expected<void, std::string>> stop();

    /// Send a JSON message to the child process
    [[nodiscard]] std::expected<void, std::string> send(const JsonVal& message);

    /// Register a handler for incoming messages
    void on_message(MessageHandler handler);

    /// Check if the agent process is currently running
    [[nodiscard]] bool is_running() const noexcept;

    /// Get the agent name
    [[nodiscard]] std::string_view name() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// =========================================================================
// StandaloneAgent - Agent context for non-swarm sessions
// =========================================================================
struct StandaloneAgentContext {
    std::string name;
    std::optional<std::string> color;
};

/// Get standalone agent name if set and not in a swarm team.
/// Returns std::nullopt if in a team (swarm takes precedence).
[[nodiscard]] std::optional<std::string_view> get_standalone_agent_name(
    const StandaloneAgentContext* context
);

// =========================================================================
// In-Process Teammate Helpers
// =========================================================================

/// Find the task ID for an in-process teammate by agent name
[[nodiscard]] std::optional<std::string> find_in_process_teammate_task_id(
    std::string_view agent_name,
    const JsonVal& app_state
);

/// Set awaitingPlanApproval state for an in-process teammate
void set_awaiting_plan_approval(
    std::string_view task_id,
    std::function<void(JsonVal&)> set_app_state,
    bool awaiting
);

/// Handle plan approval response for an in-process teammate
/// Resets awaitingPlanApproval to false.
void handle_plan_approval_response(
    std::string_view task_id,
    const JsonVal& response,
    std::function<void(JsonVal&)> set_app_state
);

/// Check if a message is a permission-related response
/// (tool permissions or sandbox/network host permissions)
[[nodiscard]] bool is_permission_related_response(std::string_view message_text);

} // namespace cc::utils::agent
