/// @file test_hooks.cpp
/// @brief Hook module smoke tests aligned with current C++ module APIs.

#include <gtest/gtest.h>
#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <vector>

import cc.hooks.command_queue;
import cc.hooks.context;
import cc.hooks.terminal_size;
import cc.hooks.text_input;
import cc.hooks.typeahead;
import cc.hooks.virtual_scroll;

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
