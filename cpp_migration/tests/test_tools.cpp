/// @file test_tools.cpp
/// @brief Tool registry smoke tests aligned with current C++ modules.

#include <gtest/gtest.h>
#include <string>
#include <vector>

import cc.tools.registry;

TEST(ToolRegistry, ListsBuiltInTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    EXPECT_FALSE(names.empty());
}

TEST(ToolRegistry, ContainsExpectedTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    ASSERT_FALSE(names.empty());

    // Check that known tools are present
    bool has_bash = false;
    for (const auto& name : names) {
        if (name == "Bash") has_bash = true;
    }
    EXPECT_TRUE(has_bash);
}

TEST(ToolRegistry, CoreRegistryCanBeConstructed) {
    cc::tools::registry::ToolRegistry registry;
    EXPECT_EQ(registry.size(), 0u);  // Empty by default
}
