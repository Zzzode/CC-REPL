module;

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.collapse_notifications;

export namespace cc::utils::collapse_notifications {

struct HookSummaryMessage {
    std::optional<std::string> hook_label;
    int hook_count = 0;
    std::vector<std::string> hook_infos;
    std::vector<std::string> hook_errors;
    bool prevented_continuation = false;
    bool has_output = false;
    std::optional<int> total_duration_ms;
};

[[nodiscard]] inline bool is_labeled_hook_summary(const HookSummaryMessage& message) noexcept {
    return message.hook_label.has_value();
}

[[nodiscard]] inline std::vector<HookSummaryMessage> collapse_hook_summaries(const std::vector<HookSummaryMessage>& messages) {
    std::vector<HookSummaryMessage> result;
    std::size_t i = 0;
    while (i < messages.size()) {
        const auto& message = messages[i];
        if (!is_labeled_hook_summary(message)) {
            result.push_back(message);
            ++i;
            continue;
        }

        const auto label = *message.hook_label;
        std::vector<HookSummaryMessage> group;
        while (i < messages.size() && is_labeled_hook_summary(messages[i]) && *messages[i].hook_label == label) {
            group.push_back(messages[i]);
            ++i;
        }
        if (group.size() == 1) {
            result.push_back(group.front());
            continue;
        }

        HookSummaryMessage collapsed = group.front();
        collapsed.hook_count = 0;
        collapsed.hook_infos.clear();
        collapsed.hook_errors.clear();
        collapsed.prevented_continuation = false;
        collapsed.has_output = false;
        int max_duration = 0;
        bool has_duration = false;
        for (const auto& item : group) {
            collapsed.hook_count += item.hook_count;
            collapsed.hook_infos.insert(collapsed.hook_infos.end(), item.hook_infos.begin(), item.hook_infos.end());
            collapsed.hook_errors.insert(collapsed.hook_errors.end(), item.hook_errors.begin(), item.hook_errors.end());
            collapsed.prevented_continuation = collapsed.prevented_continuation || item.prevented_continuation;
            collapsed.has_output = collapsed.has_output || item.has_output;
            if (item.total_duration_ms.has_value()) {
                has_duration = true;
                max_duration = std::max(max_duration, *item.total_duration_ms);
            }
        }
        collapsed.total_duration_ms = has_duration ? std::optional<int>{max_duration} : std::nullopt;
        result.push_back(std::move(collapsed));
    }
    return result;
}

struct TeammateShutdownMessage {
    std::string uuid;
    long long timestamp_ms = 0;
    bool is_shutdown = false;
    bool is_batch = false;
    int count = 1;
};

[[nodiscard]] inline std::vector<TeammateShutdownMessage> collapse_teammate_shutdowns(const std::vector<TeammateShutdownMessage>& messages) {
    std::vector<TeammateShutdownMessage> result;
    std::size_t i = 0;
    while (i < messages.size()) {
        const auto& message = messages[i];
        if (!message.is_shutdown) {
            result.push_back(message);
            ++i;
            continue;
        }
        int count = 0;
        const auto first = message;
        while (i < messages.size() && messages[i].is_shutdown) {
            ++count;
            ++i;
        }
        if (count == 1) {
            result.push_back(first);
        } else {
            result.push_back(TeammateShutdownMessage{.uuid = first.uuid, .timestamp_ms = first.timestamp_ms, .is_shutdown = false, .is_batch = true, .count = count});
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> extract_xml_tag(std::string_view text, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const auto start = text.find(open);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + open.size();
    const auto end = text.find(close, value_start);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(text.substr(value_start, end - value_start));
}

[[nodiscard]] inline bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] inline bool is_completed_background_bash(std::string_view text) {
    constexpr std::string_view task_notification_tag = "task-notification";
    constexpr std::string_view status_tag = "status";
    constexpr std::string_view summary_tag = "summary";
    constexpr std::string_view background_bash_summary_prefix = "Background command ";

    if (text.find("<" + std::string(task_notification_tag)) == std::string_view::npos) return false;
    auto status = extract_xml_tag(text, status_tag);
    if (!status.has_value() || *status != "completed") return false;
    auto summary = extract_xml_tag(text, summary_tag);
    return summary.has_value() && starts_with(*summary, background_bash_summary_prefix);
}

[[nodiscard]] inline std::vector<std::string> collapse_background_bash_notifications(
    const std::vector<std::string>& messages,
    bool fullscreen_enabled,
    bool verbose
) {
    if (!fullscreen_enabled || verbose) return messages;
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < messages.size()) {
        const auto& message = messages[i];
        if (!is_completed_background_bash(message)) {
            result.push_back(message);
            ++i;
            continue;
        }
        int count = 0;
        const auto first = message;
        while (i < messages.size() && is_completed_background_bash(messages[i])) {
            ++count;
            ++i;
        }
        if (count == 1) {
            result.push_back(first);
        } else {
            result.push_back("<task-notification><status>completed</status><summary>" + std::to_string(count) + " background commands completed</summary></task-notification>");
        }
    }
    return result;
}

} // namespace cc::utils::collapse_notifications
