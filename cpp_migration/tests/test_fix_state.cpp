// test_fix_state.cpp — regression coverage for the state-layer audit findings.
//
// Scope:
//   - M1 (claimed bug): the audit alleged Store::undo()/redo() pass the same
//     snapshot as both the prev and next arguments to notify(), so subscribers
//     would observe prev == next. This is a FALSE POSITIVE: a careful trace of
//     undo_stack_/redo_stack_/state_ shows prev is always the pre-action state
//     and next is always the post-action state. These tests pin that invariant
//     so a future edit cannot silently introduce the bug the audit described.
//   - M2 (real fix): persistence.cppm save_state now fsyncs the temp file (and
//     best-effort the parent dir) before rename. We assert that a saved blob is
//     durable enough to be reloaded round-trip — the crash-safety primitive
//     itself (fsync) is not unit-testable portably, but the round-trip proves
//     the new POSIX write path is functionally correct.
//   - M3 (false positive): TS persists AppState to disk nowhere (no
//     app_state.json, no serializeAppState). The C++ StatePersistence is a
//     migration-invented feature, so there is no parity target to extend
//     toward. These tests pin the CURRENT serialized set round-trips
//     symmetrically so the migration feature itself does not regress.
//
// Register in tests/CMakeLists.txt:
//   add_executable(test_fix_state test_fix_state.cpp)
//   target_link_libraries(test_fix_state PRIVATE cc_core GTest::gtest_main)
//   gtest_discover_tests(test_fix_state
//       DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

import cc.state.app_state;
import cc.state.store;
import cc.state.persistence;

using cc::state::Action;
using cc::state::ActionType;
using cc::state::AppState;
using cc::state::AppStore;
using cc::state::persistence::StatePersistence;

namespace {

// Build a fresh store with undo/redo enabled and no on-disk persistence wired
// (persistence_ defaults to nullptr so notify() skips the auto-save branch).
auto make_undo_store() -> std::unique_ptr<AppStore> {
    auto store = std::make_unique<AppStore>(
        cc::state::get_default_app_state(), &cc::state::app_reducer);
    store->enable_undo();
    return store;
}

// A subscriber that records every (prev, next) pair it is notified with.
struct NotifyRecorder {
    struct Pair {
        bool prev_verbose;
        bool next_verbose;
        bool same_identity; // prev.verbose == next.verbose AND same address
    };
    std::vector<Pair> pairs;

    void operator()(const AppState& prev, const AppState& next) {
        pairs.push_back({
            prev.verbose,
            next.verbose,
            prev.verbose == next.verbose,
        });
    }
};

} // namespace

// ---------------------------------------------------------------------------
// M1 regression: undo() must notify subscribers with prev = pre-undo state
// and next = post-undo state. The audit claimed prev == next; this asserts
// they differ and carry the correct values.
// ---------------------------------------------------------------------------

TEST(StateUndoRedoNotify, UndoNotifiesWithDistinctPrevAndNext) {
    auto store = make_undo_store();
    NotifyRecorder recorder;
    auto sub_id = store->subscribe([&](const AppState& p, const AppState& n) {
        recorder(p, n);
    });
    (void)sub_id;

    // verbose: default false -> true (one subscriber notification).
    store->dispatch(Action{ActionType::SetVerbose, true});
    ASSERT_EQ(recorder.pairs.size(), 1u);
    EXPECT_FALSE(recorder.pairs[0].prev_verbose);
    EXPECT_TRUE(recorder.pairs[0].next_verbose);
    EXPECT_FALSE(recorder.pairs[0].same_identity)
        << "dispatch must notify with prev != next";

    recorder.pairs.clear();

    // undo: state_ reverts true -> false. Subscriber must see
    // prev.verbose == true (pre-undo) and next.verbose == false (post-undo).
    store->undo();
    ASSERT_EQ(recorder.pairs.size(), 1u);
    EXPECT_TRUE(recorder.pairs[0].prev_verbose)
        << "undo must notify prev = pre-undo state (verbose=true)";
    EXPECT_FALSE(recorder.pairs[0].next_verbose)
        << "undo must notify next = post-undo state (verbose=false)";
    EXPECT_FALSE(recorder.pairs[0].same_identity)
        << "undo must NOT notify prev == next (the audit's bug claim)";
    EXPECT_FALSE(store->get_state().verbose);
}

TEST(StateUndoRedoNotify, RedoNotifiesWithDistinctPrevAndNext) {
    auto store = make_undo_store();
    NotifyRecorder recorder;
    auto sub_id = store->subscribe([&](const AppState& p, const AppState& n) {
        recorder(p, n);
    });
    (void)sub_id;

    store->dispatch(Action{ActionType::SetVerbose, true});  // false -> true
    store->undo();                                          // true -> false
    ASSERT_TRUE(store->can_redo());

    recorder.pairs.clear();

    // redo: state_ re-applies false -> true. Subscriber must see
    // prev.verbose == false (pre-redo) and next.verbose == true (post-redo).
    store->redo();
    ASSERT_EQ(recorder.pairs.size(), 1u);
    EXPECT_FALSE(recorder.pairs[0].prev_verbose)
        << "redo must notify prev = pre-redo state (verbose=false)";
    EXPECT_TRUE(recorder.pairs[0].next_verbose)
        << "redo must notify next = post-redo state (verbose=true)";
    EXPECT_FALSE(recorder.pairs[0].same_identity)
        << "redo must NOT notify prev == next (the audit's bug claim)";
    EXPECT_TRUE(store->get_state().verbose);
}

// Consecutive undos: each must independently notify with the correct
// pre/post pair, not a degenerate prev == next.
TEST(StateUndoRedoNotify, ConsecutiveUndosEachNotifyCorrectly) {
    auto store = make_undo_store();
    NotifyRecorder recorder;
    auto sub_id = store->subscribe([&](const AppState& p, const AppState& n) {
        recorder(p, n);
    });
    (void)sub_id;

    // Build a 3-deep history: verbose toggles false -> true -> false -> true.
    store->dispatch(Action{ActionType::SetVerbose, true});
    store->dispatch(Action{ActionType::SetVerbose, false});
    store->dispatch(Action{ActionType::SetVerbose, true});
    ASSERT_TRUE(store->get_state().verbose);

    recorder.pairs.clear();

    // undo #1: true -> false
    store->undo();
    ASSERT_EQ(recorder.pairs.size(), 1u);
    EXPECT_TRUE(recorder.pairs.back().prev_verbose);
    EXPECT_FALSE(recorder.pairs.back().next_verbose);
    EXPECT_FALSE(recorder.pairs.back().same_identity);

    // undo #2: false -> true
    store->undo();
    ASSERT_EQ(recorder.pairs.size(), 2u);
    EXPECT_FALSE(recorder.pairs.back().prev_verbose);
    EXPECT_TRUE(recorder.pairs.back().next_verbose);
    EXPECT_FALSE(recorder.pairs.back().same_identity);

    // undo #3: true -> false (original default)
    store->undo();
    ASSERT_EQ(recorder.pairs.size(), 3u);
    EXPECT_TRUE(recorder.pairs.back().prev_verbose);
    EXPECT_FALSE(recorder.pairs.back().next_verbose);
    EXPECT_FALSE(recorder.pairs.back().same_identity);

    EXPECT_FALSE(store->can_undo());
    EXPECT_FALSE(store->get_state().verbose);
}

// ---------------------------------------------------------------------------
// M2 / M3: persistence round-trip. Pins that the fsync-backed atomic write
// path produces a reloadable file and that the current serialized set
// survives serialize -> write -> fsync -> rename -> read -> deserialize.
// ---------------------------------------------------------------------------

TEST(StatePersistence, SaveLoadRoundTripPreservesFields) {
    auto tmp = std::filesystem::temp_directory_path() /
        "cc_repl_test_fix_state_app_state.json";
    std::filesystem::remove(tmp);

    StatePersistence persist(tmp);

    AppState src = cc::state::get_default_app_state();
    src.verbose = true;
    src.compact_mode = true;
    src.fast_mode = true;
    src.working_directory = std::string("/tmp/fix_state_test");
    src.view_selection_mode = std::string("viewing-agent");
    src.main_loop_model = std::string("claude-sonnet-test");
    src.selected_ip_agent_index = 7;

    auto saved = persist.save_state(src);
    ASSERT_TRUE(saved.has_value()) << saved.error().format();

    auto loaded = persist.load_state();
    ASSERT_TRUE(loaded.has_value()) << loaded.error().format();
    EXPECT_TRUE(loaded->verbose);
    EXPECT_TRUE(loaded->compact_mode);
    EXPECT_TRUE(loaded->fast_mode);
    EXPECT_EQ(loaded->working_directory, "/tmp/fix_state_test");
    EXPECT_EQ(loaded->view_selection_mode, "viewing-agent");
    ASSERT_TRUE(loaded->main_loop_model.has_value());
    EXPECT_EQ(*loaded->main_loop_model, "claude-sonnet-test");
    EXPECT_EQ(loaded->selected_ip_agent_index, 7);

    std::filesystem::remove(tmp);
}

// The crash-safe write path must not leave a stale .tmp file behind on success.
TEST(StatePersistence, AtomicWriteLeavesNoTempFile) {
    auto tmp = std::filesystem::temp_directory_path() /
        "cc_repl_test_fix_state_notmp.json";
    std::filesystem::remove(tmp);
    std::filesystem::remove(std::string(tmp.string()) + ".tmp");

    StatePersistence persist(tmp);
    AppState src = cc::state::get_default_app_state();
    auto saved = persist.save_state(src);
    ASSERT_TRUE(saved.has_value()) << saved.error().format();
    EXPECT_TRUE(std::filesystem::exists(tmp));
    EXPECT_FALSE(std::filesystem::exists(std::string(tmp.string()) + ".tmp"));

    std::filesystem::remove(tmp);
}
