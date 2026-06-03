/// @file test_skills.cpp
/// @brief Skill system smoke tests aligned with current C++ modules.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

import cc.skills.skill;
import cc.skills.bundled;

namespace fs = std::filesystem;

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

TEST(SkillLoader, LoadsDirectorySkillMarkdown) {
    auto root = fs::temp_directory_path() / "cc_repl_skill_directory_test";
    fs::remove_all(root);
    fs::create_directories(root / "review-skill");
    {
        std::ofstream skill(root / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Review code changes
---
Read the diff and report concrete risks.
)MD";
    }

    cc::skills::SkillLoader loader;
    auto result = loader.load_from_directory(root);

    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().name, "review-skill");
    EXPECT_EQ(result->front().description, "Review code changes");
    EXPECT_NE(result->front().content.find("Read the diff"), std::string::npos);
}
