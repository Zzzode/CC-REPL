// Ultra plan CCR session orchestration and keyword extraction.
// Migrated from src/utils/ultraplan/*.ts
module;

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.ultraplan;

export namespace cc::utils::ultraplan {

using namespace std::chrono;

// =========================================================================
// keyword.ts — Trigger keyword detection & replacement
// =========================================================================

/// A trigger position found in input text.
struct TriggerPosition {
    std::string word;
    std::size_t start;
    std::size_t end;
};

/// Find all triggerable "ultraplan" positions in text.
/// Skips occurrences inside delimiters, path contexts, or slash commands.
[[nodiscard]] std::vector<TriggerPosition>
find_ultraplan_trigger_positions(std::string_view text);

/// Find all triggerable "ultrareview" positions in text.
[[nodiscard]] std::vector<TriggerPosition>
find_ultrareview_trigger_positions(std::string_view text);

/// Check if text contains a triggerable "ultraplan" keyword.
[[nodiscard]] bool has_ultraplan_keyword(std::string_view text);

/// Check if text contains a triggerable "ultrareview" keyword.
[[nodiscard]] bool has_ultrareview_keyword(std::string_view text);

/// Replace the first triggerable "ultraplan" with "plan" so the forwarded
/// prompt stays grammatical ("please ultraplan this" -> "please plan this").
/// Preserves the user's casing of the "plan" suffix.
[[nodiscard]] std::string replace_ultraplan_keyword(std::string_view text);

// =========================================================================
// ccrSession.ts — CCR Session Polling for ExitPlanMode
// =========================================================================

/// Poll interval for remote session events.
inline constexpr auto kPollInterval = milliseconds{3000};

/// Maximum consecutive network failures before aborting.
inline constexpr int kMaxConsecutiveFailures = 5;

/// Sentinel string in browser PlanModal feedback for teleport-back.
inline constexpr std::string_view kUltraplanTeleportSentinel =
    "__ULTRAPLAN_TELEPORT_LOCAL__";

/// Reason a poll operation failed.
enum class PollFailReason {
    terminated,
    timeout_pending,
    timeout_no_plan,
    extract_marker_missing,
    network_or_unknown,
    stopped,
};

/// Error thrown during ultraplan poll operations.
struct UltraplanPollError {
    std::string message;
    PollFailReason reason;
    int reject_count;
};

/// Result of scanning the event stream for ExitPlanMode.
enum class ScanResultKind {
    approved,
    teleport,
    rejected,
    pending,
    terminated,
    unchanged,
};

struct ScanResult {
    ScanResultKind kind;
    std::string plan;          // populated for approved/teleport
    std::string id;            // populated for rejected
    std::string subtype;       // populated for terminated
};

/// Phase state derived from the event stream. Transitions:
///   running -> needs_input -> plan_ready -> (resolved or rejected -> running)
enum class UltraplanPhase {
    running,
    needs_input,
    plan_ready,
};

/// Represents a single SDK message event (simplified from Anthropic SDK types).
struct SDKMessage {
    std::string type;      // "assistant" | "user" | "result"
    std::string subtype;   // for result: "success" | "error_*"
    std::string content;   // serialized content
};

/// Pure stateful classifier for the CCR event stream.
/// Ingests SDKMessage batches and returns the current ExitPlanMode verdict.
/// No I/O, no timers — suitable for unit testing with synthetic events.
class ExitPlanModeScanner {
public:
    ExitPlanModeScanner() = default;

    /// Ingest a batch of new events and return the scan result.
    [[nodiscard]] ScanResult ingest(const std::vector<SDKMessage>& new_events);

    /// Number of rejected ExitPlanMode calls seen.
    [[nodiscard]] int reject_count() const noexcept { return reject_count_; }

    /// True when an ExitPlanMode tool_use exists with no tool_result yet.
    [[nodiscard]] bool has_pending_plan() const noexcept;

    /// True if a pending state was ever observed.
    [[nodiscard]] bool ever_seen_pending() const noexcept { return ever_seen_pending_; }

private:
    std::vector<std::string> exit_plan_calls_;
    std::vector<std::pair<std::string, std::string>> results_; // (tool_use_id, content)
    std::vector<std::string> rejected_ids_;
    std::optional<std::string> terminated_subtype_;
    bool rescan_after_rejection_{false};
    bool ever_seen_pending_{false};
    int reject_count_{0};
};

/// Where the user wants the plan executed.
enum class ExecutionTarget {
    local,   // user clicked teleport (execute here, archive remote)
    remote,  // user approved in-CCR execution
};

/// Result of a successful poll.
struct PollResult {
    std::string plan;
    int reject_count;
    ExecutionTarget execution_target;
};

/// Phase change callback.
using PhaseChangeCallback = std::function<void(UltraplanPhase)>;

/// Should-stop predicate for poll cancellation.
using ShouldStopFn = std::function<bool()>;

/// Poll a remote CCR session for an approved ExitPlanMode.
/// Returns the approved plan text and execution target.
/// Throws UltraplanPollError on timeout, termination, or network failure.
[[nodiscard]] std::expected<PollResult, UltraplanPollError>
poll_for_approved_exit_plan_mode(
    std::string_view session_id,
    milliseconds timeout);

/// Poll with phase change callback and stop predicate.
[[nodiscard]] std::expected<PollResult, UltraplanPollError>
poll_for_approved_exit_plan_mode(
    std::string_view session_id,
    milliseconds timeout,
    PhaseChangeCallback on_phase_change,
    ShouldStopFn should_stop);

} // namespace cc::utils::ultraplan
