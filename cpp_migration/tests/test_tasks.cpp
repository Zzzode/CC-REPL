/// @file test_tasks.cpp
/// @brief cc_tasks migration parity tests.

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

import cc.tasks.local_agent_task;
import cc.tasks.in_process_teammate_task;
import cc.tasks.task;
import cc.tasks.types;

TEST(LocalAgentTask, NotificationLeavesOutputFileEmptyWhenNoPathIsKnown) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "completed"
    );

    EXPECT_NE(xml.find("<task_id>task_123</task_id>"), std::string::npos);
    EXPECT_NE(xml.find("<output_file></output_file>"), std::string::npos);
    EXPECT_EQ(xml.find("<output_file>task_123</output_file>"), std::string::npos);
}

TEST(LocalAgentTask, NotificationUsesExplicitOutputFileWhenProvided) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "completed",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        "/tmp/agent-output.log"
    );

    EXPECT_NE(xml.find("<output_file>/tmp/agent-output.log</output_file>"), std::string::npos);
}

TEST(LocalAgentTask, NotificationEscapesXmlTextNodes) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_<1>&",
        "Review <code>&docs",
        "failed",
        "bad <tag>&value",
        "result </result> & more",
        "tool_<id>",
        "/tmp/a&b",
        "branch<main>",
        "/tmp/out&1.log"
    );

    EXPECT_NE(xml.find("task_&lt;1&gt;&amp;"), std::string::npos);
    EXPECT_NE(xml.find("Review &lt;code&gt;&amp;docs"), std::string::npos);
    EXPECT_NE(xml.find("bad &lt;tag&gt;&amp;value"), std::string::npos);
    EXPECT_NE(xml.find("result &lt;/result&gt; &amp; more"), std::string::npos);
    EXPECT_NE(xml.find("tool_&lt;id&gt;"), std::string::npos);
    EXPECT_NE(xml.find("/tmp/a&amp;b"), std::string::npos);
    EXPECT_NE(xml.find("branch&lt;main&gt;"), std::string::npos);
    EXPECT_NE(xml.find("/tmp/out&amp;1.log"), std::string::npos);
}

TEST(LocalAgentTask, StoppedNotificationUsesStoppedStatusAndSummary) {
    auto xml = cc::tasks::generate_agent_notification(
        "task_123",
        "Review code",
        "stopped"
    );

    EXPECT_NE(xml.find("<status>stopped</status>"), std::string::npos);
    EXPECT_NE(xml.find("Agent \"Review code\" was stopped"), std::string::npos);
    EXPECT_EQ(xml.find("killed"), std::string::npos);
}

TEST(InProcessTeammateTask, AppendMessageStoresUiVisiblePendingMessage) {
    cc::tasks::InProcessTeammateTaskState state{};
    state.type = cc::core::TaskType::InProcessTeammate;
    state.status = cc::core::TaskStatus::Running;

    cc::tasks::append_teammate_message("tm_1", "hello", [&](const std::string&, std::function<void(cc::tasks::InProcessTeammateTaskState&)> mutate) {
        mutate(state);
    });

    ASSERT_EQ(state.pending_user_messages.size(), 1u);
    EXPECT_EQ(state.pending_user_messages[0], "hello");
}

TEST(InProcessTeammateTask, FiltersTaskRegistryToTeammateStates) {
    cc::tasks::InProcessTeammateTaskState teammate{};
    teammate.type = cc::core::TaskType::InProcessTeammate;
    teammate.status = cc::core::TaskStatus::Running;
    teammate.identity.agent_id = "researcher@team";
    teammate.identity.agent_name = "researcher";

    cc::tasks::LocalAgentTaskState local_agent{};
    local_agent.type = cc::core::TaskType::LocalAgent;
    local_agent.status = cc::core::TaskStatus::Running;

    std::vector<cc::core::TaskStateBase*> registry = {&teammate, &local_agent};
    auto result = cc::tasks::get_all_in_process_teammate_tasks(registry);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].identity.agent_id, "researcher@team");
}
