/// @file test_skills.cpp
/// @brief Skill system smoke tests aligned with current C++ modules.

#include <gtest/gtest.h>
#include <optional>
#include <string>

import cc.skills.skill;
import cc.skills.bundled;

TEST(SkillDefinition, SerializesMetadata) {
    cc::skills::SkillDefinition skill{
        .name = "test-skill",
        .description = "A test skill",
        .trigger_patterns = {"test"},
        .content = "Use this test skill.",
        .is_builtin = true,
        .author = std::nullopt,
        .version = std::nullopt,
    };

    auto json = skill.to_json();
    EXPECT_NE(json.find("test-skill"), std::string::npos);
    EXPECT_NE(json.find("trigger_count"), std::string::npos);
}

TEST(SkillExecutor, MatchesRegisteredSkillPattern) {
    cc::skills::SkillExecutor executor;
    executor.register_skill(cc::skills::SkillDefinition{
        .name = "debug",
        .description = "Debug problems",
        .trigger_patterns = {"debug|diagnose"},
        .content = "Debug carefully.",
        .is_builtin = false,
        .author = std::nullopt,
        .version = std::nullopt,
    });

    auto matches = executor.match("please debug this failure");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().skill_name, "debug");
}

TEST(BundledSkills, ProvidesBuiltInSkills) {
    cc::skills::BundledSkills bundled;
    EXPECT_GT(bundled.size(), 0u);
    EXPECT_FALSE(bundled.all().empty());
    EXPECT_NE(bundled.find("debug"), nullptr);
}

TEST(BundledSkills, RegistersIntoExecutor) {
    cc::skills::BundledSkills bundled;
    cc::skills::SkillExecutor executor;
    bundled.register_all(executor);
    EXPECT_EQ(executor.size(), bundled.size());
}
