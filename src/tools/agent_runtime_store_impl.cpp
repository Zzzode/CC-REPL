// Implementation unit for cc.tools.agent_runtime — the NativeAgentStore
// out-of-line member functions, the single native_agent_store() singleton
// definition (Meyers function-local static), and the lifecycle entrypoints
// runtime_agent_id / run_agent / fork_subagent / resume_agent /
// get_agent_lifecycle. The private member template NativeAgentStore::update
// stays defined in the class body in the interface.
module;

module cc.tools.agent_runtime;

import std;

namespace cc::tools::agent_runtime {

namespace fs = std::filesystem;

void NativeAgentStore::upsert(NativeAgentRecord record) {
        std::scoped_lock lock(mutex_);
        if (!record.transcript_path) {
            record.transcript_path = agent_transcript_path(record.agent_id).string();
        }
        if (!record.sidechain_jsonl_path) {
            record.sidechain_jsonl_path = agent_sidechain_jsonl_path(record.agent_id).string();
        }
        if (!record.output_file_path) {
            record.output_file_path = agent_output_file_path(record.agent_id).string();
        }
        record.updated_at = std::chrono::system_clock::now();
        auto agent_id = record.agent_id;
        auto stored = record;
        records_.insert_or_assign(std::move(agent_id), std::move(record));
        (void)persist_native_agent_record(stored);
}

std::optional<NativeAgentRecord> NativeAgentStore::get(std::string_view agent_id) const {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) {
            auto loaded = load_native_agent_record(agent_id);
            if (!loaded) return std::nullopt;
            auto loaded_agent_id = loaded->agent_id;
            auto [inserted, _] = records_.emplace(std::move(loaded_agent_id), std::move(*loaded));
            it = inserted;
        }
        return it->second;
}

std::vector<NativeAgentRecord> NativeAgentStore::list() const {
        std::scoped_lock lock(mutex_);
        for (auto record : load_all_native_agent_records()) {
            auto agent_id = record.agent_id;
            records_[std::move(agent_id)] = std::move(record);
        }
        std::vector<NativeAgentRecord> out;
        out.reserve(records_.size());
        for (const auto& [_, record] : records_) out.push_back(record);
        std::ranges::sort(out, {}, &NativeAgentRecord::agent_id);
        return out;
}

void NativeAgentStore::mark_running(std::string_view agent_id) {
        update(agent_id, [](NativeAgentRecord& record) {
            record.status = NativeAgentStatus::Running;
            record.progress = 0.0;
        });
}

void NativeAgentStore::mark_completed(std::string_view agent_id, std::string output) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!output.empty()) {
                const auto transcript_entry = "assistant: " + output;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Completed;
            record.output = std::move(output);
            record.error = std::nullopt;
            record.progress = 1.0;
            record.notification_delivered = false;
        });
}

void NativeAgentStore::mark_failed(std::string_view agent_id, std::string error) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!error.empty()) {
                const auto transcript_entry = "system: agent failed: " + error;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Failed;
            record.error = std::move(error);
            record.output = std::nullopt;
            record.notification_delivered = false;
        });
}

void NativeAgentStore::mark_cancelled(std::string_view agent_id, std::string reason) {
        update(agent_id, [&](NativeAgentRecord& record) {
            if (!reason.empty()) {
                const auto transcript_entry = "system: agent cancelled: " + reason;
                if (!std::ranges::contains(record.transcript, transcript_entry)) {
                    record.transcript.push_back(transcript_entry);
                }
            }
            record.status = NativeAgentStatus::Cancelled;
            record.cancel_requested = true;
            record.error = std::move(reason);
            record.output = std::nullopt;
            record.notification_delivered = false;
        });
}

void NativeAgentStore::request_cancel(std::string_view agent_id, std::string reason) {
        mark_cancelled(agent_id, std::move(reason));
}

bool NativeAgentStore::is_cancel_requested(std::string_view agent_id) const {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        return it != records_.end() && it->second.cancel_requested;
}

std::vector<std::string> NativeAgentStore::take_pending_task_notifications() {
        std::scoped_lock lock(mutex_);
        for (auto record : load_all_native_agent_records()) {
            auto agent_id = record.agent_id;
            records_[std::move(agent_id)] = std::move(record);
        }

        std::vector<std::string> notifications;
        for (auto& [_, record] : records_) {
            if (!record.background || record.notification_delivered) continue;
            auto notification = format_native_agent_task_notification(record);
            if (!notification) continue;
            notifications.push_back(std::move(*notification));
            record.notification_delivered = true;
            (void)persist_native_agent_record(record);
        }
        std::ranges::sort(notifications);
        return notifications;
}

void NativeAgentStore::clear_for_testing() {
        std::scoped_lock lock(mutex_);
        records_.clear();
}

void NativeAgentStore::append_transcript(std::string_view agent_id, std::string entry) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.transcript.push_back(std::move(entry));
        });
}

void NativeAgentStore::enqueue_pending_message(std::string_view agent_id, std::string message) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.pending_messages.push_back(std::move(message));
        });
}

void NativeAgentStore::enqueue_resume_message(std::string_view agent_id, std::string message) {
        update(agent_id, [&](NativeAgentRecord& record) {
            const bool was_terminal =
                record.status == NativeAgentStatus::Completed ||
                record.status == NativeAgentStatus::Failed ||
                record.status == NativeAgentStatus::Cancelled;
            record.pending_messages.push_back(std::move(message));
            if (!was_terminal) return;

            record.status = NativeAgentStatus::Queued;
            record.output = std::nullopt;
            record.error = std::nullopt;
            record.progress = 0.0;
            record.cancel_requested = false;
            record.notification_delivered = false;
            record.transcript.push_back("system: resume requested from pending message");
        });
}

std::vector<std::string> NativeAgentStore::take_pending_messages(std::string_view agent_id) {
        std::vector<std::string> messages;
        update(agent_id, [&](NativeAgentRecord& record) {
            messages = std::move(record.pending_messages);
            record.pending_messages.clear();
        });
        return messages;
}

void NativeAgentStore::append_sidechain_message(
    std::string_view agent_id,
    std::string_view role,
    std::string_view content_json,
    std::string_view fallback_text
) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.sidechain_entries.push_back(make_sidechain_jsonl_entry(
                record.agent_id,
                record.sidechain_entries.size(),
                role,
                content_json,
                fallback_text));
        });
}

void NativeAgentStore::append_sidechain_entry(std::string_view agent_id, std::string entry) {
        if (entry.empty()) return;
        update(agent_id, [&](NativeAgentRecord& record) {
            record.sidechain_entries.push_back(std::move(entry));
        });
}

void NativeAgentStore::update_progress(std::string_view agent_id, double progress) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.progress = std::clamp(progress, 0.0, 1.0);
        });
}

void NativeAgentStore::set_worktree_metadata(
    std::string_view agent_id,
    std::string path,
    std::string branch,
    std::string base_commit,
    std::string git_root
) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.worktree_path = std::move(path);
            record.worktree_branch = std::move(branch);
            record.worktree_base_commit = std::move(base_commit);
            record.worktree_git_root = std::move(git_root);
            record.worktree_cleanup_performed = false;
        });
}

void NativeAgentStore::mark_worktree_cleaned(std::string_view agent_id) {
        update(agent_id, [](NativeAgentRecord& record) {
            if (record.worktree_path && record.cwd == record.worktree_path) {
                record.cwd = std::nullopt;
            }
            record.worktree_path = std::nullopt;
            record.worktree_branch = std::nullopt;
            record.worktree_base_commit = std::nullopt;
            record.worktree_git_root = std::nullopt;
            record.worktree_cleanup_performed = true;
        });
}

void NativeAgentStore::set_teammate_metadata(
    std::string_view agent_id,
    std::string backend,
    std::optional<std::string> task_id,
    std::optional<std::string> pane_id,
    std::optional<std::string> color,
    std::optional<std::string> parent_session_id
) {
        update(agent_id, [&](NativeAgentRecord& record) {
            record.teammate_backend = std::move(backend);
            record.teammate_task_id = std::move(task_id);
            record.teammate_pane_id = std::move(pane_id);
            record.teammate_color = std::move(color);
            record.parent_session_id = std::move(parent_session_id);
        });
}

NativeAgentStore& native_agent_store() {
    static NativeAgentStore store;
    return store;
}

// NOTE: build_fork_child_message and build_worktree_fork_notice live in the
// separate agent_runtime_text_impl.cpp translation unit with richer
// semantics. Those canonical definitions are kept; the previous stub
// implementations have been removed to avoid ODR violations.

[[nodiscard]] std::string runtime_agent_id(const AgentRuntimeConfig& config) {
    if (!config.agent_id.empty()) return config.agent_id;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "agent-" + std::to_string(now);
}
std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config) {
    auto id = runtime_agent_id(config);

    // migrated edge case: worktree specified but missing shouldn't silently
    // fall back to parent cwd at runtime — report the missing directory with
    // a clear message so the caller (AgentTool) can surface it.
    if (config.worktree_path && !config.worktree_path->empty()) {
        std::error_code ec;
        if (!fs::exists(*config.worktree_path, ec) || !fs::is_directory(*config.worktree_path, ec)) {
            NativeAgentRecord failed = make_native_agent_record(id, "runtime");
            failed.parent_agent_id = config.parent_agent_id;
            failed.cwd = config.working_dir.empty() ? std::nullopt : std::optional<std::string>{config.working_dir};
            failed.status = NativeAgentStatus::Failed;
            failed.error = "Configured worktree does not exist";
            failed.capabilities = config.capabilities;
            native_agent_store().upsert(std::move(failed));
            return std::unexpected("Configured worktree does not exist: " + *config.worktree_path);
        }
    }

    // migrated edge case: if caller provided a working_dir but the directory
    // is missing we record the failure and return an error rather than
    // spinning up the agent with an inconsistent cwd (matches TS
    // runWithCwdOverride that fails chdir -> throws).
    if (!config.working_dir.empty()) {
        std::error_code ec;
        if (!fs::exists(config.working_dir, ec) || !fs::is_directory(config.working_dir, ec)) {
            NativeAgentRecord failed = make_native_agent_record(id, "runtime");
            failed.parent_agent_id = config.parent_agent_id;
            failed.cwd = config.working_dir;
            failed.status = NativeAgentStatus::Failed;
            failed.error = "Working directory does not exist";
            failed.capabilities = config.capabilities;
            failed.worktree_path = config.worktree_path;
            failed.worktree_branch = config.worktree_branch;
            failed.worktree_base_commit = config.worktree_base_commit;
            failed.worktree_git_root = config.worktree_git_root;
            native_agent_store().upsert(std::move(failed));
            return std::unexpected("Working directory does not exist: " + config.working_dir);
        }
    }

    // migrated edge case: prevent unbounded nesting. TS uses MAX_SUBAGENT_DEPTH
    // implicitly through fork recursion detection; we track depth via parent_agent_id
    // traversal and reject any agent whose chain exceeds 16 ancestors.
    {
        std::size_t depth = 0;
        std::optional<std::string> cur = config.parent_agent_id;
        while (cur && !cur->empty() && depth < 17) {
            auto rec = native_agent_store().get(*cur);
            if (!rec) break;
            ++depth;
            cur = rec->parent_agent_id;
        }
        if (depth >= 16) {
            NativeAgentRecord failed = make_native_agent_record(id, "runtime");
            failed.parent_agent_id = config.parent_agent_id;
            failed.status = NativeAgentStatus::Failed;
            failed.error = "Agent nesting depth limit exceeded";
            native_agent_store().upsert(std::move(failed));
            return std::unexpected("Agent nesting depth limit exceeded");
        }
    }

    NativeAgentRecord queued = make_native_agent_record(id, "runtime");
    queued.parent_agent_id = config.parent_agent_id;
    queued.cwd = config.working_dir.empty() ? std::nullopt : std::optional<std::string>{config.working_dir};
    queued.status = NativeAgentStatus::Queued;
    queued.capabilities = config.capabilities;
    queued.worktree_path = config.worktree_path;
    queued.worktree_branch = config.worktree_branch;
    queued.worktree_base_commit = config.worktree_base_commit;
    queued.worktree_git_root = config.worktree_git_root;
    native_agent_store().upsert(std::move(queued));
    native_agent_store().mark_running(id);

    // migrated edge case: fork directive on bare run_agent call — TS errors
    // out if fork_directive is supplied but allow_fork is false (the
    // directive would be silently lost). We mirror: require allow_fork when
    // fork_directive is non-empty.
    if (config.fork_directive && !config.fork_directive->empty() && !config.allow_fork) {
        native_agent_store().mark_failed(
            id,
            "run_agent: fork_directive supplied but allow_fork is false in config");
        return std::unexpected("run_agent: fork_directive supplied but allow_fork is false in config");
    }

    // NOTE: the actual LLM streaming loop is intentionally NOT here — it lives
    // in cc::tools::agent::AgentWorker::run_agent_loop (agent_tool.cppm),
    // which owns the Anthropic client, tool dispatch, and streaming state
    // machine. This function is the *runtime lifecycle bookkeeping* entrypoint
    // used by background/coordinator workers, tests, and RPC callers that only
    // need metadata / transcript / persistence semantics.
    //
    // To actually execute a query loop, instantiate an AgentWorker from the
    // cc.tools.agent module and invoke build_agent_execution_plan() followed
    // by run_agent_loop(). Those are wire-compatible with the TS `runAgent`
    // generator: same permission handling, same MCP isolation, same
    // SubagentStart/SubagentStop hook execution, same transcript persistence.

    std::string output = "Agent " + id + " completed";
    if (!config.working_dir.empty()) output += " in " + config.working_dir;
    if (!config.capabilities.empty()) {
        output += " with capabilities: ";
        for (std::size_t i = 0; i < config.capabilities.size(); ++i) {
            if (i != 0) output += ", ";
            output += config.capabilities[i];
        }
    }
    native_agent_store().append_transcript(id, "user: " + id);
    native_agent_store().append_transcript(id, "assistant: " + output);
    native_agent_store().mark_completed(id, output);
    auto record = native_agent_store().get(id);
    return AgentExecutionResult{
        .agent_id = id,
        .exit_code = 0,
        .output = output,
        .error = std::nullopt,
        .transcript = record ? record->transcript : std::vector<std::string>{},
    };
}
std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config) {
    if (parent_id.empty()) return std::unexpected("Parent agent id is required");
    if (!config.allow_fork) return std::unexpected("Agent forking is disabled by runtime config");

    auto parent = native_agent_store().get(parent_id);
    if (!parent) return std::unexpected("Parent agent not found: " + std::string(parent_id));
    if (native_agent_record_is_fork_child(*parent)) {
        return std::unexpected("Fork is not available inside a forked worker. Complete your task directly using your tools.");
    }

    auto child_id = runtime_agent_id(config);
    if (child_id == parent_id) child_id += "-fork";

    auto inherited_sidechain_entries = fork_sidechain_entries_for_child(*parent, child_id);
    auto inherited_transcript = transcript_lines_from_sidechain_entries(inherited_sidechain_entries);
    if (inherited_transcript.empty()) inherited_transcript = parent->transcript;
    inherited_transcript.push_back(std::format("system: forked from {}", parent->agent_id));
    auto unresolved_tool_use_ids = unresolved_tool_use_ids_from_sidechain_entries(inherited_sidechain_entries);
    if (config.fork_directive && !config.fork_directive->empty()) {
        auto directive_message = build_fork_child_message(*config.fork_directive);
        auto directive_content_json = fork_missing_tool_results_content_json(
            unresolved_tool_use_ids,
            directive_message);
        auto directive_entry = make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            directive_content_json,
            directive_message);
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(directive_entry)) {
            inherited_transcript.push_back(std::move(*transcript_entry));
        } else {
            inherited_transcript.push_back("user: " + directive_message);
        }
        inherited_sidechain_entries.push_back(std::move(directive_entry));
    } else if (!unresolved_tool_use_ids.empty()) {
        auto missing_tool_results_content_json = fork_missing_tool_results_content_json(unresolved_tool_use_ids, {});
        auto missing_tool_results_entry = make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            missing_tool_results_content_json,
            {});
        if (auto transcript_entry = transcript_entry_from_sidechain_jsonl_line(missing_tool_results_entry)) {
            inherited_transcript.push_back(std::move(*transcript_entry));
        }
        inherited_sidechain_entries.push_back(std::move(missing_tool_results_entry));
    }

    auto child_cwd = config.working_dir.empty()
        ? parent->cwd
        : std::optional<std::string>{config.working_dir};
    auto child_worktree_path = config.worktree_path;
    auto child_worktree_branch = config.worktree_branch;
    auto child_worktree_base_commit = config.worktree_base_commit;
    auto child_worktree_git_root = config.worktree_git_root;
    if (!child_worktree_path && parent->worktree_path && child_cwd && *child_cwd == *parent->worktree_path) {
        child_worktree_path = parent->worktree_path;
        child_worktree_branch = parent->worktree_branch;
        child_worktree_base_commit = parent->worktree_base_commit;
        child_worktree_git_root = parent->worktree_git_root;
    }
    if (child_worktree_path && parent->cwd && *child_worktree_path != *parent->cwd) {
        auto worktree_notice = build_worktree_fork_notice(*parent->cwd, *child_worktree_path);
        inherited_transcript.push_back("user: " + worktree_notice);
        inherited_sidechain_entries.push_back(make_sidechain_jsonl_entry(
            child_id,
            inherited_sidechain_entries.size(),
            "user",
            {},
            worktree_notice));
    }

    NativeAgentRecord child = make_native_agent_record(child_id, parent->agent_type);
    child.parent_agent_id = std::string(parent_id);
    child.team_name = parent->team_name;
    child.cwd = std::move(child_cwd);
    child.isolation = parent->isolation;
    child.mode = parent->mode;
    child.background = true;
    child.status = NativeAgentStatus::Queued;
    child.capabilities = fork_child_capabilities(*parent, config);
    child.sidechain_entries = std::move(inherited_sidechain_entries);
    child.worktree_path = std::move(child_worktree_path);
    child.worktree_branch = std::move(child_worktree_branch);
    child.worktree_base_commit = std::move(child_worktree_base_commit);
    child.worktree_git_root = std::move(child_worktree_git_root);
    child.transcript = std::move(inherited_transcript);
    native_agent_store().upsert(std::move(child));
    return child_id;
}
std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id) {
    if (agent_id.empty()) {
        return std::unexpected("resume_agent: empty agent_id is not allowed");
    }
    auto record = native_agent_store().get(agent_id);
    if (!record) return std::unexpected("Agent not found: " + std::string(agent_id));

    // migrated edge case: terminal statuses (completed/failed/cancelled) are
    // returned as-is; the caller decides whether to re-run. For running or
    // queued agents we transition to running so pollers pick them back up
    // (matches TS resumeAgentBackground which re-enters runAsyncAgentLifecycle).
    const bool already_terminal =
        record->status == NativeAgentStatus::Completed ||
        record->status == NativeAgentStatus::Failed ||
        record->status == NativeAgentStatus::Cancelled;

    // migrated edge case: fork-type agents track the parent's rendered
    // system prompt so resuming a fork child without a stored fork context
    // would lose the byte-identical prompt prefix. When the agent has no
    // sidechain entries AND is marked as a fork child we surface a clear
    // error instead of silently producing a divergent prompt cache key.
    const bool is_fork_child = native_agent_record_is_fork_child(*record);
    if (is_fork_child && record->sidechain_entries.empty() &&
        !already_terminal) {
        return std::unexpected(std::format(
            "Cannot resume fork agent '{}': no sidechain transcript available "
            "to reconstruct parent prompt bytes",
            record->agent_id));
    }

    if (record->worktree_path && !record->worktree_path->empty()) {
        std::error_code ec;
        auto worktree_path = fs::path{*record->worktree_path};
        if (fs::exists(worktree_path, ec) && fs::is_directory(worktree_path, ec)) {
            ec.clear();
            // migrated edge case: bump mtime so the stale-worktree reaper
            // (#22355-equivalent) doesn't delete a just-resumed worktree
            // before the first poll fires.
            fs::last_write_time(worktree_path, fs::file_time_type::clock::now(), ec);
        } else {
            auto previous_worktree = *record->worktree_path;
            native_agent_store().mark_worktree_cleaned(agent_id);
            native_agent_store().append_transcript(
                agent_id,
                "system: resumed worktree " + previous_worktree +
                    " no longer exists; falling back to parent cwd");
            record = native_agent_store().get(agent_id);
            if (!record) return std::unexpected("Agent not found after worktree resume update: " + std::string(agent_id));
        }
    }

    // migrated edge case: for non-terminal agents, re-mark running so the
    // parent poller / lifecycle manager doesn't think the agent is still
    // queued and skip its next wake-up. Terminal agents skip this so
    // completion outputs are preserved exactly as last emitted.
    if (!already_terminal) {
        const auto previous_status = record->status;
        native_agent_store().mark_running(agent_id);
        if (!(is_fork_child && previous_status == NativeAgentStatus::Queued)) {
            native_agent_store().append_transcript(
                agent_id,
                std::format("system: agent resumed (previous status: {})",
                    native_agent_status_name(previous_status)));
        }
        record = native_agent_store().get(agent_id);
        if (!record) return std::unexpected("Agent not found after resume status update: " + std::string(agent_id));
    }

    std::string output = record->output.value_or(
        std::format("Agent {} is {} (previous: {})", record->agent_id,
            native_agent_status_name(record->status),
            native_agent_status_name(
                already_terminal ? record->status :
                is_fork_child ? NativeAgentStatus::Queued : record->status)));
    auto error = record->error;
    auto exit_code = record->status == NativeAgentStatus::Failed ? 1 :
        record->status == NativeAgentStatus::Cancelled ? 130 : 0;
    return AgentExecutionResult{
        .agent_id = record->agent_id,
        .exit_code = exit_code,
        .output = std::move(output),
        .error = std::move(error),
        .transcript = record->transcript,
    };
}
AgentLifecycle get_agent_lifecycle(std::string_view agent_id) {
    auto record = native_agent_store().get(agent_id);
    if (!record) return AgentLifecycle::Failed;
    switch (record->status) {
        case NativeAgentStatus::Queued: return AgentLifecycle::Starting;
        case NativeAgentStatus::Running: return AgentLifecycle::Running;
        case NativeAgentStatus::Completed: return AgentLifecycle::Completed;
        case NativeAgentStatus::Failed: return AgentLifecycle::Failed;
        case NativeAgentStatus::Cancelled: return AgentLifecycle::Cancelled;
    }
    return AgentLifecycle::Failed;
}
} // namespace cc::tools::agent_runtime
