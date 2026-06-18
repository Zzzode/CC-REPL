/// @file test_bootstrap.cpp
/// @brief Tests for P1-05 (history / task_types / teammate_view_helpers)
///        and P1-06 (bootstrap/interactive_helpers).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import cc.bootstrap.interactive;
import cc.history;
import cc.task_types;
import cc.state.teammate_view_helpers;
import cc.utils.json;

using namespace std::literals;

namespace bi = cc::bootstrap::interactive;
namespace h  = cc::history;
namespace t  = cc::tasks;
namespace tv = cc::state::teammate_view;

static std::filesystem::path make_tmp_path(std::string_view suffix) {
    auto base = std::filesystem::temp_directory_path();
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<long> counter{0};
    auto name = "cc-repl-test-" + std::to_string(stamp) + "-" +
                std::to_string(counter.fetch_add(1)) + std::string(suffix);
    return base / name;
}

// ============================================================================
// Interactive helpers
// ============================================================================

TEST(Interactive, ParseSlashCommit) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input("/commit -m 'hello world'", ctx);
    ASSERT_TRUE(cmd) << cmd.error();
    EXPECT_EQ(cmd->kind, bi::CommandKind::SlashCommand);
    EXPECT_EQ(cmd->slash_name, "commit");
    ASSERT_EQ(cmd->slash_args.size(), 2u);
    EXPECT_EQ(cmd->slash_args[0], "-m");
    EXPECT_EQ(cmd->slash_args[1], "hello world");
}

TEST(Interactive, ParseBashBang) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input("!ls -la /tmp", ctx);
    ASSERT_TRUE(cmd) << cmd.error();
    EXPECT_EQ(cmd->kind, bi::CommandKind::BashCommand);
    EXPECT_EQ(cmd->bash_command_line, "ls -la /tmp");
}

TEST(Interactive, DollarBashContextual) {
    bi::InteractiveContext ctx_off;
    ctx_off.dollar_is_bash = false;
    auto c1 = bi::parse_interactive_input("$ pwd", ctx_off);
    ASSERT_TRUE(c1);
    EXPECT_EQ(c1->kind, bi::CommandKind::TextPrompt);

    bi::InteractiveContext ctx_on;
    ctx_on.dollar_is_bash = true;
    auto c2 = bi::parse_interactive_input("$ pwd", ctx_on);
    ASSERT_TRUE(c2);
    EXPECT_EQ(c2->kind, bi::CommandKind::BashCommand);
    EXPECT_EQ(c2->bash_command_line, "pwd");
}

TEST(Interactive, AtMentionOnly) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input("@teammate-alpha", ctx);
    ASSERT_TRUE(cmd) << cmd.error();
    EXPECT_EQ(cmd->kind, bi::CommandKind::AtMentionOnly);
    ASSERT_EQ(cmd->at_mentions.size(), 1u);
    EXPECT_EQ(cmd->at_mentions[0], "teammate-alpha");
}

TEST(Interactive, AtMentionMixed) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input("@user hi", ctx);
    ASSERT_TRUE(cmd) << cmd.error();
    EXPECT_EQ(cmd->kind, bi::CommandKind::TextPrompt);
    ASSERT_EQ(cmd->at_mentions.size(), 1u);
    EXPECT_EQ(cmd->at_mentions[0], "user");
}

TEST(Interactive, ParseEmpty) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input("   \n  ", ctx);
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->kind, bi::CommandKind::Empty);

    auto outcome = bi::dispatch_parsed(*cmd, ctx);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->action, bi::DispatchOutcome::Action::RejectWithReason);
    EXPECT_NE(outcome->reason.find("empty"), std::string::npos);
}

TEST(Interactive, PasteDetectJson) {
    bi::InteractiveContext ctx;
    ctx.paste_min_chars = 500;
    std::string body;
    std::string snippet = R"({"a":[1,2,3]})";
    while (body.size() < 600) body += snippet;
    auto cmd = bi::parse_interactive_input(body, ctx);
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->kind, bi::CommandKind::Paste);
    EXPECT_EQ(cmd->paste_kind, bi::ParsedCommand::PasteKind::Json);
}

TEST(Interactive, PasteDetectCode) {
    bi::InteractiveContext ctx;
    ctx.paste_min_chars = 500;
    std::string body;
    std::string snippet = "int main() { return 0; }\nfunction foo() { void bar(); }\n";
    while (body.size() < 650) body += snippet;
    auto cmd = bi::parse_interactive_input(body, ctx);
    ASSERT_TRUE(cmd);
    EXPECT_EQ(cmd->kind, bi::CommandKind::Paste);
    // Code detection requires >= 2 keyword matches; snippet provides both
    // "int main()" and "function " and "def-like function()" shape.
    EXPECT_EQ(cmd->paste_kind, bi::ParsedCommand::PasteKind::Code);
}

TEST(Interactive, SanitizeStripsANSI) {
    std::string input = "\x1b[31mRED\x1b[0m text";
    auto out = bi::sanitize_paste(input);
    EXPECT_EQ(out, "RED text");
}

TEST(Interactive, SanitizeNewlines) {
    std::string input = "a\r\nb\rc\n";
    auto out = bi::sanitize_paste(input);
    EXPECT_EQ(out, "a\nb\nc\n");
}

TEST(Interactive, SanitizeTruncates) {
    // Set limit and then exercise truncation branch.
    bi::set_paste_size_limit(1000);
    std::string big(100'000, 'x');
    auto out = bi::sanitize_paste(big);
    EXPECT_LT(out.size(), 2000u);
    EXPECT_NE(out.find("...[truncated"), std::string::npos);

    // Restore default for other tests (best-effort, tests run serially by default).
    bi::set_paste_size_limit(50 * 1024);
}

TEST(Interactive, DispatchUnknownSlash) {
    bi::InteractiveContext ctx;
    auto outcome = bi::process_complete("/foobar x", ctx);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->action, bi::DispatchOutcome::Action::RouteToCommand);
    EXPECT_EQ(outcome->target, "foobar");
    auto parsed = cc::utils::json::parse(outcome->payload_json);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->root().is_arr());
    EXPECT_EQ(parsed->root().size(), 1u);
    EXPECT_EQ(std::string(parsed->root().at(0).as_str()), "x");
}

TEST(Interactive, DangerousBashNeedsConfirm) {
    bi::InteractiveContext ctx;
    auto outcome = bi::process_complete("!sudo rm -rf /", ctx);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->action, bi::DispatchOutcome::Action::AskConfirmation);
    EXPECT_NE(outcome->confirmation_prompt.find("Confirm"), std::string::npos);
}

TEST(Interactive, QuoteAwareSplitComplex) {
    bi::InteractiveContext ctx;
    auto cmd = bi::parse_interactive_input(R"(/tool a 'b c' "d e" f)", ctx);
    ASSERT_TRUE(cmd) << cmd.error();
    ASSERT_EQ(cmd->slash_args.size(), 4u);
    EXPECT_EQ(cmd->slash_args[0], "a");
    EXPECT_EQ(cmd->slash_args[1], "b c");
    EXPECT_EQ(cmd->slash_args[2], "d e");
    EXPECT_EQ(cmd->slash_args[3], "f");
}

TEST(Interactive, SlashNameInvalid1) {
    EXPECT_FALSE(bi::is_valid_slash_name("123abc"));
    EXPECT_FALSE(bi::is_valid_slash_name(""));
    EXPECT_FALSE(bi::is_valid_slash_name("tool bad"));
    std::string too_long(40, 'a');
    EXPECT_FALSE(bi::is_valid_slash_name(too_long));
}

TEST(Interactive, SlashNameValid1) {
    EXPECT_TRUE(bi::is_valid_slash_name("compact-1_a"));
    EXPECT_TRUE(bi::is_valid_slash_name("commit"));
    EXPECT_TRUE(bi::is_valid_slash_name("a"));
    EXPECT_TRUE(bi::is_valid_slash_name("MCP-1_2"));
}

// ============================================================================
// History
// ============================================================================

TEST(History, AppendAndQuery) {
    h::SessionHistory s;
    s.session_id = "s1";
    for (int i = 0; i < 3; ++i) {
        h::HistoryMessage m;
        m.role = (i % 2 == 0) ? h::Role::User : h::Role::Assistant;
        m.content_json = "{\"text\":\"msg" + std::to_string(i) + "\"}";
        s.append(std::move(m));
    }
    EXPECT_EQ(s.size(), 3u);
    auto last2 = s.query(2);
    ASSERT_EQ(last2.size(), 2u);
    EXPECT_EQ(last2[0].id, 2u);
    EXPECT_EQ(last2[1].id, 3u);

    auto only_user = s.query(10, h::Role::User);
    ASSERT_EQ(only_user.size(), 2u);
    EXPECT_EQ(only_user[0].id, 1u);
    EXPECT_EQ(only_user[1].id, 3u);
}

TEST(History, CompactBefore) {
    h::SessionHistory s;
    s.session_id = "s";
    for (int i = 0; i < 10; ++i) {
        h::HistoryMessage m;
        m.content_json = "m" + std::to_string(i);
        s.append(std::move(m));
    }
    ASSERT_EQ(s.size(), 10u);
    // Compact before id=5: drop msgs 1..4 (4 messages), insert 1 summary
    auto dropped = s.compact_before(5, "\"summary\"");
    EXPECT_EQ(dropped, 4u);
    // New size: 1 summary + 6 remaining (ids 5..10) = 7
    EXPECT_EQ(s.size(), 7u);
    EXPECT_TRUE(s.messages.front().compacted);
    EXPECT_EQ(s.messages.front().role, h::Role::System);
}

TEST(History, ManagerCreateListDelete) {
    auto& m = h::HistoryManager::instance();
    // Isolate: remove any leftover sessions from other tests in the unlikely
    // event that tests share state; then run the 3-way lifecycle check.
    (void)m.delete_session("s1");
    (void)m.delete_session("s2");
    ASSERT_EQ(m.list_sessions().size(), 0u) << "unexpected previous sessions";

    auto s1 = m.create_session("s1");
    auto s2 = m.create_session("s2");
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s1->session_id, "s1");

    auto list = m.list_sessions();
    std::sort(list.begin(), list.end());
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], "s1");
    EXPECT_EQ(list[1], "s2");

    EXPECT_TRUE(m.delete_session("s1"));
    EXPECT_EQ(m.list_sessions().size(), 1u);
    EXPECT_TRUE(m.get_session("s2") != nullptr);
    EXPECT_TRUE(m.get_session("s1") == nullptr);

    // Cleanup
    (void)m.delete_session("s2");
}

TEST(History, SaveLoadRoundTrip) {
    auto& mgr = h::HistoryManager::instance();
    (void)mgr.delete_session("r1");

    auto* s = mgr.create_session("r1");
    ASSERT_NE(s, nullptr);
    for (int i = 0; i < 3; ++i) {
        h::HistoryMessage msg;
        msg.role = static_cast<h::Role>(i % 5);
        msg.content_json = "{\"n\":" + std::to_string(i) + "}";
        msg.model_used = "test-model";
        msg.token_estimate = 100 + i;
        s->append(std::move(msg));
    }
    EXPECT_EQ(s->size(), 3u);

    auto path = make_tmp_path("-hist.json");
    auto save = mgr.save_all(path);
    ASSERT_TRUE(save) << save.error();

    // Wipe in-memory state
    ASSERT_TRUE(mgr.delete_session("r1"));
    EXPECT_EQ(mgr.list_sessions().size(), 0u);

    auto load = mgr.load_all(path);
    ASSERT_TRUE(load) << load.error();
    auto* loaded = mgr.get_session("r1");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->size(), 3u);
    auto last3 = loaded->query(3);
    ASSERT_EQ(last3.size(), 3u);
    EXPECT_EQ(last3[0].id, 1u);
    EXPECT_EQ(last3[2].id, 3u);
    EXPECT_EQ(last3[2].model_used, "test-model");
    EXPECT_EQ(last3[1].token_estimate, 101u);

    // Also exercise standalone ser/de helpers.
    auto ser = h::to_json(*loaded);
    auto round = h::history_from_json(ser);
    ASSERT_TRUE(round) << round.error();
    EXPECT_EQ(round->session_id, "r1");
    EXPECT_EQ(round->size(), 3u);

    // Filesystem cleanup
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// Task types
// ============================================================================

TEST(TaskTypes, TaskCreateAndValidate) {
    t::TaskCreateParams ok;
    ok.title = "Implement frobnicator";
    ok.description = "...";
    ok.priority = t::TaskPriority::High;
    EXPECT_EQ(t::validate_task_create(ok), std::nullopt);

    t::TaskCreateParams missing_title;
    missing_title.title = "   \n\t  ";
    auto err = t::validate_task_create(missing_title);
    EXPECT_TRUE(err.has_value());
    EXPECT_NE(err->find("empty"), std::string::npos);
}

TEST(TaskTypes, Serde) {
    t::Task tsk;
    tsk.id = "abc123";
    tsk.title = "Write tests";
    tsk.description = "Lots of tests";
    tsk.status = t::TaskStatus::InProgress;
    tsk.priority = t::TaskPriority::Critical;
    tsk.assignee_agent_id = "agent-007";
    tsk.parent_id = "parent-1";
    tsk.subtask_ids = {"s1", "s2"};
    tsk.created_at_ms = 100;
    tsk.updated_at_ms = 200;
    tsk.due_at_ms = 300;
    tsk.external_ref = "T-42";
    tsk.metadata = {{"k1", "v1"}, {"k2", "v2"}};

    auto json = t::task_to_json(tsk);
    auto reparsed = t::task_from_json(json);
    ASSERT_TRUE(reparsed) << reparsed.error();
    EXPECT_EQ(reparsed->id, tsk.id);
    EXPECT_EQ(reparsed->title, tsk.title);
    EXPECT_EQ(reparsed->description, tsk.description);
    EXPECT_EQ(reparsed->status, tsk.status);
    EXPECT_EQ(reparsed->priority, tsk.priority);
    EXPECT_EQ(reparsed->assignee_agent_id, tsk.assignee_agent_id);
    EXPECT_EQ(reparsed->parent_id, tsk.parent_id);
    EXPECT_EQ(reparsed->subtask_ids, tsk.subtask_ids);
    EXPECT_EQ(reparsed->created_at_ms, tsk.created_at_ms);
    EXPECT_EQ(reparsed->updated_at_ms, tsk.updated_at_ms);
    EXPECT_EQ(reparsed->due_at_ms, tsk.due_at_ms);
    EXPECT_EQ(reparsed->external_ref, tsk.external_ref);
    EXPECT_EQ(reparsed->metadata, tsk.metadata);
}

TEST(TaskTypes, DescendantDetection) {
    std::unordered_map<std::string, t::Task> idx;
    t::Task a; a.id = "a";
    t::Task b; b.id = "b"; b.parent_id = "a";
    t::Task c; c.id = "c"; c.parent_id = "b";
    idx.emplace("a", a);
    idx.emplace("b", b);
    idx.emplace("c", c);

    EXPECT_TRUE(t::is_descendant_of(idx, "a", "c"));
    EXPECT_TRUE(t::is_descendant_of(idx, "a", "b"));
    EXPECT_FALSE(t::is_descendant_of(idx, "c", "a"));
    EXPECT_FALSE(t::is_descendant_of(idx, "a", "a"));
    EXPECT_FALSE(t::is_descendant_of(idx, "b", "a"));
}

TEST(TaskTypes, IdGeneration) {
    std::set<std::string> ids;
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    };
    for (int i = 0; i < 100; ++i) {
        auto id = t::generate_task_id();
        ASSERT_EQ(id.size(), 24u);
        for (char c : id) EXPECT_TRUE(is_hex(c)) << "bad hex char: " << c;
        EXPECT_TRUE(ids.insert(id).second) << "duplicate id: " << id;
    }
}

// ============================================================================
// Teammate view helpers
// ============================================================================

static tv::TeammateStateView make_tv(std::string id, std::string name,
                                     tv::TeammateStateView::OnlineState s,
                                     int active = 0,
                                     int64_t hb = 0) {
    tv::TeammateStateView m;
    m.agent_id = std::move(id);
    m.display_name = std::move(name);
    m.online = s;
    m.active_tasks = active;
    m.last_heartbeat_ms = hb;
    return m;
}

TEST(TeammateView, PinnedFirst) {
    std::vector<tv::TeammateStateView> all = {
        make_tv("a", "A", tv::TeammateStateView::OnlineState::Online),
        make_tv("b", "B", tv::TeammateStateView::OnlineState::Online),
        make_tv("c", "C", tv::TeammateStateView::OnlineState::Online),
    };
    tv::TeammateViewState view;
    view.pinned_ids.insert("b");
    view.sort = t::TaskSortKey::TitleAsc;

    auto res = tv::apply_view_filter(all, view);
    ASSERT_EQ(res.size(), 3u);
    EXPECT_EQ(res[0].agent_id, "b");
}

TEST(TeammateView, FilterText) {
    std::vector<tv::TeammateStateView> all = {
        make_tv("id-1", "alpha-bot", tv::TeammateStateView::OnlineState::Online),
        make_tv("id-2", "beta-agent", tv::TeammateStateView::OnlineState::Online),
        make_tv("id-3", "Gamma-ALPHA", tv::TeammateStateView::OnlineState::Offline),
    };
    tv::TeammateViewState view;
    view.filter_text = "alpha";
    auto res = tv::apply_view_filter(all, view);
    ASSERT_EQ(res.size(), 2u);
    std::set<std::string> got{res[0].display_name, res[1].display_name};
    EXPECT_TRUE(got.contains("alpha-bot"));
    EXPECT_TRUE(got.contains("Gamma-ALPHA"));
}

TEST(TeammateView, HideOffline) {
    std::vector<tv::TeammateStateView> all = {
        make_tv("a", "A", tv::TeammateStateView::OnlineState::Online),
        make_tv("b", "B", tv::TeammateStateView::OnlineState::Offline),
        make_tv("c", "C", tv::TeammateStateView::OnlineState::Crashed),
    };
    tv::TeammateViewState view;
    view.show_offline = false;
    view.sort = t::TaskSortKey::TitleAsc;
    auto res = tv::apply_view_filter(all, view);
    ASSERT_EQ(res.size(), 2u);
    std::set<std::string> got{res[0].agent_id, res[1].agent_id};
    EXPECT_TRUE(got.contains("a"));
    EXPECT_TRUE(got.contains("c")); // crashed is still visible
}

TEST(TeammateView, PinToggle) {
    tv::TeammateViewState v;
    EXPECT_FALSE(tv::is_pinned(v, "a"));
    tv::toggle_pin(v, "a");
    EXPECT_TRUE(tv::is_pinned(v, "a"));
    tv::toggle_pin(v, "a");
    EXPECT_FALSE(tv::is_pinned(v, "a"));
}

TEST(TeammateView, Serde) {
    tv::TeammateViewState v;
    v.collapsed = true;
    v.filter_text = "abc";
    v.sort = t::TaskSortKey::CreatedAtDesc;
    v.show_offline = false;
    v.pinned_ids = {"x", "y"};

    auto json = tv::teammate_view_state_to_json(v);
    auto back = tv::teammate_view_state_from_json(json);
    ASSERT_TRUE(back) << back.error();
    EXPECT_EQ(back->collapsed, v.collapsed);
    EXPECT_EQ(back->filter_text, v.filter_text);
    EXPECT_EQ(back->sort, v.sort);
    EXPECT_EQ(back->show_offline, v.show_offline);
    EXPECT_EQ(back->pinned_ids, v.pinned_ids);
}
