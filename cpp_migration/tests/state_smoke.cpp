/// @file state_smoke.cpp
/// @brief Phase 3-F standalone validator for cc.state.store_impl +
///        cc.state.persistence_json.
///
/// Exercises:
///   * default store → dispatch SetModel → snapshot matches & subscriber called
///   * dispatch with all 6 built-in reducers → revision monotonic
///   * custom reducer registered via register_reducer for AddSessionMetadata
///   * SaveToJson("/tmp/state.json") → new store LoadFromJson → data restored
/// Exit 0 on all assertions passed, non-zero + FAIL lines otherwise.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <filesystem>
#include <functional>

#include <yyjson.h>

import cc.state.store_impl;
import cc.state.persistence_json;

using namespace cc::state::store_impl;
using namespace cc::state::persistence_json;

static int g_fail = 0;
#define FAIL(...) do { \
    ++g_fail; \
    std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); \
} while(0)
#define CHECK(expr, ...) do { \
    if (!(expr)) { FAIL(__VA_ARGS__); } \
    else { std::printf("  OK "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while(0)

int main() {
    std::puts("=== Phase 3-F: AppState store + JSON persistence ===");

    // Initial state.
    AppStateData init{};
    init.settings.model_id   = "claude-haiku-4-5-20251001";
    init.settings.theme_name = "dark";
    init.flags.proactive     = true;
    init.flags.kairos        = false;
    init.user_id             = "phase3-user-01";

    AppStateStore store{std::move(init)};
    CHECK(store.snapshot().revision == 0, "initial revision 0 got=%llu",
          (unsigned long long)store.snapshot().revision);
    CHECK(store.snapshot().settings.model_id == "claude-haiku-4-5-20251001",
          "initial model preserved");

    // Subscribe.
    int calls = 0;
    std::string last_model_seen;
    std::uint64_t sub = store.subscribe([&](const AppStateData& s) {
        ++calls;
        last_model_seen = s.settings.model_id;
    });
    CHECK(sub != 0, "subscribe token non-zero got=%llu",
          (unsigned long long)sub);

    // Action: SetModel.
    Action set_model;
    set_model.type = ActionType::SetModel;
    set_model.json_payload = R"({"model_id":"claude-opus-4-8"})";
    store.dispatch(std::move(set_model));
    CHECK(store.snapshot().settings.model_id == "claude-opus-4-8",
          "SetModel reflected in snapshot got='%s'",
          store.snapshot().settings.model_id.c_str());
    CHECK(store.snapshot().active_session.has_value() == false,
          "no active session until SetActiveSession");
    CHECK(calls == 1, "subscriber called once got=%d", calls);
    CHECK(last_model_seen == "claude-opus-4-8", "subscriber saw new model");

    // Action: SetActiveSession.
    Action set_session;
    set_session.type = ActionType::SetActiveSession;
    set_session.json_payload = R"({"id":"sess-abc","model_id":"claude-opus-4-8","branch":"feat/x","draft_text":"hello","status":1})";
    store.dispatch(std::move(set_session));
    auto& s = store.snapshot();
    CHECK(s.active_session.has_value(), "active session set");
    CHECK(s.active_session->id == "sess-abc", "session id matches");
    CHECK(s.active_session->branch == "feat/x", "branch matches");
    CHECK(s.active_session->status == SessionStatus::Streaming,
          "SessionStatus::Streaming expected");

    // Action: UpdateDraft.
    Action upd;
    upd.type = ActionType::UpdateDraft;
    upd.json_payload = R"({"draft_text":"multi\nline\ndraft"})";
    store.dispatch(std::move(upd));
    CHECK(store.snapshot().active_session->draft_text == "multi\nline\ndraft",
          "draft updated got='%s'",
          store.snapshot().active_session->draft_text.c_str());

    // Action: ToggleFlag twice → back to initial state.
    Action tg;
    tg.type = ActionType::ToggleFlag;
    tg.json_payload = R"({"name":"kairos"})";
    store.dispatch(tg);
    CHECK(store.snapshot().flags.kairos == true, "ToggleFlag kairos → on");
    tg.json_payload = R"({"name":"kairos"})";
    store.dispatch(tg);
    CHECK(store.snapshot().flags.kairos == false, "ToggleFlag kairos → off again");

    // Action: UpdateSettings (partial merge).
    Action us;
    us.type = ActionType::UpdateSettings;
    us.json_payload = R"({"compact_mode":true,"max_tokens":8192,"temperature":0.3})";
    store.dispatch(std::move(us));
    const auto& settings = store.snapshot().settings;
    CHECK(settings.compact_mode, "compact_mode true");
    CHECK(settings.max_tokens == 8192, "max_tokens 8192 got=%d", settings.max_tokens);
    CHECK(settings.temperature > 0.29 && settings.temperature < 0.31,
          "temperature 0.3 got=%f", settings.temperature);
    // model_id and theme_name must be unchanged.
    CHECK(settings.model_id == "claude-opus-4-8", "model preserved across partial UpdateSettings");
    CHECK(settings.theme_name == "dark", "theme preserved across partial UpdateSettings");

    // Action: AddRecentFile (path + dedup + order + cap).
    Action arf;
    arf.type = ActionType::AddRecentFile;
    for (int i = 0; i < 55; ++i) {
        arf.json_payload = R"({"path":"/tmp/f)" + std::to_string(i) + R"("})";
        store.dispatch(arf);
    }
    // Cap is 50.
    CHECK(store.snapshot().recent_files.size() == 50,
          "recent_files cap=50 got=%zu", store.snapshot().recent_files.size());
    // Most recent first.
    CHECK(store.snapshot().recent_files.front().path == "/tmp/f54",
          "front path /tmp/f54 got='%s'",
          store.snapshot().recent_files.front().path.c_str());
    // Dedup: re-add /tmp/f50 — should jump to front, not duplicate.
    arf.json_payload = R"({"path":"/tmp/f50"})";
    store.dispatch(std::move(arf));
    CHECK(store.snapshot().recent_files.size() == 50,
          "recent_files dedup size still 50 got=%zu",
          store.snapshot().recent_files.size());
    CHECK(store.snapshot().recent_files.front().path == "/tmp/f50",
          "dedup — /tmp/f50 promoted to front got='%s'",
          store.snapshot().recent_files.front().path.c_str());

    // Revision monotonic.
    const auto final_rev = store.snapshot().revision;
    CHECK(final_rev >= 60, "revision monotonic final=%llu",
          (unsigned long long)final_rev);

    // Custom reducer: AddSessionMetadata.
    int custom_runs = 0;
    store.register_reducer(ActionType::AddSessionMetadata,
        [&](const AppStateData& in, const Action& /* a */) -> AppStateData {
            ++custom_runs;
            AppStateData out = in;
            SessionMeta sm;
            sm.id         = "sess-001";
            sm.title      = "first session";
            sm.turn_count = 3;
            sm.created_at = 1718000000;
            sm.updated_at = 1718000100;
            out.recent_sessions.push_back(std::move(sm));
            ++out.revision;
            return out;
        });
    Action asm_;
    asm_.type = ActionType::AddSessionMetadata;
    asm_.json_payload = R"({"id":"sess-001","title":"first session","turn_count":3})";
    store.dispatch(std::move(asm_));
    CHECK(custom_runs == 1, "custom reducer ran once got=%d", custom_runs);
    CHECK(store.snapshot().recent_sessions.size() == 1,
          "1 recent session added got=%zu",
          store.snapshot().recent_sessions.size());
    CHECK(store.snapshot().recent_sessions.front().id == "sess-001",
          "session id in metadata");
    CHECK(store.snapshot().recent_sessions.front().title == "first session",
          "session title in metadata");
    CHECK(store.snapshot().recent_sessions.front().turn_count == 3,
          "session turn_count in metadata");

    // Unsubscribe: capture count now, then dispatch a Tick; subscriber must
    // not be invoked further.
    const int calls_before_unsub = calls;
    CHECK(calls_before_unsub > 50, "many dispatches notified subscriber got=%d",
          calls_before_unsub);
    store.unsubscribe(sub);
    const int pre_tick = calls;
    Action ping{ActionType::Tick, "{}"};
    store.dispatch(std::move(ping));
    CHECK(calls == pre_tick, "after unsubscribe subscriber not called got %d->%d",
          pre_tick, calls);

    // ----- persistence -----
    std::puts("  -- persistence round-trip --");
    const std::string json_path = "/tmp/phase3_state.json";
    std::remove(json_path.c_str());
    bool saved = SaveToJson(json_path, store.snapshot());
    CHECK(saved, "SaveToJson returns true");
    std::error_code ec;
    CHECK(std::filesystem::exists(json_path, ec), "file present on disk");

    // Load into a brand new store with empty initial state.
    auto restored = LoadFromJson(json_path);
    CHECK(restored.has_value(), "LoadFromJson returns value");
    const auto& r = *restored;
    CHECK(r.user_id == "phase3-user-01", "user_id restored got='%s'", r.user_id.c_str());
    CHECK(r.settings.model_id == "claude-opus-4-8", "model_id restored got='%s'",
          r.settings.model_id.c_str());
    CHECK(r.settings.compact_mode == true, "compact_mode restored");
    CHECK(r.settings.max_tokens == 8192, "max_tokens restored");
    CHECK(std::abs(r.settings.temperature - 0.3) < 1e-6,
          "temperature restored got=%f", r.settings.temperature);
    CHECK(r.flags.proactive == true && r.flags.kairos == false,
          "flags restored");
    CHECK(r.recent_files.size() == 50, "recent_files size 50 restored got=%zu",
          r.recent_files.size());
    CHECK(r.recent_files.front().path == "/tmp/f50", "recent order restored");
    CHECK(r.recent_sessions.size() == 1, "recent_sessions restored got=%zu",
          r.recent_sessions.size());
    CHECK(r.active_session.has_value(), "active_session present after load");
    CHECK(r.active_session->draft_text == "multi\nline\ndraft",
          "draft_text restored");
    CHECK(r.revision == store.snapshot().revision,
          "revision preserved round-trip got exp=%llu actual=%llu",
          (unsigned long long)store.snapshot().revision,
          (unsigned long long)r.revision);

    // StateToJson / StateFromJson in-memory round-trip with empty input.
    auto empty_roundtrip = StateFromJson("");
    CHECK(!empty_roundtrip.has_value(), "empty json → nullopt");
    std::string re_encoded = StateToJson(r);
    auto re_decoded       = StateFromJson(re_encoded);
    CHECK(re_decoded.has_value(), "re-encoded json parses");
    CHECK(re_decoded->revision == r.revision, "re-decoded revision matches");

    // Check valid JSON structural properties: starts with '{', non-empty.
    CHECK(!re_encoded.empty() && re_encoded.front() == '{',
          "JSON output well-formed prefix");

    std::remove(json_path.c_str());

    if (g_fail == 0) {
        std::puts("STATE ALL PASSED");
        return 0;
    }
    std::fprintf(stderr, "STATE %d assertion(s) FAILED\n", g_fail);
    return 1;
}
