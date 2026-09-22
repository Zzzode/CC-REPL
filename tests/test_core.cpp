/// @file test_core.cpp
/// @brief Core C++ module smoke tests aligned with current module names/APIs.

#include <gtest/gtest.h>
#include <cstdint>
#include <climits>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

import cc.types.types;
import cc.config.config;
import cc.config.feature_flags;
import cc.constants.constants;
import cc.coordinator.types;
import cc.tasks.task_graph;
import cc.utils.yaml;
import cc.utils.parse_int;

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

TEST(UtilsYaml, ScalarsParseStrictlyWithoutFromChars) {
    namespace uy = cc::utils;
    // Integers parse as int64 (the portable strict parser).
    auto pos = uy::parse_yaml("v: 42");
    ASSERT_TRUE(std::holds_alternative<uy::YamlMap>(pos.data));
    EXPECT_EQ(std::get<std::int64_t>(std::get<uy::YamlMap>(pos.data).at("v").data), 42);

    auto neg = uy::parse_yaml("v: -7");
    EXPECT_EQ(std::get<std::int64_t>(
        std::get<uy::YamlMap>(neg.data).at("v").data), -7);

    // Floats parse as double.
    auto dbl = uy::parse_yaml("v: 1.5");
    EXPECT_DOUBLE_EQ(std::get<double>(
        std::get<uy::YamlMap>(dbl.data).at("v").data), 1.5);

    // Non-numeric / partial-garbage stay strings. (Leading whitespace after
    // the colon is trimmed by parse_block before reaching parse_scalar, so it
    // is not tested here; "+5" is a valid double per from_chars parity.)
    for (const char* bad : {"v: 12abc", "v: 1.2.3", "v: 1-2"}) {
        auto y = uy::parse_yaml(bad);
        ASSERT_TRUE(std::holds_alternative<uy::YamlMap>(y.data)) << bad;
        EXPECT_TRUE(std::get<uy::YamlMap>(y.data).at("v").is_string())
            << "expected string for: " << bad;
    }

    // A magnitude beyond int64 range is still a valid (huge) double, matching
    // from_chars(double) which fully consumes it; assert it is NOT an int.
    auto huge = uy::parse_yaml("v: 99999999999999999999999");
    const auto& huge_v = std::get<uy::YamlMap>(huge.data).at("v").data;
    EXPECT_FALSE(std::holds_alternative<std::int64_t>(huge_v));
}

TEST(UtilsParseInt, StrictFromCharsSemantics) {
    auto parse = [](std::string_view s, std::int64_t& out) {
        return cc::utils::from_chars(s.data(), s.data() + s.size(), out);
    };

    std::int64_t v = 0;
    EXPECT_EQ(parse("42", v).ec, std::errc{});
    EXPECT_EQ(v, 42);
    EXPECT_EQ(parse("-7", v).ec, std::errc{});
    EXPECT_EQ(v, -7);

    // signed range boundaries
    EXPECT_EQ(parse("9223372036854775807", v).ec, std::errc{});
    EXPECT_EQ(v, INT64_MAX);
    EXPECT_EQ(parse("-9223372036854775808", v).ec, std::errc{});
    EXPECT_EQ(v, INT64_MIN);

    // out of range
    EXPECT_EQ(parse("9223372036854775808", v).ec, std::errc::result_out_of_range);
    EXPECT_EQ(parse("-9223372036854775809", v).ec, std::errc::result_out_of_range);

    // malformed: empty, sign only, leading +, whitespace, trailing garbage
    EXPECT_EQ(parse("", v).ec, std::errc::invalid_argument);
    EXPECT_EQ(parse("-", v).ec, std::errc::invalid_argument);
    EXPECT_EQ(parse("+5", v).ec, std::errc::invalid_argument);
    EXPECT_EQ(parse(" 5", v).ec, std::errc::invalid_argument);
    EXPECT_EQ(parse("5x", v).ec, std::errc::invalid_argument);

    // unsigned (uint16_t, used by port parsing)
    auto parse_u16 = [](std::string_view s, std::uint16_t& out) {
        return cc::utils::from_chars(s.data(), s.data() + s.size(), out);
    };
    std::uint16_t port = 0;
    EXPECT_EQ(parse_u16("8080", port).ec, std::errc{});
    EXPECT_EQ(port, 8080);
    EXPECT_EQ(parse_u16("70000", port).ec, std::errc::result_out_of_range);
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
