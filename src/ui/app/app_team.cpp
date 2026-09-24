// app_team.cpp — plain impl unit for cc.ui.app. Owns TeammateState (the
// nested PIMPL for the teammate inbox/permission cluster).
//
// Contains the leader-side live teams projection:
//   AppAdapter::ProjectLiveTeammatesToScreenState()
//     roster (<root>/<sanitized team>/config.json, camelCase)
//     + in-process native agent store
//     + cc::utils::pane_observer tmux/iTerm pane snapshots
//   AppAdapter::drain_one_teammate_permission()
//     stage-A permission_request envelopes -> the existing
//     ToolPermission dialog (Band3 overlay) -> PermissionSync reply
//   AppAdapter::start_leader_inbox_worker()
//     background jthread polling the team-lead mailbox
//
// Mirrors app_agent_menu.cpp / app_extra_methods.cpp: kept in its own TU so
// the swarm/observer import closure never enters app.cppm's source-location
// budget. Event-driven: the observer's background poller only flags a dirty
// atomic + posts one FTXUI event; there is NO constant-rate render ticker.
module;

#include <cctype>
#include <cstdlib>

module cc.ui.app.app;

import std;

import cc.utils.json;
import cc.utils.team_helpers;
import cc.utils.swarm_helpers;
import cc.utils.swarm_backends;
import cc.utils.swarm_pane_observer;
import cc.tools.agent_runtime;
import cc.ui.features.teams.live_teammates;
import cc.ui.screens.repl_state;
import cc.ui.dialogs.system;
import cc.ui.dialogs.triggers;

namespace cc::ui {

struct TeammateState {
    // Leader-side teammate permission requests (drained into the dialog).
    struct PendingTeammatePermission {
        cc::utils::swarm_helpers::SwarmPermissionRequestMessage request;
        std::string team;
    };
    std::mutex teammate_permission_mutex_;
    std::deque<PendingTeammatePermission> teammate_pending_permissions_;

    // Pane-teammate inbox worker state.
    std::jthread teammate_inbox_thread_;
    std::mutex teammate_pending_mutex_;
    std::deque<std::string> teammate_pending_prompts_;
    std::unordered_set<std::string> teammate_seen_message_ids_;
    std::string teammate_self_agent_id_;
    std::string teammate_self_agent_name_;
    std::string teammate_self_team_;
};

void TeammateStateDeleter::operator()(TeammateState* p) const noexcept {
    delete p;
}

void AppAdapter::construct_teammate() {
    teammate_.reset(new TeammateState());
}


namespace repl = cc::ui::repl_screen;
namespace live = cc::ui::teams::live;
namespace sh = cc::utils::swarm_helpers;
namespace sw = cc::utils::swarm_backends;
namespace po = cc::utils::pane_observer;
namespace dtrig = cc::ui::dialogs::triggers;
namespace dsys = cc::ui::dialogs::system;

// ── Teammate inbox worker (moved out of app.cppm to keep TeammateMessage ──
//    and the file-inbox closure out of the interface BMI).
namespace {
std::string env_first(std::initializer_list<const char*> names) {
        for (const char* n : names) {
            if (const char* v = std::getenv(n); v && *v) return v;
        }
        return {};
    }

    // Stable per-message key so repeated polls don't redeliver. The inbox
    // entry has no id; from+timestamp+text is unique enough.
std::string teammate_message_key(const cc::utils::TeammateMessage& m) {
        return m.from + "|" + m.timestamp + "|" + m.text;
    }

    // Control messages (shutdown / permission / mode) are handled by dedicated
    // paths, not submitted as task prompts. Heuristic matching the TS inbox
    // classifier tags embedded in the message text.
bool is_teammate_control_message(std::string_view text) {
        static constexpr std::string_view tags[] = {
            "loom:shutdown", "loom:permission", "loom:mode",
            "loom:plan-approval", "loom:sandbox",
        };
        for (auto t : tags) {
            if (text.find(t) != std::string_view::npos) return true;
        }
        return false;
    }

}  // namespace

// True when this process was spawned with teammate identity.
bool AppAdapter::running_as_pane_teammate() const {
    return !teammate_->teammate_self_agent_name_.empty() &&
           !teammate_->teammate_self_team_.empty();
}

void AppAdapter::enqueue_teammate_prompt(std::string prompt) {
        {
            std::lock_guard lock(teammate_->teammate_pending_mutex_);
            teammate_->teammate_pending_prompts_.push_back(std::move(prompt));
        }
        PostRenderEvent();  // wake the UI thread to drain
    }

    // Called on the UI thread (Custom-event handler) when idle: submit one
    // queued teammate task. Returns true if a prompt was submitted.
bool AppAdapter::drain_one_teammate_prompt() {
        if (query_running_.load()) return false;
        std::string prompt;
        {
            std::lock_guard lock(teammate_->teammate_pending_mutex_);
            if (teammate_->teammate_pending_prompts_.empty()) return false;
            prompt = std::move(teammate_->teammate_pending_prompts_.front());
            teammate_->teammate_pending_prompts_.pop_front();
        }
        HandleSubmit(prompt);
        return true;
    }

void AppAdapter::start_teammate_inbox_worker() {
        teammate_->teammate_self_agent_id_ =
            env_first({"LOOM_AGENT_ID", "LOOM_AGENT_ID"});
        teammate_->teammate_self_agent_name_ =
            env_first({"LOOM_AGENT_NAME", "LOOM_AGENT_NAME"});
        teammate_->teammate_self_team_ =
            env_first({"LOOM_TEAM_NAME", "LOOM_TEAM_NAME"});
        if (!running_as_pane_teammate()) return;

        const std::string agent = teammate_->teammate_self_agent_name_;
        const std::string team = teammate_->teammate_self_team_;
        teammate_->teammate_inbox_thread_ = std::jthread(
            [this, agent, team](std::stop_token stop) {
                constexpr auto kPollInterval = std::chrono::milliseconds(1500);
                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(kPollInterval);
                    if (stop.stop_requested()) break;
                    poll_teammate_inbox_once(agent, team);
                }
            });
    }

    // One filesystem-inbox poll: read unread addressed messages, dedup,
    // enqueue task prompts (control messages skipped), mark read.
void AppAdapter::poll_teammate_inbox_once(const std::string& agent,
                                          const std::string& team) {
        auto msgs = cc::utils::read_inbox(agent, team);
        if (!msgs) return;

        std::vector<std::string> to_submit;
        for (const auto& m : *msgs) {
            if (m.read) continue;
            if (is_teammate_control_message(m.text)) continue;
            auto key = teammate_message_key(m);
            {
                std::lock_guard lock(teammate_->teammate_pending_mutex_);
                if (!teammate_->teammate_seen_message_ids_.insert(key).second) continue;
            }
            // Wrap like the TS useInboxPoller delivery format so the model
            // sees the sender identity.
            to_submit.push_back(std::format(
                "<teammate_message teammate_id=\"{}\">\n{}\n"
                "</teammate_message>",
                m.from, m.text));
        }

        // Mark everything we read as processed (file inbox).
        if (!to_submit.empty() || !msgs->empty()) {
            (void)cc::utils::mark_all_read(agent, team);
        }
        for (auto& p : to_submit) {
            enqueue_teammate_prompt(std::move(p));
        }
    }

namespace {

namespace fs = std::filesystem;

/// Teams runtime root — identical resolution to team_tool.cppm
/// team_runtime_dir() and team_helpers detail::teams_dir().
fs::path teams_root_dir() {
    if (const char* env = std::getenv("LOOM_TEAM_RUNTIME_DIR");
        env && *env) {
        return fs::path{env};
    }
    if (const char* env = std::getenv("LOOM_TEAMS_DIR");
        env && *env) {
        return fs::path{env};
    }
    return fs::current_path() / ".loom" / "teams";
}

/// Byte-identical to ts_sanitized_team_dir_name
/// (runtime_team_shared.cppm) and detail::sanitize_path_component
/// (team_helpers.cppm): lowercase alnum kept, everything else -> '-'.
std::string sanitize_team(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back('-');
        }
    }
    return out.empty() ? std::string{"team"} : out;
}

std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

/// Last physical line of captured output, carriage returns stripped, capped.
std::string last_tail_line(std::string text, std::size_t max = 200) {
    text = trim_copy(text);
    if (text.empty()) return {};
    if (const auto nl = text.find_last_of('\n'); nl != std::string::npos) {
        text = text.substr(nl + 1);
    }
    for (char& ch : text) {
        if (ch == '\r') ch = ' ';
    }
    text = trim_copy(text);
    if (text.size() > max) {
        text.resize(max > 0 ? max - 1 : 0);
        text += "…";
    }
    return text;
}

/// Transcript entries are stored as "role: text" ("user: ...",
/// "assistant: ...", "system: ..."). Strip one leading role prefix for the
/// tail display.
std::string strip_role_prefix(std::string line) {
    if (!line.empty() && line[0] != ':') {
        std::size_t i = 0;
        while (i < line.size() &&
               (std::isalpha(static_cast<unsigned char>(line[i])) ||
                line[i] == '-')) {
            ++i;
        }
        if (i + 2 <= line.size() && line[i] == ':' && line[i + 1] == ' ') {
            line = line.substr(i + 2);
        }
    }
    return line;
}

/// "alice@some-team" -> "alice"; identities without '@' pass through.
std::string short_agent_name(std::string_view agent_id) {
    if (const auto at = agent_id.find('@'); at != std::string_view::npos) {
        return std::string{agent_id.substr(0, at)};
    }
    return std::string{agent_id};
}

/// Lazily bind the process-wide pane observer to the cached pane backend.
/// Returns null when this environment has no tmux/iTerm backend (pure
/// in-process teams need no observer).
std::shared_ptr<po::PaneObserver> ensure_observer_for_cached_backend() {
    if (auto existing = po::global_pane_observer()) return existing;
    auto backend = sw::BackendRegistry::get_cached_backend();
    if (!backend) {
        const auto detection = sw::BackendRegistry::detect_and_get_backend();
        backend = sw::BackendRegistry::get_cached_backend();
        if (!backend || detection.needs_it2_setup) return nullptr;
    }
    return po::ensure_global_pane_observer(backend);
}

}  // namespace

// ============================================================================
// Live teammate projection
// ============================================================================

void AppAdapter::ProjectLiveTeammatesToScreenState() {
    std::vector<live::LiveTeammate> out;

    const auto team_name_opt = cc::utils::get_team_name();
    if (!team_name_opt || team_name_opt->empty()) return;  // not a leader
    const std::string team = *team_name_opt;

    // Lazy event-driven observer subscription (ONCE). The callback runs on
    // the observer jthread and must not touch screen_state_ off-thread:
    // flag + post, identical to the statusline worker pattern.
    if (pane_observer_token_ == 0) {
        auto sub = po::subscribe_changed_with_guard([this] {
            pane_snapshot_dirty_.store(true, std::memory_order_release);
            PostRenderEvent();
        });
        pane_observer_token_ = sub.first;
        pane_observer_sub_guard_ = std::move(sub.second);
    }

    std::unordered_map<std::string, std::size_t> by_agent;
    auto upsert_index = [&](const std::string& agent_id) -> std::size_t {
        if (auto it = by_agent.find(agent_id); it != by_agent.end()) {
            return it->second;
        }
        const std::size_t index = out.size();
        live::LiveTeammate t;
        t.agent_id = agent_id;
        t.name = short_agent_name(agent_id);
        by_agent.emplace(agent_id, index);
        out.push_back(std::move(t));
        return index;
    };

    // ── (1) Roster from <root>/<sanitized(team)>/config.json ──────────────
    // camelCase schema written by runtime_team_shared write_team_config_member
    // (members[]{name,agentId,tmuxPaneId,color,isActive,backendType}).
    // Parse failure => roster starts empty; native store + observer still
    // contribute below.
    {
        const fs::path config_path =
            teams_root_dir() / sanitize_team(team) / "config.json";
        std::error_code ec;
        if (fs::exists(config_path, ec)) {
            auto doc = cc::utils::json::parse_file(config_path);
            if (doc) {
                const auto members = doc->root().get("members");
                if (members.is_arr()) {
                    members.iter([&](cc::utils::json::JsonVal member) {
                        if (!member.is_obj()) return;
                        const std::string name = member.get_string("name");
                        // TS getTeammateStatuses filter (teamDiscovery.ts):
                        // the implicit lead row is not a teammate.
                        if (name == "team-lead") return;
                        const std::string agent_id =
                            member.get_string("agentId");
                        if (agent_id.empty()) return;
                        auto& t = out.emplace_back();
                        t.agent_id = agent_id;
                        t.name = name.empty() ? short_agent_name(agent_id)
                                             : name;
                        t.color = member.get_string("color");
                        t.pane_id = member.get_string("tmuxPaneId");
                        if (t.pane_id == "in-process") t.pane_id.clear();
                        const auto is_active = member.get("isActive");
                        t.status = (is_active.is_bool() && !is_active.as_bool())
                                       ? "idle"
                                       : "running";
                        by_agent.emplace(agent_id, out.size() - 1);
                    });
                }
            }
        }
    }

    // ── (2) In-process teammates from the native agent store ──────────────
    for (const auto& record : cc::tools::agent_runtime::native_agent_store()
                                  .list()) {
        if (!record.team_name || *record.team_name != team) continue;
        const std::size_t idx = upsert_index(record.agent_id);
        auto& t = out[idx];

        if (record.name && !record.name->empty()) t.name = *record.name;
        if (record.teammate_color && !record.teammate_color->empty()) {
            t.color = *record.teammate_color;
        }
        if (record.teammate_pane_id &&
            !record.teammate_pane_id->empty() &&
            *record.teammate_pane_id != "in-process") {
            t.pane_id = *record.teammate_pane_id;
        }

        using NativeStatus = cc::tools::agent_runtime::NativeAgentStatus;
        switch (record.status) {
            case NativeStatus::Queued:
            case NativeStatus::Running:
                t.status = "running";
                break;
            case NativeStatus::Completed:
            case NativeStatus::Cancelled:
                t.status = "idle";
                break;
            case NativeStatus::Failed:
                t.status = "unknown";
                break;
        }

        if (record.output && !record.output->empty()) {
            t.last_output_tail = last_tail_line(*record.output);
        } else if (!record.transcript.empty()) {
            t.last_output_tail =
                last_tail_line(strip_role_prefix(record.transcript.back()));
        }
        if (record.status == NativeStatus::Failed &&
            record.error && !record.error->empty()) {
            t.last_output_tail = last_tail_line(*record.error);
        }
    }

    // ── (3) tmux/iTerm pane snapshots via the observer ────────────────────
    if (auto observer = ensure_observer_for_cached_backend()) {
        const bool inside_tmux =
            sw::EnvironmentDetection::is_inside_tmux_sync();

        // Track roster panes not already tracked (track() resets the
        // snapshot, so never re-track an existing agent).
        const auto tracked = observer->tracked_agent_ids();
        std::unordered_map<std::string, bool> tracked_set;
        tracked_set.reserve(tracked.size());
        for (const auto& id : tracked) tracked_set.emplace(id, true);
        for (const auto& t : out) {
            if (t.pane_id.empty()) continue;
            if (tracked_set.contains(t.agent_id)) continue;
            observer->track(t.agent_id, t.pane_id, inside_tmux);
        }

        for (const auto& [agent_id, snap] : observer->get_pane_snapshot()) {
            std::size_t idx = 0;
            if (auto it = by_agent.find(agent_id); it != by_agent.end()) {
                idx = it->second;
            } else {
                // Snapshot for a pane missing from both config and the native
                // store: surface it defensively.
                idx = out.size();
                live::LiveTeammate t;
                t.agent_id = agent_id;
                t.name = short_agent_name(agent_id);
                out.push_back(std::move(t));
                by_agent.emplace(agent_id, idx);
            }
            auto& t = out[idx];
            if (!snap.agent_id.empty()) t.agent_id = snap.agent_id;

            // Last non-empty captured line.
            for (auto it = snap.lines.rbegin(); it != snap.lines.rend(); ++it) {
                const auto line = last_tail_line(*it);
                if (!line.empty()) {
                    t.last_output_tail = line;
                    break;
                }
            }
            if (t.last_output_tail.empty() && !snap.raw.empty()) {
                t.last_output_tail = last_tail_line(snap.raw);
            }
            if (snap.state == po::PaneRunState::Done) {
                t.status = "idle";
            } else if (t.status == "unknown") {
                t.status = "running";
            }
        }
    }

    std::ranges::sort(out, {}, &live::LiveTeammate::name);

    // Replace the state vector only when the projection actually changed,
    // to avoid needless reference churn on the event-driven refresh path.
    std::string signature;
    for (const auto& t : out) {
        signature += t.agent_id;
        signature += '|';
        signature += t.name;
        signature += '|';
        signature += t.color;
        signature += '|';
        signature += t.status;
        signature += '|';
        signature += t.last_output_tail;
        signature += '|';
        signature += t.pane_id;
        signature += '\n';
    }
    if (signature == projected_teams_signature_) return;
    projected_teams_signature_ = std::move(signature);
    screen_state_->live_teammates = std::move(out);
    screen_state_->teammate_count =
        static_cast<int>(screen_state_->live_teammates.size());
    // Callers own the render wake (SyncState ends in a render anyway; the
    // Custom-event caller posts nothing extra).
}

// ============================================================================
// Leader-side teammate permission requests
// ============================================================================

bool AppAdapter::drain_one_teammate_permission() {
    TeammateState::PendingTeammatePermission pending;
    {
        std::lock_guard lock(teammate_->teammate_permission_mutex_);
        // One ToolPermission overlay at a time, like every other Band3
        // request — wait for the active dialog to finish first.
        if (teammate_->teammate_pending_permissions_.empty() ||
            screen_state_->dialog_queue.has_overlay()) {
            return false;
        }
        pending = std::move(teammate_->teammate_pending_permissions_.front());
        teammate_->teammate_pending_permissions_.pop_front();
    }

    const auto request = pending.request;
    const std::string team = pending.team;
    // Show the worker's concrete tool input in the approval dialog.
    const std::string input_pretty =
        sh::format_permission_request_input(request.tool_name,
                                            request.input_json);
    const std::string description =
        "@" + request.agent_id + " requests " + request.tool_name +
        (request.description.empty() ? std::string{}
                                     : ("\n" + request.description)) +
        (input_pretty.empty() ? std::string{}
                              : ("\n\nInput:\n" + input_pretty));

    auto reply = [request, team](bool allow, bool always_allow,
                                 std::string error_text) {
        sh::SwarmPermissionResponseMessage response;
        response.type = "permission_response";
        response.request_id = request.request_id;
        if (allow) {
            response.subtype = "success";
            if (always_allow) {
                // Persist a whole-tool allow grant on the worker so matching
                // future calls no longer round-trip to the leader.
                response.permission_updates_json =
                    sh::build_always_allow_updates_json(request.tool_name);
            }
        } else {
            response.subtype = "error";
            response.error = std::move(error_text);
        }
        (void)sh::PermissionSync::send_response_to_worker(
            request.agent_id, response, team);
    };

    dtrig::PushToolPermission(
        screen_state_->dialog_queue,
        request.tool_name,
        description,
        /*on_response=*/[this, reply](
            dsys::ToolPermissionPayload::Decision decision,
            bool /*sandbox*/) {
            const bool allow =
                decision == dsys::ToolPermissionPayload::Decision::AllowOnce ||
                decision ==
                    dsys::ToolPermissionPayload::Decision::AlwaysAllow;
            const bool always_allow =
                decision == dsys::ToolPermissionPayload::Decision::AlwaysAllow;
            reply(allow, always_allow,
                  allow ? std::string{}
                        : std::string{"Permission denied by team lead"});
            screen_state_->dialog_queue.pop_overlay();
            PostRenderEvent();
        },
        /*on_abort=*/[this, reply] {
            reply(false, false, "Permission aborted by team lead");
            screen_state_->dialog_queue.pop_overlay();
            PostRenderEvent();
        },
        /*can_always_allow=*/true);
    PostRenderEvent();
    return true;
}

void AppAdapter::start_leader_inbox_worker() {
    // The leader has a team identity but no teammate agent identity. A pane
    // teammate process has both and uses the other inbox worker.
    const auto team_opt = cc::utils::get_team_name();
    if (!team_opt || team_opt->empty()) return;
    if (running_as_pane_teammate()) return;
    const std::string team = *team_opt;

    leader_inbox_thread_ = std::jthread(
        [this, team](std::stop_token stop) {
            constexpr auto kPollInterval = std::chrono::milliseconds(1500);
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(kPollInterval);
                if (stop.stop_requested()) break;

                auto messages =
                    cc::utils::read_inbox(std::string{sh::TEAM_LEAD_NAME}, team);
                if (!messages) continue;

                std::vector<TeammateState::PendingTeammatePermission> fresh;
                std::vector<std::string> consumed_texts;
                for (const auto& message : *messages) {
                    // Discriminator substrings from the frozen stage-A
                    // protocol; the generic "loom:permission" tag grep is
                    // intentionally NOT used here so only real envelopes
                    // parse.
                    if (message.text.find(
                            "\"type\":\"permission_request\"") ==
                        std::string::npos) {
                        continue;
                    }
                    auto parsed =
                        sh::PermissionSync::parse_request(message.text);
                    if (!parsed) continue;
                    {
                        std::lock_guard lock(teammate_->teammate_permission_mutex_);
                        if (!seen_leader_permission_ids_
                                 .insert(parsed->request_id)
                                 .second) {
                            continue;
                        }
                    }
                    fresh.push_back(
                        TeammateState::PendingTeammatePermission{std::move(*parsed), team});
                    consumed_texts.push_back(message.text);
                }

                // Remove consumed envelopes without touching unrelated
                // mailbox traffic (remove-on-consume mirrors the worker-side
                // response path).
                for (const auto& text : consumed_texts) {
                    sh::permission_detail::remove_mailbox_message_by_text(
                        std::string{sh::TEAM_LEAD_NAME}, team, text);
                }
                if (!fresh.empty()) {
                    {
                        std::lock_guard lock(teammate_->teammate_permission_mutex_);
                        for (auto& item : fresh) {
                            teammate_->teammate_pending_permissions_.push_back(
                                std::move(item));
                        }
                    }
                    PostRenderEvent();
                }
            }
        });
}


// ── Teammate test seams ────────────────────────────────────────────────────
void AppAdapter::configure_teammate_for_testing(std::string agent_name,
                                               std::string team) {
    teammate_->teammate_self_agent_name_ = std::move(agent_name);
    teammate_->teammate_self_team_ = std::move(team);
}

void AppAdapter::poll_teammate_inbox_once_for_testing() {
    poll_teammate_inbox_once(teammate_->teammate_self_agent_name_,
                             teammate_->teammate_self_team_);
}

std::size_t AppAdapter::teammate_pending_count_for_testing() {
    std::lock_guard lock(teammate_->teammate_pending_mutex_);
    return teammate_->teammate_pending_prompts_.size();
}

std::string AppAdapter::pop_teammate_prompt_for_testing() {
    std::lock_guard lock(teammate_->teammate_pending_mutex_);
    if (teammate_->teammate_pending_prompts_.empty()) return {};
    std::string out = std::move(teammate_->teammate_pending_prompts_.front());
    teammate_->teammate_pending_prompts_.pop_front();
    return out;
}

void AppAdapter::enqueue_teammate_permission_for_testing(void* request,
                                                         std::string team) {
    auto* req = static_cast<cc::utils::swarm_helpers::SwarmPermissionRequestMessage*>(request);
    {
        std::lock_guard lock(teammate_->teammate_permission_mutex_);
        teammate_->teammate_pending_permissions_.push_back(
            TeammateState::PendingTeammatePermission{*req, std::move(team)});
    }
    PostRenderEvent();
}

std::size_t AppAdapter::pending_teammate_permission_count_for_testing() {
    std::lock_guard lock(teammate_->teammate_permission_mutex_);
    return teammate_->teammate_pending_permissions_.size();
}


void AppAdapter::set_live_teammates_for_testing(void* v) {
    auto& teammates =
        *static_cast<std::vector<cc::ui::teams::live::LiveTeammate>*>(v);
    screen_state_->live_teammates = std::move(teammates);
    screen_state_->teammate_count =
        static_cast<int>(screen_state_->live_teammates.size());
}

bool AppAdapter::teams_overview_open_for_testing() const {
    auto peek = screen_state_->dialog_queue.peek_modal();
    return peek.has_value() &&
           std::holds_alternative<
               cc::ui::dialogs::system::TeamsViewPayload>(peek->get());
}

}  // namespace cc::ui
