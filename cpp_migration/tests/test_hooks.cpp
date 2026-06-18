/// @file test_hooks.cpp
/// @brief Hook module smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

import cc.hooks.command_queue;
import cc.hooks.context;
import cc.hooks.ide_at_mentioned;
import cc.hooks.remaining_notifs;
import cc.hooks.terminal_size;
import cc.hooks.text_input;
import cc.hooks.typeahead;
import cc.hooks.virtual_scroll;
import cc.utils.hooks_execution;
import cc.utils.json;
import cc.utils.hooks_registry;

using namespace std::chrono_literals;

// Convenience alias for the 8 notification-hook smoke tests below.
namespace notif = cc::hooks::notifs;

// The P0-03 hooks-engine tests below use the unqualified names exported from
// cc::utils::hooks_execution (evaluate_hook_condition, CommandHookRunner,
// HttpHookRunner, PromptHookRunner, HookExecutionContext, ...).
using cc::utils::hooks_execution::CommandHookRunner;
using cc::utils::hooks_execution::HookExecutionContext;
using cc::utils::hooks_execution::HttpHookRunner;
using cc::utils::hooks_execution::PromptHookRunner;
using cc::utils::hooks_execution::evaluate_hook_condition;
using cc::utils::hooks_execution::AgentHookRunner;
using cc::utils::hooks_execution::HookResponseAction;
using namespace cc::utils::hooks_registry;  // IndividualHookConfig / CommandHookConfig / HookEventType / HookSource

// Build a HookExecutionContext with the given dotted-path -> string context
// vars. The hooks engine resolves "tool.name" by walking nested objects
// (ctx["tool"]["name"]), so we materialise a nested JSON document and copy its
// top-level children into context_vars.
// NOTE: We must keep the JsonDoc alive for the lifetime of the context, since
// JsonVal is a non-owning view into the document.
[[nodiscard]] inline HookExecutionContext ctx_with_vars(
    std::initializer_list<std::pair<std::string_view, std::string_view>> vars) {
    HookExecutionContext ctx;
    auto esc = [](std::string_view s) {
        std::string out;
        for (char ch : s) {
            if (ch == '"' || ch == '\\') out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    };
    std::string json = "{";
    bool first = true;
    for (const auto& [k, v] : vars) {
        if (!first) json += ",";
        first = false;
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : k) {
            if (ch == '.') { if (!cur.empty()) parts.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(ch);
        }
        if (!cur.empty()) parts.push_back(std::move(cur));
        if (parts.empty()) continue;
        json += "\"" + esc(parts[0]) + "\":";
        for (std::size_t i = 1; i < parts.size(); ++i) json += "{\"" + esc(parts[i]) + "\":";
        json += "\"" + esc(v) + "\"";
        for (std::size_t i = 1; i < parts.size(); ++i) json += "}";
    }
    json += "}";
    auto parsed = cc::utils::json::parse(json);
    if (parsed) {
        // Store the doc so JsonVal views remain valid for ctx lifetime.
        ctx.set_payload_doc(std::move(*parsed));
        // Now copy top-level fields from the payload into context_vars.
        // (We re-read from ctx.hook_payload which points to the owned doc.)
        if (ctx.hook_payload && ctx.hook_payload->is_obj()) {
            ctx.hook_payload->iter_obj([&](cc::utils::json::JsonVal kk, cc::utils::json::JsonVal vv) {
                ctx.context_vars[std::string(kk.as_str())] = vv;
            });
        }
        // Clear the payload since it was just a carrier for context_vars.
        ctx.hook_payload.reset();
        // NOTE: The document is still kept alive in owned_docs (shared_ptr),
        // so the context_vars JsonVal views remain valid.
    }
    return ctx;
}

// RAII guard that clears all per-hook dismissal state on scope exit so tests
// are independent regardless of execution order.
struct NotifStateReset {
    NotifStateReset() = default;
    ~NotifStateReset() { cc::hooks::notifs::reset_dismissals_for_tests(); }
    NotifStateReset(const NotifStateReset&) = delete;
    NotifStateReset& operator=(const NotifStateReset&) = delete;
};

TEST(TypeaheadHook, ComputesAndAcceptsProviderSuggestions) {
    cc::hooks::TypeaheadHook hook(/*debounce_ms=*/0, /*max_suggestions=*/3);
    hook.add_source(cc::hooks::CompletionSource::Commands, [](std::string_view) {
        return std::vector<cc::hooks::CompletionItem>{
            {.label = "/help", .detail = "show help", .insert_text = "/help", .score = 0.0f, .source = cc::hooks::CompletionSource::Commands, .icon = std::nullopt},
            {.label = "/hooks", .detail = "list hooks", .insert_text = "/hooks", .score = 0.0f, .source = cc::hooks::CompletionSource::Commands, .icon = std::nullopt},
            {.label = "/model", .detail = "switch model", .insert_text = "/model", .score = 0.0f, .source = cc::hooks::CompletionSource::Commands, .icon = std::nullopt},
        };
    });

    hook.update_input("/h", 2);

    ASSERT_TRUE(hook.is_visible());
    ASSERT_GE(hook.get_suggestions().size(), 2u);
    EXPECT_EQ(hook.get_suggestions().front().label, "/help");

    auto accepted = hook.accept_suggestion(0);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(*accepted, "/help");
    EXPECT_FALSE(hook.is_visible());
}

TEST(TypeaheadHook, CyclesSelectionAndExposesGhostText) {
    cc::hooks::TypeaheadHook hook(0);
    hook.add_source(cc::hooks::CompletionSource::Custom, [](std::string_view) {
        return std::vector<cc::hooks::CompletionItem>{
            {.label = "commit", .detail = "git commit", .insert_text = "commit", .score = 0.0f, .source = cc::hooks::CompletionSource::Custom, .icon = std::nullopt},
            {.label = "compact", .detail = "compact context", .insert_text = "compact", .score = 0.0f, .source = cc::hooks::CompletionSource::Custom, .icon = std::nullopt},
        };
    });

    hook.update_input("com", 3);
    hook.cycle_next();

    ASSERT_TRUE(hook.state().selected_index.has_value());
    EXPECT_EQ(*hook.state().selected_index, 0u);
    auto ghost = hook.get_ghost_text();
    ASSERT_TRUE(ghost.has_value());
    EXPECT_FALSE(ghost->empty());

    hook.cycle_prev();
    ASSERT_TRUE(hook.state().selected_index.has_value());
    EXPECT_EQ(*hook.state().selected_index, hook.get_suggestions().size() - 1);
}

TEST(TextInputHook, HandlesInsertionCursorMovementAndBackspace) {
    cc::hooks::TextInputHook input;
    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "H"}));
    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "i"}));
    EXPECT_EQ(input.text(), "Hi");
    EXPECT_EQ(input.cursor().col, 2u);

    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "Left"}));
    EXPECT_EQ(input.cursor().col, 1u);

    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "Backspace"}));
    EXPECT_EQ(input.text(), "i");
    EXPECT_EQ(input.cursor().col, 0u);
}

TEST(TextInputHook, SupportsUndoRedoAndSelection) {
    cc::hooks::TextInputHook input;
    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "a"}));
    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "b"}));
    EXPECT_EQ(input.text(), "ab");

    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "z", .ctrl = true}));
    EXPECT_EQ(input.text(), "a");

    EXPECT_TRUE(input.handle_key(cc::hooks::KeyEvent{.key = "y", .ctrl = true}));
    EXPECT_EQ(input.text(), "ab");

    input.select_all();
    EXPECT_TRUE(input.has_selection());
    EXPECT_EQ(input.get_selected_text(), "ab");
}

TEST(CommandQueue, ProcessesCommandsByPriorityAndReportsCompletion) {
    cc::hooks::CommandQueue queue;
    std::vector<std::string> completed;
    queue.on_command_complete([&completed](const cc::hooks::CommandCompleteEvent& event) {
        if (event.success) completed.push_back(event.id);
    });

    auto low = queue.enqueue("low", cc::hooks::QueuePriority::low);
    auto high = queue.enqueue("high", cc::hooks::QueuePriority::high);
    ASSERT_EQ(queue.get_pending_count(), 2u);

    std::string first_processed;
    auto result = queue.process_next([&first_processed](const cc::hooks::QueuedCommand& cmd) {
        first_processed = cmd.command_text;
        return std::expected<void, std::string>{};
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(first_processed, "high");
    EXPECT_EQ(queue.state().completed_count, 1u);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed.front(), high);
    EXPECT_NE(low, high);
}

TEST(CommandQueue, DeduplicatesAndCancelsPendingCommands) {
    cc::hooks::CommandQueue queue;
    queue.set_dedup_window(std::chrono::seconds(5));
    auto first = queue.enqueue("same");
    auto duplicate = queue.enqueue("same");
    EXPECT_NE(first, duplicate);
    EXPECT_EQ(queue.get_pending_count(), 1u);

    EXPECT_TRUE(queue.cancel(first));
    EXPECT_EQ(queue.state().cancelled_count, 1u);
    EXPECT_TRUE(queue.is_idle());
}

TEST(VirtualScrollHook, ComputesVisibleRangeAndBottomState) {
    cc::hooks::VirtualScrollHook scroll(/*viewport_height=*/5);
    scroll.set_overscan(0);
    scroll.add_items(10, 1);

    auto [start, end] = scroll.get_visible_range();
    EXPECT_EQ(start, 5u);
    EXPECT_EQ(end, 10u);
    EXPECT_TRUE(scroll.is_at_bottom());

    scroll.scroll_by(-3);
    auto detailed = scroll.get_visible_range_detailed();
    EXPECT_EQ(detailed.start, 2u);
    EXPECT_EQ(detailed.end, 7u);
    EXPECT_FALSE(scroll.is_at_bottom());
}

TEST(VirtualScrollHook, TracksDynamicItemHeightsAndScrollbar) {
    cc::hooks::VirtualScrollHook scroll(4);
    scroll.set_overscan(0);
    scroll.add_items(3, 1);
    scroll.update_item_height(1, 5);

    EXPECT_EQ(scroll.total_content_height(), 7u);
    EXPECT_LT(scroll.scrollbar_thumb_size(), 1.0f);

    auto consumed = scroll.handle_scroll_event(cc::hooks::ScrollEvent{.type = cc::hooks::ScrollEvent::Type::Home, .delta = 0, .thumb_pos = std::nullopt});
    EXPECT_TRUE(consumed);
    EXPECT_EQ(scroll.scrollbar_position(), 0.0f);
}

TEST(TerminalSizeHook, ReportsDefaultSizeAndMonitoringState) {
    cc::hooks::TerminalSizeHook hook;
    auto dims = hook.get_size();
    EXPECT_EQ(dims.cols, 80u);
    EXPECT_EQ(dims.rows, 24u);
    EXPECT_EQ(dims.area(), 1920u);
    EXPECT_FALSE(hook.is_too_small());

    hook.start_monitoring();
    EXPECT_TRUE(hook.is_monitoring());
    hook.stop_monitoring();
    EXPECT_FALSE(hook.is_monitoring());
}

TEST(ContextBudget, ComputesAvailableTokensCompressionAndUtilization) {
    cc::hooks::ContextBudget budget{
        .max_tokens = 1000,
        .reserved_for_output = 100,
        .system_prompt_tokens = 400,
        .conversation_tokens = 300,
    };

    EXPECT_EQ(budget.available(), 200u);
    EXPECT_FALSE(budget.needs_compression());
    EXPECT_EQ(budget.utilization_percent(), 70);

    budget.conversation_tokens = 500;
    EXPECT_TRUE(budget.needs_compression());
    EXPECT_EQ(budget.available(), 0u);
}

namespace {

// RAII temp directory with a sample file inside, for filesystem resolution tests.
struct TempWorkspace {
    std::filesystem::path root;
    std::filesystem::path file;
    explicit TempWorkspace(std::string_view rel = "src/sample.cpp") {
        auto tmpl = std::filesystem::temp_directory_path() / "cc_at_mention_XXXXXX";
        root = std::string(tmpl);
        // mkdtemp-style unique dir via PID + a counter-safe suffix is overkill here;
        // use a fixed unique-ish path and remove first.
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / std::filesystem::path(std::string(rel)).parent_path());
        file = root / std::string(rel);
        std::ofstream out(file);
        out << "// sample\nint main() { return 0; }\n";
        out.close();
    }
    ~TempWorkspace() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;
};

} // namespace

TEST(AtMentionParse, ClassifiesFilesAndSymbols) {
    auto mentions = cc::hooks::parse_at_mentions("look at @src/foo.cpp and @Bar please");
    ASSERT_EQ(mentions.size(), 2u);
    EXPECT_EQ(mentions[0].type, "file");
    EXPECT_EQ(mentions[0].value, "src/foo.cpp");
    EXPECT_EQ(mentions[1].type, "symbol");
    EXPECT_EQ(mentions[1].value, "Bar");
    // Offsets point at the leading '@' and the char after the token.
    EXPECT_EQ(mentions[0].start, 8);
    EXPECT_GT(mentions[0].end, mentions[0].start);
}

TEST(AtMentionParse, ExtractsLineAnchorSingleAndRange) {
    auto single = cc::hooks::parse_at_mentions("@a/b.cpp#L12");
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single[0].value, "a/b.cpp");
    ASSERT_TRUE(single[0].line_start.has_value());
    EXPECT_EQ(*single[0].line_start, 12);
    EXPECT_FALSE(single[0].line_end.has_value());

    auto range = cc::hooks::parse_at_mentions("@a/b.cpp#L12-30");
    ASSERT_EQ(range.size(), 1u);
    EXPECT_EQ(range[0].value, "a/b.cpp");
    ASSERT_TRUE(range[0].line_start.has_value());
    EXPECT_EQ(*range[0].line_start, 12);
    ASSERT_TRUE(range[0].line_end.has_value());
    EXPECT_EQ(*range[0].line_end, 30);
}

TEST(AtMentionParse, IgnoresAnchorWithoutDigits) {
    // A "#" that is not a valid "#L<num>" anchor is kept inside the value.
    auto weird = cc::hooks::parse_at_mentions("@a/b.cpp#fragment");
    ASSERT_EQ(weird.size(), 1u);
    EXPECT_EQ(weird[0].value, "a/b.cpp#fragment");
    EXPECT_FALSE(weird[0].line_start.has_value());
}

TEST(AtMentionResolve, ResolvesRelativeFileAgainstWorkspaceRoot) {
    TempWorkspace ws;
    auto mentions = cc::hooks::parse_at_mentions("@src/sample.cpp");
    ASSERT_EQ(mentions.size(), 1u);
    auto resolved = cc::hooks::resolve_at_mention(mentions[0], ws.root.string());
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(ws.file, ec);
    EXPECT_EQ(*resolved, canonical.string());
}

TEST(AtMentionResolve, ResolvesAbsolutePath) {
    TempWorkspace ws;
    auto mentions = cc::hooks::parse_at_mentions("@" + ws.file.string());
    ASSERT_EQ(mentions.size(), 1u);
    auto resolved = cc::hooks::resolve_at_mention(mentions[0], "/some/other/root");
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(ws.file, ec);
    EXPECT_EQ(*resolved, canonical.string());
}

TEST(AtMentionResolve, NormalisesDotDot) {
    TempWorkspace ws;
    // "../src/sample.cpp" relative to ws.root/src resolves back to ws.file.
    auto rel = (ws.root / "src" / ".." / "src" / "sample.cpp").string();
    auto mentions = cc::hooks::parse_at_mentions("@" + rel);
    ASSERT_EQ(mentions.size(), 1u);
    auto resolved = cc::hooks::resolve_at_mention(mentions[0], ws.root.string());
    ASSERT_TRUE(resolved.has_value()) << resolved.error();
    EXPECT_EQ(*resolved, std::filesystem::weakly_canonical(ws.file).string());
}

TEST(AtMentionResolve, FailsOnMissingFile) {
    auto mentions = cc::hooks::parse_at_mentions("@nope/missing.cpp");
    ASSERT_EQ(mentions.size(), 1u);
    auto resolved = cc::hooks::resolve_at_mention(mentions[0], "/tmp");
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("does not exist"), std::string::npos);
}

TEST(AtMentionResolve, FailsOnSymbolMention) {
    cc::hooks::AtMention sym{.type = "symbol", .value = "Foo", .start = 0, .end = 0};
    auto resolved = cc::hooks::resolve_at_mention(sym, "/tmp");
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("Symbol resolution"), std::string::npos);
}

TEST(AtMentionResolve, FailsOnRelativeWithoutWorkspaceRoot) {
    TempWorkspace ws;
    auto mentions = cc::hooks::parse_at_mentions("@src/sample.cpp");
    ASSERT_EQ(mentions.size(), 1u);
    auto resolved = cc::hooks::resolve_at_mention(mentions[0], "");
    ASSERT_FALSE(resolved.has_value());
    EXPECT_NE(resolved.error().find("workspace root"), std::string::npos);
}

// ─── P1-03: 8 notification hooks smoke tests ────────────────────────────────
TEST(NotifHooks, NpmDeprecationReturnsWhenSet) {
    NotifStateReset guard;
    const int64_t future = notif::detail::now_ms() + 60 * 60 * 1000;
    notif::set_npm_deprecation_data(notif::NpmDeprecationInfo{
        .id = "npm-cc-core-2026",
        .message = "cc-core@1.2 is deprecated, upgrade to 2.0",
        .deadline_ms = future,
        .package_name = "cc-core",
        .deprecated_version = "1.2.0",
        .recommended_version = "2.0.0",
    });
    auto got = notif::check_npm_deprecation();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, "npm-cc-core-2026");
    EXPECT_EQ(got->recommended_version, "2.0.0");
}

TEST(NotifHooks, DismissedNpmDeprecationHidden) {
    NotifStateReset guard;
    const int64_t future = notif::detail::now_ms() + 60 * 60 * 1000;
    notif::set_npm_deprecation_data(notif::NpmDeprecationInfo{
        .id = "npm-dismiss-me", .deadline_ms = future,
        .message = "x", .package_name = "p", .deprecated_version = "1", .recommended_version = "2"});
    notif::acknowledge_notification("npm_deprecation", "npm-dismiss-me");
    EXPECT_FALSE(notif::check_npm_deprecation().has_value());
}

TEST(NotifHooks, DeadlineExpiredHidden) {
    NotifStateReset guard;
    const int64_t past = notif::detail::now_ms() - 1000;
    notif::set_npm_deprecation_data(notif::NpmDeprecationInfo{
        .id = "npm-expired", .deadline_ms = past,
        .message = "x", .package_name = "p", .deprecated_version = "1", .recommended_version = "2"});
    EXPECT_FALSE(notif::check_npm_deprecation().has_value());
}

TEST(NotifHooks, ModelMigrationFiltersByCurrent) {
    NotifStateReset guard;
    notif::set_current_model("sonnet45");
    notif::set_all_model_migrations({
        {.from_model = "sonnet45", .to_model = "opus", .reason = "performance",
         .deadline_ms = notif::detail::now_ms() + 86400000, .auto_migrated = false},
        {.from_model = "sonnet45", .to_model = "sonnet46", .reason = "upgrade",
         .deadline_ms = notif::detail::now_ms() + 86400000, .auto_migrated = false},
        {.from_model = "haiku", .to_model = "sonnet45", .reason = "better",
         .deadline_ms = notif::detail::now_ms() + 86400000, .auto_migrated = false},
    });
    auto pending = notif::get_pending_model_migrations();
    // haiku->sonnet45 does not match the current model "sonnet45" as `from`,
    // so we expect 2 matches.
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0].to_model, "opus");
    EXPECT_EQ(pending[1].to_model, "sonnet46");

    // Dismiss one and verify it's filtered out.
    notif::acknowledge_migration("sonnet45", "opus");
    auto again = notif::get_pending_model_migrations();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again.front().to_model, "sonnet46");
}

TEST(NotifHooks, PluginAutoupdateEmptyByDefault) {
    NotifStateReset guard;
    EXPECT_TRUE(notif::get_plugin_updates().empty());
}

TEST(NotifHooks, InstallationStatusFullTriplet) {
    NotifStateReset guard;
    notif::PluginInstallationStatusSnapshot snap{
        .queued = {{"q1", "QueuedPlugin"}, {"q2", "AnotherQueued"}},
        .installing = notif::InProgressInstall{
            .plugin_id = "inst-1", .name = "InstallingPlugin", .progress_pct = 42},
        .failed = {{.plugin_id = "fail-1", .name = "BrokenPlugin",
                    .error = "network timeout while fetching tarball"}},
    };
    notif::set_plugin_installation_snapshot(snap);
    auto all = notif::get_plugin_installation_status();
    ASSERT_EQ(all.size(), 4u);

    // Order: queued first, then installing, then failed.
    EXPECT_EQ(all[0].plugin_id, "q1");
    EXPECT_EQ(all[0].status, notif::PluginInstallStatus::Pending);
    EXPECT_EQ(all[1].plugin_id, "q2");
    EXPECT_EQ(all[1].status, notif::PluginInstallStatus::Pending);
    EXPECT_EQ(all[2].plugin_id, "inst-1");
    EXPECT_EQ(all[2].status, notif::PluginInstallStatus::Installing);
    EXPECT_EQ(all[2].progress_pct, 42);
    EXPECT_EQ(all[3].plugin_id, "fail-1");
    EXPECT_EQ(all[3].status, notif::PluginInstallStatus::Failed);
    ASSERT_TRUE(all[3].error.has_value());
    EXPECT_NE(all[3].error->find("network timeout"), std::string::npos);
}

TEST(NotifHooks, McpConnectivityIssueDetected) {
    NotifStateReset guard;
    notif::set_raw_mcp_connectivity({
        {.server_id = "mcp-ok", .display_name = "Works Fine",
         .state = notif::McpServerStatus::Connected,
         .last_error = std::nullopt,
         .last_seen_ms = notif::detail::now_ms()},
        {.server_id = "mcp-bad", .display_name = "Broken Server",
         .state = notif::McpServerStatus::Error,
         .last_error = "connection refused",
         .last_seen_ms = notif::detail::now_ms() - 15000},
    });
    EXPECT_TRUE(notif::has_mcp_connectivity_issues());
    auto list = notif::get_mcp_connectivity_status();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list.front().state, notif::McpServerStatus::Connected);
    EXPECT_EQ(list.back().state, notif::McpServerStatus::Error);
    ASSERT_TRUE(list.back().last_error.has_value());
    EXPECT_EQ(*list.back().last_error, "connection refused");
}

TEST(NotifHooks, SettingsErrorsSeverity) {
    NotifStateReset guard;
    notif::set_raw_settings_errors({
        {.key_path = "api.base_url",
         .severity = notif::SettingsSeverity::Warning,
         .message = "base URL contains a trailing slash",
         .suggested_fix_opt = "remove the trailing '/' character",
         .human_field_label = "API Base URL"},
        {.key_path = "auth.api_key",
         .severity = notif::SettingsSeverity::Error,
         .message = "API key has invalid format (expected sk-ant-...)",
         .suggested_fix_opt = std::nullopt,
         .human_field_label = "API Key"},
    });
    EXPECT_TRUE(notif::has_settings_errors());
    auto errs = notif::get_settings_errors();
    ASSERT_EQ(errs.size(), 2u);
    EXPECT_EQ(errs[0].severity, notif::SettingsSeverity::Warning);
    EXPECT_EQ(errs[1].severity, notif::SettingsSeverity::Error);
    EXPECT_TRUE(errs[0].suggested_fix_opt.has_value());
    EXPECT_FALSE(errs[1].suggested_fix_opt.has_value());

    // Dismiss one.
    notif::dismiss_settings_error("auth.api_key");
    auto after = notif::get_settings_errors();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after.front().key_path, "api.base_url");
}

TEST(NotifHooks, TeammateShutdownRecentOnly) {
    NotifStateReset guard;
    const int64_t now = notif::detail::now_ms();
    notif::set_all_teammate_shutdowns({
        {.agent_id = "old-agent", .display_name = "Agent Long Gone",
         .went_down_ms = now - 10 * 60 * 1000,   // 10 minutes ago
         .cause = notif::TeammateShutdownCause::Finished},
        {.agent_id = "fresh-agent", .display_name = "Just Crashed",
         .went_down_ms = now - 60 * 1000,         // 1 minute ago
         .cause = notif::TeammateShutdownCause::Crashed},
    });
    auto recent = notif::get_teammate_shutdowns(/*within=*/5min);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent.front().agent_id, "fresh-agent");
    EXPECT_EQ(recent.front().cause, notif::TeammateShutdownCause::Crashed);

    // Acknowledge the recent one; subsequent query returns nothing.
    notif::acknowledge_teammate_shutdown("fresh-agent");
    auto again = notif::get_teammate_shutdowns(5min);
    EXPECT_TRUE(again.empty());
}

TEST(NotifHooks, SubscriptionSwitchAllFieldsFilled) {
    NotifStateReset guard;
    notif::set_subscription_switch_data(notif::SubscriptionSwitch{
        .can_switch = true,
        .offer_tier = "tier1",
        .monthly_cost_usd_cents = 2000,
        .saving_pct_over_current = 33,
        .trial_days_remaining = 14,
    });
    auto offer = notif::can_switch_to_existing_subscription();
    ASSERT_TRUE(offer.has_value());
    EXPECT_TRUE(offer->can_switch);
    EXPECT_EQ(offer->offer_tier, "tier1");
    EXPECT_EQ(offer->monthly_cost_usd_cents, 2000);
    EXPECT_EQ(offer->saving_pct_over_current, 33);
    EXPECT_EQ(offer->trial_days_remaining, 14);

    // Dismiss per-tier.
    notif::acknowledge_subscription_switch("tier1");
    auto after = notif::can_switch_to_existing_subscription();
    EXPECT_FALSE(after.has_value());

    // Sanity: can_switch=false → nullopt regardless of other fields.
    notif::set_subscription_switch_data(notif::SubscriptionSwitch{
        .can_switch = false, .offer_tier = "tier2", .monthly_cost_usd_cents = 100,
        .saving_pct_over_current = 0, .trial_days_remaining = 0});
    EXPECT_FALSE(notif::can_switch_to_existing_subscription().has_value());
}

// ─── P0-03: hooks execution engine tests ─────────────────────────────────────
TEST(Hooks, ConditionEvaluation) {
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
    });
    auto r = evaluate_hook_condition("tool.name == bash", ctx);
    ASSERT_TRUE(r.has_value()) << "error: " << r.error();
    EXPECT_TRUE(*r);

    auto r2 = evaluate_hook_condition("tool.name == file_read", ctx);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(*r2);

    // Quoted right-hand side.
    auto r3 = evaluate_hook_condition(R"(tool.name == "bash")", ctx);
    ASSERT_TRUE(r3.has_value());
    EXPECT_TRUE(*r3);
}

TEST(Hooks, ConditionLogicalAnd) {
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
        {"session.is_admin", "true"},
    });
    auto r = evaluate_hook_condition(
        "tool.name == bash && session.is_admin == true", ctx);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);

    // Without admin.
    auto ctx_noadmin = ctx_with_vars({
        {"tool.name", "bash"},
        {"session.is_admin", "false"},
    });
    auto r2 = evaluate_hook_condition(
        "tool.name == bash && session.is_admin == true", ctx_noadmin);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(*r2);
}

TEST(Hooks, ConditionNegation) {
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
    });
    auto r = evaluate_hook_condition("!tool.name == dangerous", ctx);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(*r);

    // Double negation.
    auto r2 = evaluate_hook_condition("!tool.name == bash", ctx);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(*r2);

    // Not-equal operator.
    auto r3 = evaluate_hook_condition("tool.name != dangerous", ctx);
    ASSERT_TRUE(r3.has_value());
    EXPECT_TRUE(*r3);
}

TEST(Hooks, ConditionInOperator) {
    auto ctx = ctx_with_vars({
        {"env", "staging"},
    });
    auto r = evaluate_hook_condition("env in prod,staging,dev", ctx);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r);

    auto ctx_local = ctx_with_vars({
        {"env", "local"},
    });
    auto r2 = evaluate_hook_condition("env in prod,staging,dev", ctx_local);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(*r2);

    // Quoted values.
    auto r3 = evaluate_hook_condition(R"(env in "prod","staging","dev")", ctx);
    ASSERT_TRUE(r3.has_value());
    EXPECT_TRUE(*r3);
}

TEST(Hooks, CommandRunsEcho) {
    auto r = CommandHookRunner::run_raw("/bin/echo", {"hello world"});
    EXPECT_TRUE(r.executed);
    EXPECT_EQ(r.exit_code, 0) << "stderr: " << r.stderr << ", err: " << r.error;
    // stdout should contain "hello world" (echo adds newline).
    EXPECT_NE(r.stdout.find("hello world"), std::string::npos)
        << "actual stdout: [" << r.stdout << "]";
}

TEST(Hooks, CommandTimeoutKills) {
    auto before = std::chrono::steady_clock::now();
    auto r = CommandHookRunner::run_raw("/bin/sleep", {"10"}, 150);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - before)
                       .count();
    EXPECT_FALSE(r.error.empty()); // should have timeout error.
    EXPECT_NE(r.exit_code, 0)
        << "exit_code=" << r.exit_code << " elapsed=" << r.elapsed.count() << "ms";
    // Should be close to 150ms (definitely well below 2000ms from SIGKILL wait plus margin).
    EXPECT_LT(elapsed, 3000) << "process should have been killed by timeout";
    EXPECT_LT(r.elapsed.count(), 3000);
}

TEST(Hooks, HttpBlocksSSRF) {
    auto r = HttpHookRunner::run("http://192.168.0.1/admin", "GET");
    EXPECT_FALSE(r.error.empty());
    std::string lower;
    for (char c : r.error) lower.push_back(std::tolower(static_cast<unsigned char>(c)));
    EXPECT_TRUE(lower.find("ssrf") != std::string::npos ||
                lower.find("blocked") != std::string::npos ||
                lower.find("private") != std::string::npos)
        << "expected SSRF/blocked in error, got: " << r.error;
    EXPECT_FALSE(r.executed && r.exit_code == 0);
}

TEST(Hooks, PromptExpandsVariables) {
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
        {"user.role", "admin"},
    });
    std::string tmpl = "Tool ${tool.name} called by ${user.role}";
    auto s = PromptHookRunner::run(tmpl, ctx, 4000);
    EXPECT_NE(s.find("Tool bash called by admin"), std::string::npos)
        << "got: " << s;

    // Unknown variable preserved.
    std::string tmpl2 = "Value is ${unknown.var} end";
    auto s2 = PromptHookRunner::run(tmpl2, ctx, 4000);
    EXPECT_NE(s2.find("${unknown.var}"), std::string::npos)
        << "got: " << s2;
}

TEST(Hooks, AgentBlocksNested) {
    HookExecutionContext ctx;
    ctx.tool_runner_delegate = [](std::string_view) -> std::expected<std::string, std::string> {
        return std::string("ok");
    };
    // Subprompt containing the forbidden "execAgentHook" string.
    auto r = AgentHookRunner::run("Please execAgentHook with args", ctx, 512);
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_FALSE(r.error.empty());
    std::string lower;
    for (char c : r.error) lower.push_back(std::tolower(static_cast<unsigned char>(c)));
    EXPECT_NE(lower.find("nested agent hooks are forbidden"), std::string::npos)
        << "got error: " << r.error;

    // Also checks run_agent_hook.
    auto r2 = AgentHookRunner::run("Run run_agent_hook now", ctx, 512);
    EXPECT_EQ(r2.exit_code, 1);
    EXPECT_FALSE(r2.error.empty());
    std::string lower2;
    for (char c : r2.error) lower2.push_back(std::tolower(static_cast<unsigned char>(c)));
    EXPECT_NE(lower2.find("nested agent hooks are forbidden"), std::string::npos);

    // Safe prompt: works via delegate.
    auto r3 = AgentHookRunner::run("Do something safe", ctx, 512);
    EXPECT_EQ(r3.exit_code, 0) << "err: " << r3.error;
    EXPECT_EQ(r3.stdout, "ok");
}

TEST(Hooks, FilterByMatcher) {
    IndividualHookConfig cfg_match;
    cfg_match.event = HookEventType::PreToolUse;
    cfg_match.matcher = "tool.name == bash";
    cfg_match.config = CommandHookConfig{.command = "echo match"};
    cfg_match.source = HookSource::UserSettings;

    IndividualHookConfig cfg_miss;
    cfg_miss.event = HookEventType::PreToolUse;
    cfg_miss.matcher = "tool.name == file_write";
    cfg_miss.config = CommandHookConfig{.command = "echo miss"};
    cfg_miss.source = HookSource::UserSettings;

    std::vector<IndividualHookConfig> all = {cfg_match, cfg_miss};
    auto ctx = ctx_with_vars({{"tool.name", "bash"}});
    auto filtered = filter_hooks_by_matcher(all, ctx);
    ASSERT_EQ(filtered.size(), 1u);
    // Get the command out.
    ASSERT_TRUE(std::holds_alternative<CommandHookConfig>(filtered[0].config));
    EXPECT_EQ(std::get<CommandHookConfig>(filtered[0].config).command, "echo match");
}

TEST(Hooks, ApiQueryPipelineAccumulatesPrompts) {
    IndividualHookConfig p1;
    p1.event = HookEventType::UserPromptSubmit;
    p1.config = PromptHookConfig{.prompt = "Hook A: tool=${tool.name}"};
    p1.source = HookSource::UserSettings;

    IndividualHookConfig p2;
    p2.event = HookEventType::UserPromptSubmit;
    p2.config = PromptHookConfig{.prompt = "Hook B: user=${user.role}"};
    p2.source = HookSource::UserSettings;

    std::vector<IndividualHookConfig> reg = {p1, p2};
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
        {"user.role", "admin"},
    });
    auto [modified, action] = run_api_query_hooks(
        reg, HookEventType::UserPromptSubmit, ctx, "original payload");
    EXPECT_EQ(action.action, HookResponseAction::Continue);
    EXPECT_NE(modified.find("Hook A: tool=bash"), std::string::npos) << modified;
    EXPECT_NE(modified.find("Hook B: user=admin"), std::string::npos) << modified;
    EXPECT_NE(modified.find("original payload"), std::string::npos);
}

TEST(Hooks, PostToolBlocksOnError) {
    // A shell hook that exits non-zero should produce a blocking/aborting action.
    IndividualHookConfig bad;
    bad.event = HookEventType::PostToolUse;
    bad.config = CommandHookConfig{.command = "exit 1"};
    bad.source = HookSource::UserSettings;
    std::vector<IndividualHookConfig> reg = {bad};
    auto ctx = ctx_with_vars({});
    auto action = execute_post_tool_hooks(reg, ctx, "bash", "{}", "output");
    EXPECT_TRUE(action.action == HookResponseAction::AbortQuery ||
                action.action == HookResponseAction::BlockToolCall)
        << "action was " << static_cast<int>(action.action);
}

// ===========================================================================
// End-to-end integration: multi-hook registries exercising the full
// filter → execute pipeline across event boundaries.  These tests favour
// prompt hooks and pure filter logic over spawning subprocesses, because the
// command-hook runner uses a poll loop that is intentionally slow under the
// project's timeout policy.
// ===========================================================================

namespace {

/// Build a prompt hook whose template interpolates a context variable.
IndividualHookConfig make_prompt_hook(HookEventType evt,
                                      std::string matcher,
                                      std::string prompt_tmpl) {
    IndividualHookConfig h;
    h.event = evt;
    h.matcher = std::move(matcher);
    h.config = PromptHookConfig{.prompt = std::move(prompt_tmpl)};
    h.source = HookSource::UserSettings;
    return h;
}

}  // namespace

TEST(HooksE2E, MatcherFiltersBeforePipelineRuns) {
    // A registry of three UserPromptSubmit hooks gated by different matchers.
    // Only the ones whose matcher holds against the current context should
    // survive filter_hooks_by_matcher; the pipeline then accumulates only
    // their prompts.
    auto h_match  = make_prompt_hook(HookEventType::UserPromptSubmit,
                                     "tool.name == bash", "match-prompt");
    auto h_miss   = make_prompt_hook(HookEventType::UserPromptSubmit,
                                     "tool.name == file_write", "miss-prompt");
    auto h_uncond = make_prompt_hook(HookEventType::UserPromptSubmit,
                                     "", "always-prompt");

    std::vector<IndividualHookConfig> reg = {h_match, h_miss, h_uncond};
    auto ctx = ctx_with_vars({{"tool.name", "bash"}});
    auto filtered = filter_hooks_by_matcher(reg, ctx);
    ASSERT_EQ(filtered.size(), 2u);  // match + unconditional

    auto [modified, action] = run_api_query_hooks(
        reg, HookEventType::UserPromptSubmit, ctx, "PAYLOAD");
    EXPECT_EQ(action.action, HookResponseAction::Continue);
    EXPECT_NE(modified.find("match-prompt"), std::string::npos);
    EXPECT_NE(modified.find("always-prompt"), std::string::npos);
    EXPECT_EQ(modified.find("miss-prompt"), std::string::npos)
        << "mismatched hook must not contribute";
    EXPECT_NE(modified.find("PAYLOAD"), std::string::npos);
}

TEST(HooksE2E, PromptAccumulationPersistsAcrossManyHooks) {
    // Several UserPromptSubmit prompt-hooks must all contribute to the
    // accumulated preamble, with the original payload surviving at the tail.
    std::vector<IndividualHookConfig> reg;
    for (int i = 1; i <= 4; ++i) {
        reg.push_back(make_prompt_hook(HookEventType::UserPromptSubmit,
                                       {}, "rule" + std::to_string(i)));
    }
    auto ctx = ctx_with_vars({});
    auto [modified, action] = run_api_query_hooks(
        reg, HookEventType::UserPromptSubmit, ctx, "USER_PAYLOAD");
    EXPECT_EQ(action.action, HookResponseAction::Continue);
    for (int i = 1; i <= 4; ++i) {
        EXPECT_NE(modified.find("rule" + std::to_string(i)),
                  std::string::npos)
            << "missing rule" << i << " in:\n" << modified;
    }
    // Original payload survives and is positioned after the preamble.
    auto last_user = modified.rfind("USER_PAYLOAD");
    auto last_rule = modified.rfind("rule4");
    EXPECT_NE(last_user, std::string::npos);
    EXPECT_NE(last_rule, std::string::npos);
    EXPECT_GT(last_user, last_rule)
        << "payload must come after the accumulated prompts";
}

TEST(HooksE2E, EmptyRegistryIsNoOp) {
    // With no hooks registered, the pipeline returns Continue + the payload
    // unchanged.
    auto ctx = ctx_with_vars({{"tool.name", "bash"}});
    auto [modified, act] = run_api_query_hooks(
        {}, HookEventType::UserPromptSubmit, ctx, "payload");
    EXPECT_EQ(act.action, HookResponseAction::Continue);
    EXPECT_EQ(modified, "payload");
}

TEST(HooksE2E, CrossEventIsolation) {
    // Hooks registered for one event must not fire when the pipeline runs for
    // a different event.  Here a PostToolUse prompt hook is in the registry,
    // but we run the UserPromptSubmit pipeline — the hook must be skipped.
    auto post_hook = make_prompt_hook(HookEventType::PostToolUse,
                                      {}, "post-leak");
    std::vector<IndividualHookConfig> reg = {post_hook};
    auto ctx = ctx_with_vars({});
    auto [modified, act] = run_api_query_hooks(
        reg, HookEventType::UserPromptSubmit, ctx, "payload");
    EXPECT_EQ(act.action, HookResponseAction::Continue)
        << "post-tool hook must not fire on UserPromptSubmit";
    EXPECT_EQ(modified, "payload")
        << "no prompt accumulation from a different event's hook";
}

TEST(HooksE2E, VariableInterpolationAcrossHooks) {
    // Each prompt hook interpolates a different context variable; the
    // pipeline must resolve all of them from a single shared context.
    auto h1 = make_prompt_hook(HookEventType::UserPromptSubmit,
                               {}, "tool=${tool.name}");
    auto h2 = make_prompt_hook(HookEventType::UserPromptSubmit,
                               {}, "user=${user.role}");
    auto h3 = make_prompt_hook(HookEventType::UserPromptSubmit,
                               {}, "cwd=${cwd}");
    std::vector<IndividualHookConfig> reg = {h1, h2, h3};
    auto ctx = ctx_with_vars({
        {"tool.name", "bash"},
        {"user.role", "admin"},
        {"cwd", "/repo"},
    });
    auto [modified, action] = run_api_query_hooks(
        reg, HookEventType::UserPromptSubmit, ctx, "");
    EXPECT_EQ(action.action, HookResponseAction::Continue);
    EXPECT_NE(modified.find("tool=bash"), std::string::npos);
    EXPECT_NE(modified.find("user=admin"), std::string::npos);
    EXPECT_NE(modified.find("cwd=/repo"), std::string::npos);
}

