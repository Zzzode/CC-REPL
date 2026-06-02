/// @file test_core.cpp
/// @brief Core C++ module smoke tests aligned with current module names/APIs.

#include <gtest/gtest.h>
#include <string>
#include <variant>

import cc.types.types;
import cc.config.config;
import cc.config.feature_flags;
import cc.constants.constants;
import cc.coordinator.types;
import cc.tasks.task_graph;

TEST(CoreTypes, RoleToStringAndContentVariant) {
    EXPECT_EQ(cc::core::role_to_string(cc::core::Role::User), "user");

    cc::core::ContentBlock block = cc::core::TextBlock{"hello"};
    ASSERT_TRUE(std::holds_alternative<cc::core::TextBlock>(block));
    EXPECT_EQ(std::get<cc::core::TextBlock>(block).text, "hello");
}

TEST(CoreConfig, FeatureFlagsToggleRuntimeBits) {
    cc::core::FeatureFlags flags;
    EXPECT_FALSE(flags.is_enabled(cc::core::FeatureFlag::MultiAgent));

    flags.enable(cc::core::FeatureFlag::MultiAgent);
    EXPECT_TRUE(flags.is_enabled(cc::core::FeatureFlag::MultiAgent));

    flags.disable(cc::core::FeatureFlag::MultiAgent);
    EXPECT_FALSE(flags.is_enabled(cc::core::FeatureFlag::MultiAgent));
}

TEST(CoreConfig, ConfigManagerExposesDefaultSettings) {
    cc::core::ConfigManager manager;
    EXPECT_FALSE(manager.settings().model.default_model.empty());
    EXPECT_GT(manager.settings().model.max_output_tokens, 0u);
}

TEST(CoreFeatureFlags, RuntimeManagerCanFindAndToggleFeature) {
    auto feature = cc::core::flags::FeatureFlagManager::find_by_name("PROACTIVE");
    ASSERT_TRUE(feature.has_value());

    cc::core::flags::FeatureFlagManager manager;
    manager.enable(*feature);
    EXPECT_TRUE(manager.is_enabled(*feature));
    EXPECT_NE(manager.enabled_summary().find("PROACTIVE"), std::string::npos);
}

TEST(CoreConstants, AppMetadataIsDefined) {
    EXPECT_FALSE(std::string(cc::core::constants::kAppName).empty());
    EXPECT_FALSE(std::string(cc::core::constants::kVersion).empty());
    EXPECT_GT(cc::core::constants::api_limits::kMaxTokensDefault, 0u);
}

TEST(CoreCoordinator, CoordinatorModeParseRoundTrip) {
    auto parsed = cc::coordinator::parse_coordinator_mode("parallel");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(cc::coordinator::coordinator_mode_to_string(*parsed), "parallel");
}

TEST(CoreTasks, TaskSchedulerTracksSubmittedTask) {
    cc::core::TaskScheduler scheduler;
    auto id = scheduler.submit("test task");
    ASSERT_TRUE(id.has_value());

    auto status = scheduler.get_status(*id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(scheduler.total_tasks(), 1u);
}
