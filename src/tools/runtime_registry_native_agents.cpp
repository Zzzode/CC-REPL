// Implementation unit for cc.tools.runtime_registry — native-agent record
// formatters, transcript-artifact cleanup (whose function pointers are taken
// by inline wrappers that stay in the interface), and the team-member Agent
// start-input builder.
module;

module cc.tools.runtime_registry;

import std;

import cc.tools.agent_runtime;
import cc.tools.team;
import cc.tools.runtime_team_shared;
import cc.utils.json;

namespace cc::tools::detail {

namespace fs = std::filesystem;

namespace json = cc::utils::json;

namespace {

[[nodiscard]] bool path_is_agent_runtime_artifact(const fs::path& path) {
    if (path.empty()) return false;
    const auto root = normalized_absolute_path(agent_runtime::runtime_state_dir());
    const auto candidate = normalized_absolute_path(path);
    return path_has_prefix(candidate, root);
}

void add_unique_artifact_path(std::vector<fs::path>& paths, fs::path path) {
    if (path.empty()) return;
    path = normalized_absolute_path(path);
    if (std::ranges::find(paths, path) == paths.end()) {
        paths.push_back(std::move(path));
    }
}

} // namespace

[[nodiscard]] bool is_native_agent_task(const agent_runtime::NativeAgentRecord& record) {
    return record.background;
}

[[nodiscard]] std::string native_agent_display_name(const agent_runtime::NativeAgentRecord& record) {
    if (record.name && !record.name->empty()) return *record.name;
    return record.agent_id;
}

[[nodiscard]] std::string native_agent_output_file(const agent_runtime::NativeAgentRecord& record) {
    if (record.output_file_path && !record.output_file_path->empty()) return *record.output_file_path;
    if (record.transcript_path && !record.transcript_path->empty()) return *record.transcript_path;
    return agent_runtime::agent_output_file_path(record.agent_id).string();
}

[[nodiscard]] bool native_agent_status_is_terminal(agent_runtime::NativeAgentStatus status) {
    return status == agent_runtime::NativeAgentStatus::Completed ||
        status == agent_runtime::NativeAgentStatus::Failed ||
        status == agent_runtime::NativeAgentStatus::Cancelled;
}

[[nodiscard]] std::size_t cleanup_native_agent_transcript_artifacts(
    const agent_runtime::NativeAgentRecord& record
) {
    std::vector<fs::path> paths;
    if (record.output_file_path) add_unique_artifact_path(paths, fs::path{*record.output_file_path});
    if (record.transcript_path) add_unique_artifact_path(paths, fs::path{*record.transcript_path});
    if (record.sidechain_jsonl_path) add_unique_artifact_path(paths, fs::path{*record.sidechain_jsonl_path});
    add_unique_artifact_path(paths, agent_runtime::agent_output_file_path(record.agent_id));
    add_unique_artifact_path(paths, agent_runtime::agent_transcript_path(record.agent_id));
    add_unique_artifact_path(paths, agent_runtime::agent_sidechain_jsonl_path(record.agent_id));

    std::size_t removed_count = 0;
    for (const auto& path : paths) {
        if (!path_is_agent_runtime_artifact(path)) continue;
        std::error_code ec;
        if (!fs::exists(path, ec) && !fs::is_symlink(path, ec)) continue;
        ec.clear();
        const auto removed = fs::remove_all(path, ec);
        if (!ec && removed > 0) ++removed_count;
    }
    return removed_count;
}

[[nodiscard]] std::string format_native_agent_task_summary(const agent_runtime::NativeAgentRecord& record) {
    std::string out = std::format(
        "{} [{}] Agent {}: {}",
        record.agent_id,
        agent_runtime::native_agent_status_name(record.status),
        record.agent_type.empty() ? "general-purpose" : record.agent_type,
        cc::tools::detail::native_agent_display_name(record));
    out += "\noutput_file: " + cc::tools::detail::native_agent_output_file(record);
    if (record.team_name && !record.team_name->empty()) out += "\nteam: " + *record.team_name;
    if (record.cwd && !record.cwd->empty()) out += "\ncwd: " + *record.cwd;
    if (record.worktree_path && !record.worktree_path->empty()) out += "\nworktree_path: " + *record.worktree_path;
    if (record.worktree_branch && !record.worktree_branch->empty()) out += "\nworktree_branch: " + *record.worktree_branch;
    if (record.teammate_backend && !record.teammate_backend->empty()) out += "\nteammate_backend: " + *record.teammate_backend;
    if (record.teammate_task_id && !record.teammate_task_id->empty()) out += "\nteammate_task_id: " + *record.teammate_task_id;
    if (record.teammate_pane_id && !record.teammate_pane_id->empty()) out += "\nteammate_pane_id: " + *record.teammate_pane_id;
    if (record.teammate_color && !record.teammate_color->empty()) out += "\nteammate_color: " + *record.teammate_color;
    if (record.progress) out += std::format("\nprogress: {:.0f}%", *record.progress * 100.0);
    if (record.output && !record.output->empty()) out += "\nresult: " + *record.output;
    if (record.error && !record.error->empty()) out += "\nerror: " + *record.error;
    return out;
}

[[nodiscard]] std::optional<std::string> native_agent_notification_status(
    agent_runtime::NativeAgentStatus status) {
    switch (status) {
        case agent_runtime::NativeAgentStatus::Completed: return "completed";
        case agent_runtime::NativeAgentStatus::Failed: return "failed";
        case agent_runtime::NativeAgentStatus::Cancelled: return "stopped";
        case agent_runtime::NativeAgentStatus::Queued:
        case agent_runtime::NativeAgentStatus::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> format_native_agent_task_notification(
    const agent_runtime::NativeAgentRecord& record) {
    auto status = native_agent_notification_status(record.status);
    if (!status) return std::nullopt;

    const auto description = cc::tools::detail::native_agent_display_name(record);
    std::string summary;
    if (*status == "completed") {
        summary = std::format("Agent \"{}\" completed", description);
    } else if (*status == "failed") {
        summary = std::format("Agent \"{}\" failed: {}", description, record.error.value_or("Unknown error"));
    } else {
        summary = std::format("Agent \"{}\" was stopped", description);
    }

    std::string result_section;
    if (record.output && !record.output->empty()) {
        result_section = std::format("\n<result>{}</result>", escape_xml_text(*record.output));
    } else if (record.error && !record.error->empty()) {
        result_section = std::format("\n<result>{}</result>", escape_xml_text(*record.error));
    }
    std::string worktree_section;
    if (record.worktree_path && !record.worktree_path->empty()) {
        worktree_section += std::format("\n<worktree_path>{}</worktree_path>", escape_xml_text(*record.worktree_path));
    }
    if (record.worktree_branch && !record.worktree_branch->empty()) {
        worktree_section += std::format("\n<worktree_branch>{}</worktree_branch>", escape_xml_text(*record.worktree_branch));
    }
    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}{}\n"
        "</task_notification>",
        escape_xml_text(record.agent_id),
        escape_xml_text(cc::tools::detail::native_agent_output_file(record)),
        escape_xml_text(*status),
        escape_xml_text(summary),
        result_section,
        worktree_section);
}

[[nodiscard]] std::string format_native_agent_task_output(const agent_runtime::NativeAgentRecord& record) {
    std::string out = format_native_agent_task_summary(record);
    if (record.output && !record.output->empty()) {
        out += "\n\nOutput:\n" + *record.output;
    } else if (record.error && !record.error->empty()) {
        out += "\n\nError:\n" + *record.error;
    }

    if (!record.transcript.empty()) {
        out += "\n\nTranscript:\n";
        for (const auto& line : record.transcript) {
            out += line + "\n";
        }
    }

    if (auto notification = cc::tools::detail::format_native_agent_task_notification(record)) {
        out += "\n" + *notification;
    }
    return out;
}

// build_team_member_agent_start_input_json stays here: it depends on
// runtime_registry's ToolInput/ToolRegistry semantics and participates in the
// team_create dispatcher branch (S9), so it is not part of the S7 extraction.

[[nodiscard]] std::string build_team_member_agent_start_input_json(
    const TeamMember& member,
    std::string_view team_name,
    std::string_view prompt,
    const runtime_team_shared::TeamMemberStartOptions* options,
    const std::optional<std::string>& default_cwd,
    const std::optional<std::string>& default_mode,
    const std::optional<std::string>& default_isolation
) {
    json::JsonBuilder b;
    b.str("agent_id", member.agent_id);
    b.str("subagent_type",
         options && options->agent_type && !options->agent_type->empty()
             ? std::string_view{*options->agent_type}
             : std::string_view{"general-purpose"});
    b.str("prompt", prompt);
    b.boolean("run_in_background", true);
    b.str("team_name", team_name);
    b.str("description", std::format("Team member {} for {}", member.agent_id, team_name));
    if (options && options->mode && !options->mode->empty()) {
        b.str("mode", *options->mode);
    } else {
        b.opt_str("mode", default_mode);
    }
    if (options && options->cwd && !options->cwd->empty()) {
        b.str("cwd", *options->cwd);
    } else {
        b.opt_str("cwd", default_cwd);
    }
    if (options && options->isolation && !options->isolation->empty()) {
        b.str("isolation", *options->isolation);
    } else {
        b.opt_str("isolation", default_isolation);
    }
    return b.serialize();
}

} // namespace cc::tools::detail
