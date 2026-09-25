// Implementation unit for cc.tools.agent_runtime — sidechain/transcript
// persistence: task-notification rendering, JSONL entry construction and
// fork rebasing, tool-use state collection, record persistence, transcript
// readers (TS-jsonl and sidechain-jsonl), and NativeAgentRecord loading.
module;

module cc.tools.agent_runtime;

import std;

import cc.utils.json;

namespace cc::tools::agent_runtime {

namespace {

inline constexpr std::string_view kForkSubagentType = "fork";

} // namespace

[[nodiscard]] std::optional<std::string> format_native_agent_task_notification(
    const NativeAgentRecord& record
) {
    auto status = native_agent_terminal_notification_status(record.status);
    if (!status) return std::nullopt;

    const auto description = native_agent_display_name(record);
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
        result_section = std::format("\n<result>{}</result>", xml_escape(*record.output));
    } else if (record.error && !record.error->empty()) {
        result_section = std::format("\n<result>{}</result>", xml_escape(*record.error));
    }
    std::string worktree_section;
    if (record.worktree_path && !record.worktree_path->empty()) {
        worktree_section += std::format("\n<worktree_path>{}</worktree_path>", xml_escape(*record.worktree_path));
    }
    if (record.worktree_branch && !record.worktree_branch->empty()) {
        worktree_section += std::format("\n<worktree_branch>{}</worktree_branch>", xml_escape(*record.worktree_branch));
    }
    return std::format(
        "<task_notification>\n"
        "<task_id>{}</task_id>\n"
        "<output_file>{}</output_file>\n"
        "<status>{}</status>\n"
        "<summary>{}</summary>{}{}\n"
        "</task_notification>",
        xml_escape(record.agent_id),
        xml_escape(native_agent_output_file(record)),
        xml_escape(*status),
        xml_escape(summary),
        result_section,
        worktree_section);
}
void write_json_string_array(std::ostream& out, std::string_view name, const std::vector<std::string>& values) {
    out << R"(,")" << name << R"(":[)";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ',';
        out << '"' << json_escape(values[i]) << '"';
    }
    out << ']';
}
void write_json_optional_string(
    std::ostream& out,
    std::string_view name,
    const std::optional<std::string>& value
) {
    if (value) out << R"(,")" << name << R"(":")" << json_escape(*value) << '"';
}
void refresh_native_agent_output_symlink(
    const fs::path& output_path,
    const fs::path& transcript_path
) {
    if (output_path.empty() || output_path == transcript_path) return;

    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) return;

    fs::remove(output_path, ec);
    ec.clear();
    fs::create_symlink(transcript_path, output_path, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(transcript_path, output_path, fs::copy_options::overwrite_existing, ec);
    }
}
[[nodiscard]] std::string fallback_sidechain_content_json(std::string_view text) {
    return std::format(R"([{{"type":"text","text":"{}"}}])", json_escape(text));
}
[[nodiscard]] std::string normalize_sidechain_content_json(
    std::string_view content_json,
    std::string_view fallback_text
) {
    auto content = trim(content_json);
    if (content.empty()) return fallback_sidechain_content_json(fallback_text);
    auto parsed = cc::utils::json::parse(content);
    if (!parsed) return fallback_sidechain_content_json(fallback_text);
    auto root = parsed->root();
    if (!root.valid() || (!root.is_arr() && !root.is_obj() && !root.is_str())) {
        return fallback_sidechain_content_json(fallback_text);
    }
    return std::string(content);
}
[[nodiscard]] std::string make_sidechain_jsonl_entry(
    std::string_view agent_id,
    std::size_t index,
    std::string_view role,
    std::string_view content_json,
    std::string_view fallback_text
) {
    const auto message_role = sidechain_message_role(role);
    const auto uuid = std::format("{}-{}", agent_id, index);
    const auto parent_uuid = index == 0 ? std::string{} : std::format("{}-{}", agent_id, index - 1);
    const auto normalized_content = normalize_sidechain_content_json(content_json, fallback_text);

    std::string entry;
    entry.reserve(normalized_content.size() + agent_id.size() * 3 + message_role.size() * 2 + 128);
    entry += R"({"type":")";
    entry += json_escape(message_role);
    entry += R"(","uuid":")";
    entry += json_escape(uuid);
    entry += R"(","parentUuid":)";
    if (index == 0) {
        entry += "null";
    } else {
        entry += '"';
        entry += json_escape(parent_uuid);
        entry += '"';
    }
    entry += R"(,"isSidechain":true,"agentId":")";
    entry += json_escape(agent_id);
    entry += R"(","message":{"role":")";
    entry += json_escape(message_role);
    entry += R"(","content":)";
    entry += normalized_content;
    entry += "}}";
    return entry;
}
[[nodiscard]] std::optional<std::string> rebase_sidechain_jsonl_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id,
    std::size_t index
) {
    auto parsed = cc::utils::json::parse(entry);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;

    auto root = parsed->root();
    auto message = root.get("message");
    std::string role = json_string_field(root, {"type"});
    cc::utils::json::JsonVal content = root.get("content");

    if (message.is_obj()) {
        auto message_role = json_string_field(message, {"role"});
        if (!message_role.empty()) role = std::move(message_role);
        content = message.get("content");
    }
    if (role.empty()) role = "user";

    auto content_json = content.valid() ? content.to_string() : std::string{};
    if (content_json.empty()) {
        auto text = json_string_field(root, {"raw", "content", "text"});
        content_json = fallback_sidechain_content_json(text);
    }

    return make_sidechain_jsonl_entry(
        agent_id,
        index,
        role,
        content_json,
        {});
}
[[nodiscard]] std::optional<std::string> rebase_content_replacement_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id
) {
    auto parsed = cc::utils::json::parse(entry);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    if (json_string_field(root, {"type"}) != "content-replacement") return std::nullopt;

    auto replacements = root.get("replacements");
    if (!replacements.is_arr()) return std::nullopt;

    std::string rebased;
    rebased.reserve(entry.size() + agent_id.size() + 64);
    rebased += R"({"type":"content-replacement")";
    auto session_id = json_string_field(root, {"sessionId", "session_id"});
    if (!session_id.empty()) {
        rebased += R"(,"sessionId":")";
        rebased += json_escape(session_id);
        rebased += '"';
    }
    rebased += R"(,"agentId":")";
    rebased += json_escape(agent_id);
    rebased += R"(","replacements":)";
    rebased += replacements.to_string();
    rebased += '}';
    return rebased;
}
[[nodiscard]] std::vector<std::string> fork_sidechain_entries_for_child(
    const NativeAgentRecord& parent,
    std::string_view child_agent_id
) {
    std::vector<std::string> entries;
    entries.reserve(parent.sidechain_entries.empty()
        ? parent.transcript.size()
        : parent.sidechain_entries.size());

    if (!parent.sidechain_entries.empty()) {
        std::size_t message_index = 0;
        for (const auto& entry : parent.sidechain_entries) {
            if (entry.empty()) continue;
            if (auto rebased_replacement = rebase_content_replacement_entry_for_agent(entry, child_agent_id)) {
                entries.push_back(std::move(*rebased_replacement));
                continue;
            }
            if (auto rebased = rebase_sidechain_jsonl_entry_for_agent(
                    entry,
                    child_agent_id,
                    message_index)) {
                entries.push_back(std::move(*rebased));
                ++message_index;
            }
        }
        if (!entries.empty()) return entries;
    }

    std::size_t message_index = 0;
    for (const auto& transcript_entry : parent.transcript) {
        const auto [role, content] = split_transcript_role(transcript_entry);
        entries.push_back(make_sidechain_jsonl_entry(
            child_agent_id,
            message_index++,
            role,
            {},
            content));
    }
    return entries;
}
void collect_sidechain_tool_use_state(
    cc::utils::json::JsonVal content,
    std::vector<std::string>& tool_use_ids,
    std::unordered_set<std::string>& seen_tool_use_ids,
    std::unordered_set<std::string>& tool_result_ids
) {
    if (!content.valid()) return;

    if (content.is_arr()) {
        content.iter([&](cc::utils::json::JsonVal block) {
            collect_sidechain_tool_use_state(block, tool_use_ids, seen_tool_use_ids, tool_result_ids);
        });
        return;
    }

    if (!content.is_obj()) return;
    const auto type = json_string_field(content, {"type"});
    if (type == "tool_use") {
        auto id = json_string_field(content, {"id"});
        if (!id.empty() && !seen_tool_use_ids.contains(id)) {
            seen_tool_use_ids.insert(id);
            tool_use_ids.push_back(std::move(id));
        }
        return;
    }
    if (type == "tool_result") {
        auto id = json_string_field(content, {"tool_use_id"});
        if (!id.empty()) tool_result_ids.insert(std::move(id));
        return;
    }

    collect_sidechain_tool_use_state(content.get("content"), tool_use_ids, seen_tool_use_ids, tool_result_ids);
}
[[nodiscard]] std::vector<std::string> unresolved_tool_use_ids_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    std::vector<std::string> tool_use_ids;
    std::unordered_set<std::string> seen_tool_use_ids;
    std::unordered_set<std::string> tool_result_ids;

    for (const auto& entry : entries) {
        auto parsed = cc::utils::json::parse(entry);
        if (!parsed || !parsed->root().is_obj()) continue;
        auto root = parsed->root();
        auto message = root.get("message");
        auto content = message.is_obj() ? message.get("content") : root.get("content");
        collect_sidechain_tool_use_state(content, tool_use_ids, seen_tool_use_ids, tool_result_ids);
    }

    std::vector<std::string> unresolved;
    for (const auto& id : tool_use_ids) {
        if (!tool_result_ids.contains(id)) unresolved.push_back(id);
    }
    return unresolved;
}
[[nodiscard]] std::string fork_missing_tool_results_content_json(
    const std::vector<std::string>& tool_use_ids,
    std::string_view directive_message
) {
    if (tool_use_ids.empty() && directive_message.empty()) return {};

    std::string content = "[";
    bool first = true;
    for (const auto& id : tool_use_ids) {
        if (!first) content += ',';
        first = false;
        content += R"({"type":"tool_result","tool_use_id":")";
        content += json_escape(id);
        content += R"(","content":[{"type":"text","text":"Fork started \u2014 processing in background"}]})";
    }
    if (!directive_message.empty()) {
        if (!first) content += ',';
        content += R"({"type":"text","text":")";
        content += json_escape(directive_message);
        content += R"("})";
    }
    content += ']';
    return content;
}
[[nodiscard]] bool native_agent_record_is_fork_child(const NativeAgentRecord& record) {
    if (record.agent_type == std::string_view(kForkSubagentType)) return true;

    for (const auto& capability : record.capabilities) {
        if (capability == "fork" || capability == "fork-subagent" || capability == "fork_child") {
            return true;
        }
    }

    const auto has_fork_marker = [](std::string_view text) {
        return text.find("<fork-boilerplate>") != std::string_view::npos ||
            text.find("system: forked from ") != std::string_view::npos;
    };
    for (const auto& line : record.transcript) {
        if (has_fork_marker(line)) return true;
    }
    for (const auto& entry : record.sidechain_entries) {
        if (has_fork_marker(entry)) return true;
    }
    return false;
}
[[nodiscard]] std::vector<std::string> fork_child_capabilities(
    const NativeAgentRecord& parent,
    const AgentRuntimeConfig& config
) {
    auto capabilities = config.capabilities.empty() ? parent.capabilities : config.capabilities;
    if (!std::ranges::contains(capabilities, "fork-subagent")) {
        capabilities.push_back("fork-subagent");
    }
    return capabilities;
}
bool write_sidechain_jsonl(
    const NativeAgentRecord& record,
    const fs::path& sidechain_path
) {
    std::error_code ec;
    fs::create_directories(sidechain_path.parent_path(), ec);
    if (ec) return false;

    std::ofstream sidechain(sidechain_path, std::ios::trunc);
    if (!sidechain) return false;
    if (!record.sidechain_entries.empty()) {
        for (const auto& entry : record.sidechain_entries) {
            if (entry.empty()) continue;
            sidechain << entry;
            if (!entry.ends_with('\n')) sidechain << '\n';
        }
        return sidechain.good();
    }
    for (std::size_t i = 0; i < record.transcript.size(); ++i) {
        const auto& entry = record.transcript[i];
        const auto [role, content] = split_transcript_role(entry);
        const auto message_type =
            (role == "user" || role == "assistant" || role == "system") ? role : std::string_view{"system"};
        const auto uuid = std::format("{}-{}", record.agent_id, i);
        const auto parent_uuid = i == 0 ? std::string{} : std::format("{}-{}", record.agent_id, i - 1);
        sidechain
            << R"({"type":")" << json_escape(message_type)
            << R"(","uuid":")" << json_escape(uuid)
            << R"(","parentUuid":)";
        if (i == 0) {
            sidechain << "null";
        } else {
            sidechain << '"' << json_escape(parent_uuid) << '"';
        }
        sidechain
            << R"(,"isSidechain":true)"
            << R"(,"agentId":")" << json_escape(record.agent_id)
            << R"(","message":{"role":")" << json_escape(message_type)
            << R"(","content":[{"type":"text","text":")" << json_escape(content)
            << R"("}]})"
            << R"(,"agent_id":")" << json_escape(record.agent_id)
            << R"(","parent_uuid":)";
        if (i == 0) {
            sidechain << "null";
        } else {
            sidechain << '"' << json_escape(parent_uuid) << '"';
        }
        sidechain
            << R"(,"role":")" << json_escape(message_type)
            << R"(","content":")" << json_escape(content)
            << R"(","raw":")" << json_escape(entry)
            << "\"}\n";
    }
    return sidechain.good();
}
bool persist_native_agent_record(const NativeAgentRecord& record) {
    std::error_code ec;
    const auto dir = runtime_state_dir();
    fs::create_directories(dir, ec);
    if (ec) return false;

    auto transcript_path = record.transcript_path
        ? fs::path{*record.transcript_path}
        : agent_transcript_path(record.agent_id);
    if (transcript_path.is_relative()) transcript_path = dir / transcript_path;
    fs::create_directories(transcript_path.parent_path(), ec);

    {
        std::ofstream transcript(transcript_path, std::ios::trunc);
        if (!transcript) return false;
        for (const auto& line : record.transcript) transcript << line << '\n';
    }

    auto sidechain_path = record.sidechain_jsonl_path
        ? fs::path{*record.sidechain_jsonl_path}
        : agent_sidechain_jsonl_path(record.agent_id);
    if (sidechain_path.is_relative()) sidechain_path = dir / sidechain_path;
    (void)write_sidechain_jsonl(record, sidechain_path);

    auto output_path = record.output_file_path
        ? fs::path{*record.output_file_path}
        : agent_output_file_path(record.agent_id);
    if (output_path.is_relative()) output_path = dir / output_path;
    refresh_native_agent_output_symlink(output_path, transcript_path);

    std::ofstream out(agent_record_path(record.agent_id), std::ios::trunc);
    if (!out) return false;
    out << R"({"agent_id":")" << json_escape(record.agent_id)
        << R"(","agent_type":")" << json_escape(record.agent_type)
        << R"(","background":)" << (record.background ? "true" : "false")
        << R"(,"status":")" << native_agent_status_name(record.status) << '"'
        << R"(,"cancel_requested":)" << (record.cancel_requested ? "true" : "false")
        << R"(,"notification_delivered":)" << (record.notification_delivered ? "true" : "false")
        << R"(,"worktree_cleanup_performed":)" << (record.worktree_cleanup_performed ? "true" : "false")
        << R"(,"transcript_path":")" << json_escape(transcript_path.string()) << '"'
        << R"(,"sidechain_jsonl_path":")" << json_escape(sidechain_path.string()) << '"'
        << R"(,"output_file_path":")" << json_escape(output_path.string()) << '"';
    write_json_optional_string(out, "parent_agent_id", record.parent_agent_id);
    write_json_optional_string(out, "description", record.description);
    write_json_optional_string(out, "name", record.name);
    write_json_optional_string(out, "team_name", record.team_name);
    write_json_optional_string(out, "cwd", record.cwd);
    write_json_optional_string(out, "isolation", record.isolation);
    write_json_optional_string(out, "mode", record.mode);
    write_json_optional_string(out, "output", record.output);
    write_json_optional_string(out, "error", record.error);
    write_json_optional_string(out, "worktree_path", record.worktree_path);
    write_json_optional_string(out, "worktree_branch", record.worktree_branch);
    write_json_optional_string(out, "worktree_base_commit", record.worktree_base_commit);
    write_json_optional_string(out, "worktree_git_root", record.worktree_git_root);
    write_json_optional_string(out, "teammate_backend", record.teammate_backend);
    write_json_optional_string(out, "teammate_task_id", record.teammate_task_id);
    write_json_optional_string(out, "teammate_pane_id", record.teammate_pane_id);
    write_json_optional_string(out, "teammate_color", record.teammate_color);
    write_json_optional_string(out, "parent_session_id", record.parent_session_id);
    if (record.progress) out << R"(,"progress":)" << *record.progress;
    write_json_string_array(out, "sidechain_entries", record.sidechain_entries);
    write_json_string_array(out, "pending_messages", record.pending_messages);
    write_json_string_array(out, "capabilities", record.capabilities);
    out << '}';
    return out.good();
}
[[nodiscard]] std::vector<std::string> read_transcript_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}
[[nodiscard]] std::string transcript_text_from_content(cc::utils::json::JsonVal content) {
    if (!content.valid()) return {};
    if (content.is_str()) return std::string(content.as_str());

    if (content.is_obj()) {
        if (auto text = content.get("text"); text.is_str()) return std::string(text.as_str());
        return transcript_text_from_content(content.get("content"));
    }

    if (!content.is_arr()) return {};

    std::string out;
    auto append = [&](std::string text) {
        text = trim(text);
        if (text.empty()) return;
        if (!out.empty()) out += '\n';
        out += std::move(text);
    };

    content.iter([&](cc::utils::json::JsonVal block) {
        if (block.is_str()) {
            append(std::string(block.as_str()));
            return;
        }
        if (!block.is_obj()) return;

        const auto type = json_string_field(block, {"type"});
        if (auto text = block.get("text"); text.is_str()) {
            append(std::string(text.as_str()));
            return;
        }
        if (type == "tool_use") {
            auto name = json_string_field(block, {"name"});
            append(name.empty() ? "[tool_use]" : "[tool_use:" + name + "]");
            return;
        }
        if (type == "tool_result") {
            auto nested = transcript_text_from_content(block.get("content"));
            append(nested.empty() ? "[tool_result]" : "tool_result: " + nested);
            return;
        }
        append(transcript_text_from_content(block.get("content")));
    });
    return out;
}
[[nodiscard]] std::optional<std::string> transcript_entry_from_ts_jsonl(cc::utils::json::JsonVal root) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;

    auto type = json_string_field(root, {"type"});
    if (type != "user" && type != "assistant" && type != "system") return std::nullopt;

    std::string role = type;
    cc::utils::json::JsonVal content = root.get("content");
    auto message = root.get("message");
    if (message.is_obj()) {
        auto message_role = json_string_field(message, {"role"});
        if (!message_role.empty()) role = message_role;
        content = message.get("content");
    }

    auto text = transcript_text_from_content(content);
    if (text.empty()) return std::nullopt;
    return std::format("{}: {}", role, text);
}
[[nodiscard]] std::optional<std::string> transcript_entry_from_sidechain_jsonl_line(std::string_view line) {
    auto parsed = cc::utils::json::parse(line);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto raw = root.get("raw");
    if (raw.is_str() && !raw.as_str().empty()) {
        return std::string(raw.as_str());
    }
    auto role = root.get("role");
    auto content = root.get("content");
    if (role.is_str() && content.is_str()) {
        return std::format("{}: {}", role.as_str(), content.as_str());
    }
    return transcript_entry_from_ts_jsonl(root);
}
[[nodiscard]] std::vector<std::string> transcript_lines_from_sidechain_entries(
    const std::vector<std::string>& entries
) {
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.empty()) continue;
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(entry)) {
            lines.push_back(std::move(*transcript_entry));
        }
    }
    return lines;
}
[[nodiscard]] std::vector<std::string> read_sidechain_transcript_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(line)) {
            lines.push_back(std::move(*transcript_entry));
        }
    }
    return lines;
}
[[nodiscard]] std::vector<std::string> read_sidechain_jsonl_entries(const fs::path& path) {
    std::vector<std::string> entries;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) entries.push_back(std::move(line));
    }
    return entries;
}
[[nodiscard]] std::optional<NativeAgentRecord> load_native_agent_record_from_path(const fs::path& path) {
    auto parsed = cc::utils::json::parse_file(path);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;
    auto root = parsed->root();
    auto agent_id = root.get_string("agent_id");
    if (agent_id.empty()) return std::nullopt;

    auto status = native_agent_status_from_string(root.get_string("status"))
        .value_or(NativeAgentStatus::Queued);
    auto agent_type = root.get_string("agent_type").empty()
        ? std::string("runtime")
        : root.get_string("agent_type");
    NativeAgentRecord record = make_native_agent_record(std::move(agent_id), std::move(agent_type));
    record.background = root.get("background").is_bool() && root.get("background").as_bool();
    record.status = status;
    record.capabilities = json_string_array(root.get("capabilities"));
    record.cancel_requested = root.get("cancel_requested").is_bool() && root.get("cancel_requested").as_bool();
    record.notification_delivered = root.get("notification_delivered").is_bool() &&
        root.get("notification_delivered").as_bool();
    record.worktree_cleanup_performed = root.get("worktree_cleanup_performed").is_bool() &&
        root.get("worktree_cleanup_performed").as_bool();

    auto assign_optional = [&](std::string_view key, std::optional<std::string>& field) {
        auto value = root.get(key);
        if (value.is_str()) field = std::string(value.as_str());
    };
    assign_optional("parent_agent_id", record.parent_agent_id);
    assign_optional("description", record.description);
    assign_optional("name", record.name);
    assign_optional("team_name", record.team_name);
    assign_optional("cwd", record.cwd);
    assign_optional("isolation", record.isolation);
    assign_optional("mode", record.mode);
    assign_optional("output", record.output);
    assign_optional("error", record.error);
    assign_optional("transcript_path", record.transcript_path);
    assign_optional("sidechain_jsonl_path", record.sidechain_jsonl_path);
    assign_optional("output_file_path", record.output_file_path);
    assign_optional("worktree_path", record.worktree_path);
    assign_optional("worktree_branch", record.worktree_branch);
    assign_optional("worktree_base_commit", record.worktree_base_commit);
    assign_optional("worktree_git_root", record.worktree_git_root);
    assign_optional("teammate_backend", record.teammate_backend);
    assign_optional("teammate_task_id", record.teammate_task_id);
    assign_optional("teammate_pane_id", record.teammate_pane_id);
    assign_optional("teammate_color", record.teammate_color);
    assign_optional("parent_session_id", record.parent_session_id);
    auto progress = root.get("progress");
    if (progress.is_num()) record.progress = progress.as_double();
    record.sidechain_entries = json_string_array(root.get("sidechain_entries"));
    record.pending_messages = json_string_array(root.get("pending_messages"));
    if (record.sidechain_entries.empty() && record.sidechain_jsonl_path) {
        record.sidechain_entries = read_sidechain_jsonl_entries(fs::path{*record.sidechain_jsonl_path});
    }
    if (record.transcript_path) {
        record.transcript = read_transcript_lines(fs::path{*record.transcript_path});
    }
    if (record.transcript.empty() && record.sidechain_jsonl_path) {
        record.transcript = read_sidechain_transcript_lines(fs::path{*record.sidechain_jsonl_path});
    }
    return record;
}
[[nodiscard]] std::optional<NativeAgentRecord> load_native_agent_record(std::string_view agent_id) {
    return load_native_agent_record_from_path(agent_record_path(agent_id));
}
[[nodiscard]] std::vector<NativeAgentRecord> load_all_native_agent_records() {
    std::vector<NativeAgentRecord> records;
    std::error_code ec;
    const auto dir = runtime_state_dir();
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return records;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        if (auto record = load_native_agent_record_from_path(entry.path())) {
            records.push_back(std::move(*record));
        }
    }
    return records;
}
} // namespace cc::tools::agent_runtime
