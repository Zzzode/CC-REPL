/// @file collapse_background_bash.cppm
/// @brief Faithful port of TS `collapseBackgroundBashNotifications`
///        (src/utils/collapseBackgroundBashNotifications.ts).
///
/// Collapses consecutive *completed* background-bash task-notifications into a
/// single synthetic "N background commands completed" notification, so a burst
/// of finished background shells doesn't flood the transcript.  Failed/killed
/// tasks and agent/workflow notifications are left individually visible.
///
/// This is one of the four collapse passes chained in TS Messages.tsx:520
///   collapseBackgroundBashNotifications(collapseHookSummaries(
///     collapseTeammateShutdowns(collapseReadSearchGroups(grouped, tools))))
/// and is part of the confirmed P0 gap `msg-pipeline-missing` (audit round7).
///
/// TS REFERENCE (port verbatim): src/utils/collapseBackgroundBashNotifications.ts
///
/// TAG-FORMAT NOTE (TS vs CPP divergence — intentional):
///   TS constants/xml.ts uses HYPHENATED tag names ('task-notification',
///   'status', 'summary').  The CPP engine, however, emits UNDERSCORED tags
///   — see local_agent_task.cppm:435 / local_shell_task.cppm and
///   runtime_registry.cppm:766 which all write "<task_notification>",
///   "<status>", "<summary>".  To collapse the messages the CPP tree actually
///   produces, we match the CPP wire format (underscore).  The constants below
///   are the single source of truth for that decision.
// ────────────────────────────────────────────────────────────────────────
module;

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module cc.ui.messages.collapse_background_bash;

import cc.types.types;
import cc.ui.messages.message_pipeline;  // reuse faithful extract_tag()

export namespace cc::ui::messages::collapse {

namespace pipeline = cc::ui::messages::pipeline;

// TS REF: constants/xml.ts TASK_NOTIFICATION_TAG / STATUS_TAG / SUMMARY_TAG.
// CPP wire format is underscored (see file header TAG-FORMAT NOTE).
inline constexpr std::string_view kTaskNotificationTag = "task_notification";
inline constexpr std::string_view kStatusTag           = "status";
inline constexpr std::string_view kSummaryTag          = "summary";

// TS REF: src/tasks/LocalShellTask/LocalShellTask.tsx:23
//   `export const BACKGROUND_BASH_SUMMARY_PREFIX = 'Background command '`
// Mirrored locally (must equal cc::tasks::BACKGROUND_BASH_SUMMARY_PREFIX in
// tasks/local_shell_task.cppm:31) rather than imported, so the UI-messages
// layer stays free of the cc.tasks.* / bash-execution module graph.  If the
// task-layer constant ever changes, update this mirror in lock-step.
inline constexpr std::string_view kBackgroundBashSummaryPrefix = "Background command ";

/// Read the first text content block of a message, if any.
/// TS: `msg.message.content[0]` where `content[0]?.type === 'text'`.
[[nodiscard]] inline std::optional<std::string_view> first_text_block(
    const cc::core::Message& msg) noexcept {
    const auto* user = std::get_if<cc::core::UserMessage>(&msg);
    if (user == nullptr) return std::nullopt;          // TS: msg.type !== 'user'
    if (user->content.empty()) return std::nullopt;    // TS: content[0] undefined
    const auto* text = std::get_if<cc::core::TextBlock>(&user->content.front());
    if (text == nullptr) return std::nullopt;          // TS: content[0].type !== 'text'
    return std::string_view(text->text);
}

/// TS REF: isCompletedBackgroundBash(msg).  A user message whose first text
/// block is a task-notification with <status>completed</status> and a
/// <summary> beginning with BACKGROUND_BASH_SUMMARY_PREFIX (i.e. a bash-kind
/// LocalShellTask completion, not an agent/workflow/monitor notification).
[[nodiscard]] inline bool is_completed_background_bash(
    const cc::core::Message& msg) {
    const auto text = first_text_block(msg);
    if (!text.has_value()) return false;
    // TS: content.text.includes(`<${TASK_NOTIFICATION_TAG}`)  (no '>' — an
    // opening tag with attributes still matches).
    if (text->find(std::string("<") + std::string(kTaskNotificationTag)) ==
        std::string_view::npos) {
        return false;
    }
    // Only collapse successful completions — failed/killed stay visible.
    if (pipeline::extract_tag(*text, kStatusTag) != "completed") return false;
    // Distinguish bash-kind completions from agent/workflow/monitor ones.
    const auto summary = pipeline::extract_tag(*text, kSummaryTag);
    return summary.has_value() &&
           summary->starts_with(kBackgroundBashSummaryPrefix);
}

/// Build the synthetic merged notification text for `count` collapsed bashes.
/// TS REF: the template literal at collapseBackgroundBashNotifications.ts:71.
[[nodiscard]] inline std::string make_collapsed_notification_text(int count) {
    const std::string open_notif  = "<" + std::string(kTaskNotificationTag) + ">";
    const std::string close_notif = "</" + std::string(kTaskNotificationTag) + ">";
    const std::string status  = "<" + std::string(kStatusTag) + ">completed</" +
                                std::string(kStatusTag) + ">";
    const std::string summary = "<" + std::string(kSummaryTag) + ">" +
                                std::to_string(count) +
                                " background commands completed</" +
                                std::string(kSummaryTag) + ">";
    return open_notif + status + summary + close_notif;
}

/// Faithful port of TS collapseBackgroundBashNotifications(messages, verbose).
///
/// `fullscreen` mirrors TS `isFullscreenEnvEnabled()` — the collapse only runs
/// in the fullscreen transcript (the classic scrollback shows each completion).
/// `verbose` is the ctrl+O pass-through: when true, every completion is shown.
[[nodiscard]] inline std::vector<cc::core::Message>
collapse_background_bash_notifications(
    const std::vector<cc::core::Message>& messages,
    bool fullscreen,
    bool verbose) {
    // TS: `if (!isFullscreenEnvEnabled()) return messages;`
    if (!fullscreen) return messages;
    // TS: `if (verbose) return messages;`
    if (verbose) return messages;

    std::vector<cc::core::Message> result;
    result.reserve(messages.size());

    std::size_t i = 0;
    while (i < messages.size()) {
        if (is_completed_background_bash(messages[i])) {
            const std::size_t run_start = i;
            int count = 0;
            while (i < messages.size() &&
                   is_completed_background_bash(messages[i])) {
                ++count;
                ++i;
            }
            if (count == 1) {
                // Single completion — keep it as-is.
                result.push_back(messages[run_start]);
            } else {
                // Synthesize a task-notification that the existing
                // UserAgentNotificationMessage renderer already understands —
                // no new renderer needed (TS parity).  Preserve the first
                // message's id/timestamp; replace only its text content.
                auto synthetic = std::get<cc::core::UserMessage>(messages[run_start]);
                synthetic.content.clear();
                synthetic.content.push_back(
                    cc::core::TextBlock{make_collapsed_notification_text(count)});
                result.emplace_back(std::move(synthetic));
            }
        } else {
            result.push_back(messages[i]);
            ++i;
        }
    }

    return result;
}

} // namespace cc::ui::messages::collapse
