// Implementation unit for cc.query.query_engine — conversation lifecycle:
// get/restore/clear, locking append (+ transcript persistence + auto
// compact trigger), session metadata/dump-prompt wiring, explicit
// compaction, session-summary I/O, user-message construction, transcript
// flattening, and the detached post-turn memory-extraction sub-engine.
module;

module cc.query.query_engine;

import std;

import cc.types.types;
import cc.utils.error;
import cc.utils.json;
// ToolRegistry (used by the memory-extraction thread) arrives through the
// primary interface's retained `import cc.tools.tool` — visible to every
// impl unit of this module; no direct import needed here.
import cc.session.storage;
import cc.memdir.paths;
import cc.services.extract_memories;
import cc.utils.debug;

namespace cc::core {

std::vector<Message> QueryEngine::get_conversation() const {
    std::lock_guard lock(conversation_mutex_);
    return conversation_;
}

void QueryEngine::restore_conversation(std::vector<Message> messages) {
    std::lock_guard lock(conversation_mutex_);
    conversation_ = std::move(messages);
    restore_task_budget_remaining_from_compact_boundaries_locked();
    rebuild_content_replacement_state_locked();
}

void QueryEngine::clear_conversation() {
    std::lock_guard lock(conversation_mutex_);
    conversation_.clear();
    build_and_add_system_prompt();
}

[[nodiscard]] cc::utils::VoidResult QueryEngine::compact_conversation(std::string_view trigger) {
    std::lock_guard lock(conversation_mutex_);
    if (conversation_.size() <= 4) {
        return {};  // Nothing to compact
    }

    // Keep first (system) and last N messages, drop middle
    constexpr std::size_t keep_recent = 6;
    if (conversation_.size() <= keep_recent + 1) return {};

    const auto pre_tokens = estimate_conversation_tokens_locked();
    update_task_budget_remaining_after_compact(pre_tokens);
    auto recent_start = conversation_.end() - static_cast<std::ptrdiff_t>(keep_recent);
    auto summary_text = build_compaction_summary(
        conversation_.begin() + 1,
        recent_start);
    // Persist the summary for future runs/resumption of this session.
    append_session_summary(working_directory(), summary_text);

    std::vector<Message> retained_recent;
    retained_recent.reserve(keep_recent);
    for (auto it = recent_start; it != conversation_.end(); ++it) {
        if (is_compact_boundary_message(*it)) continue;
        retained_recent.push_back(*it);
    }

    const auto summary_id = generate_id();
    CompactMetadata metadata{
        .trigger = std::string(trigger),
        .pre_tokens = pre_tokens,
        .preserved_segment = compact_preserved_segment(retained_recent.begin(), retained_recent.end(), summary_id),
    };

    // Replace middle section with a compact boundary and summary marker.
    std::vector<Message> compacted;
    compacted.push_back(conversation_.front());  // System prompt

    SystemMessage boundary{};
    boundary.id.value = generate_id();
    boundary.timestamp = std::chrono::system_clock::now();
    boundary.subtype = "compact_boundary";
    boundary.compact_metadata = std::move(metadata);
    boundary.content.push_back(TextBlock{std::format(
        "Conversation compacted by {} compact.",
        trigger.empty() ? std::string_view{"manual"} : trigger)});
    compacted.push_back(Message{std::move(boundary)});

    UserMessage marker{};
    marker.id.value = summary_id;
    marker.timestamp = std::chrono::system_clock::now();
    marker.content.push_back(TextBlock{std::move(summary_text)});
    compacted.push_back(Message{std::move(marker)});

    // Keep recent messages, dropping stale compact boundaries from prior compaction chains.
    compacted.insert(compacted.end(), retained_recent.begin(), retained_recent.end());

    conversation_ = std::move(compacted);
    return {};
}

void QueryEngine::set_session_storage(std::filesystem::path sessions_dir) {
    sessions_dir_ = std::move(sessions_dir);
    cc::session::SessionMetadata meta{
        .session_id = session_id_.str(),
        .model = config_.model_params.model,
        .cwd = std::filesystem::current_path(),
        .created_at = session_start_,
        .last_active = std::chrono::system_clock::now(),
        .message_count = 0,
        .title = std::nullopt,
        .is_archived = false,
    };
    (void)cc::session::save_session_metadata(*sessions_dir_, meta);
}

void QueryEngine::set_dump_prompts_dir(std::filesystem::path dump_dir) {
    dump_prompts_dir_ = std::move(dump_dir);
    std::filesystem::create_directories(*dump_prompts_dir_);
}

void QueryEngine::flush_session() {
    if (!sessions_dir_) return;
    std::size_t count = 0;
    {
        std::lock_guard lock(conversation_mutex_);
        for (const auto& m : conversation_) {
            // SystemMessage is not persisted to the transcript (it is
            // rebuilt dynamically each query), so exclude it from the
            // on-disk message_count to match messages.jsonl line count.
            if (!std::holds_alternative<SystemMessage>(m)) ++count;
        }
    }
    cc::session::SessionMetadata meta{
        .session_id = session_id_.str(),
        .model = config_.model_params.model,
        .cwd = std::filesystem::current_path(),
        .created_at = session_start_,
        .last_active = std::chrono::system_clock::now(),
        .message_count = static_cast<int>(count),
        .title = std::nullopt,
        .is_archived = false,
    };
    (void)cc::session::save_session_metadata(*sessions_dir_, meta);
}

[[nodiscard]] std::string QueryEngine::message_to_jsonl_(const Message& msg) const {
    cc::utils::json::JsonMutDoc doc;
    auto arr = doc.array();
    append_message_to_json(msg, arr, doc);
    doc.set_root(arr);
    auto s = doc.to_string();
    // arr is a single-element array "[{...}]" -> strip outer brackets.
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

void QueryEngine::append_message(Message msg) {
    bool should_compact = false;
    std::string persist_json;
    bool do_persist = false;
    {
        std::lock_guard lock(conversation_mutex_);
        // Serialize for disk persistence BEFORE moving (if enabled).
        if (sessions_dir_) {
            persist_json = message_to_jsonl_(msg);
            do_persist = !persist_json.empty();
        }
        conversation_.push_back(std::move(msg));

        // Check if auto-compact needed (but don't call it while holding lock)
        if (config_.context_window.auto_compact &&
            context_utilization() > config_.context_window.compaction_threshold) {
            should_compact = true;
        }
    }
    // Persist transcript line (outside the conversation lock; ofstream is
    // not part of the engine critical section). SystemMessage serializes
    // empty and is skipped.
    if (do_persist) {
        (void)cc::session::append_message(
            *sessions_dir_, session_id_.str(), persist_json);
    }
    // Lock released — safe to call compact which re-acquires
    if (should_compact) {
        (void)compact_conversation("auto");
    }
}

[[nodiscard]] UserMessage QueryEngine::make_user_message(std::string_view text,
                                             std::optional<std::string_view> uuid) const {
    UserMessage msg{};
    if (uuid) {
        msg.id.value = std::string(*uuid);
    } else {
        msg.id.value = generate_id();
    }
    msg.timestamp = std::chrono::system_clock::now();
    msg.content.push_back(TextBlock{std::string(text)});
    return msg;
}

[[nodiscard]] std::string QueryEngine::read_session_summary(std::string_view cwd) const {
    const auto path = cc::memdir::get_session_memory_path(
        std::filesystem::path(cwd), session_id_.str());
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::string content((std::istreambuf_iterator<char>(ifs)), {});
    return content;
}

void QueryEngine::append_session_summary(std::string_view cwd,
                                         std::string_view summary) const {
    const auto dir = cc::memdir::get_session_memory_dir(
        std::filesystem::path(cwd), session_id_.str());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "summary.md";
    std::ofstream ofs(path, std::ios::app);
    if (!ofs) return;
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ofs << "\n\n## Compaction " << millis << "\n\n" << summary << '\n';
}

[[nodiscard]] std::string QueryEngine::transcript_to_text(
    const std::vector<Message>& messages, std::size_t max_chars) {
    std::string out;
    for (const auto& m : messages) {
        std::string role;
        std::vector<ContentBlock> empty_blocks;
        const std::vector<ContentBlock>* blocks = &empty_blocks;
        if (const auto* u = std::get_if<UserMessage>(&m)) {
            role = "user"; blocks = &u->content;
        } else if (const auto* a = std::get_if<AssistantMessage>(&m)) {
            role = "assistant"; blocks = &a->content;
        }
        for (const auto& b : *blocks) {
            const auto* tb = std::get_if<TextBlock>(&b);
            if (!tb || tb->text.empty()) continue;
            if (!out.empty()) out += "\n";
            out += role;
            out += ": ";
            out += tb->text;
        }
    }
    if (out.size() > max_chars) {
        out = out.substr(out.size() - max_chars);  // keep the newest end
    }
    return out;
}

void QueryEngine::maybe_run_memory_extraction() {
    namespace em = cc::services::extract_memories;
    if (!memory_extraction_enabled_) return;
    if (messages_since_last_extraction_ < em::kExtractionMinNewMessages * 2)
        return;
    // Process-wide single-flight via the engine-owned flag. The thread is
    // a member jthread joined at destruction, so captured state outlives it.
    bool expected = false;
    if (!memory_extraction_inflight_.compare_exchange_strong(expected, true))
        return;  // an extraction is already running

    const auto cwd = config_.cwd.value_or(
        std::filesystem::current_path().string());
    auto mem_dir_opt = cc::memdir::get_auto_mem_path(std::filesystem::path(cwd));
    if (!mem_dir_opt) {
        memory_extraction_inflight_.store(false);
        return;
    }
    std::error_code mkdir_ec;
    std::filesystem::create_directories(*mem_dir_opt, mkdir_ec);

    const auto mem_dir = mem_dir_opt->string();
    const auto recent = transcript_to_text(get_conversation(), 32'000);
    messages_since_last_extraction_ = 0;

    // Independent engine config sharing API creds / model / cwd but with a
    // fresh conversation. The extractor is a forked sub-agent that reads
    // the supplied transcript and writes memory files itself.
    QueryEngineConfig sub_config = config_;
    sub_config.custom_system_prompt = std::nullopt;
    sub_config.append_system_prompt = std::nullopt;
    sub_config.context_window.auto_compact = false;
    sub_config.max_turns = 5;  // TS cap; read→write, no verification loops

    const auto prompt = em::build_llm_extraction_prompt(mem_dir, recent, {});

    // Detached. Registry outlives engines (same assumption as AgentTool
    // sub-agents); no `this` is captured, so engine destruction is safe.
    // Member jthread: joined when this QueryEngine is destroyed, so the
    // captured registry pointer is always valid for the sub-agent's life.
    ToolRegistry* registry = tool_registry_;
    memory_extraction_thread_ = std::jthread(
        [this, registry, sub_config = std::move(sub_config),
         prompt = std::move(prompt)]() mutable {
        struct Guard {
            std::atomic<bool>& flag;
            ~Guard() { flag.store(false); }
        } guard{memory_extraction_inflight_};
        try {
            QueryEngine sub(std::move(sub_config), *registry);
            sub.memory_extraction_enabled_ = false;  // no recursion
            auto res = sub.query(prompt);
            if (!res) {
                cc::utils::debug("memory.extract",
                    "extraction sub-agent failed: {}", res.error().message);
            }
        } catch (const std::exception& e) {
            cc::utils::debug("memory.extract",
                "extraction sub-agent threw: {}", e.what());
        }
    });
}

} // namespace cc::core
