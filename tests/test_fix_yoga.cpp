/// @file test_fix_yoga.cpp
/// @brief Coverage for the implemented subset of cc.ui.layout.yoga.
///
/// The C++ yoga module is a small single-pass flexbox (see the module-level
/// header in yoga.cppm for the exact supported subset). These tests pin the
/// behaviour that IS implemented so that future partial ports do not silently
/// regress it. They deliberately avoid flex-wrap / absolute positioning /
/// measure / gap / percent — those features are not implemented and no caller
/// in the C++ migration exercises them (the UI renders via FTXUI, not via
/// compute_layout). See migration-audit-report.md finding H5.

#include <gtest/gtest.h>

import cc.ui.layout.yoga;

using namespace cc::ui::layout;

namespace {

// Helper: build a leaf node with the given fixed main-axis size (no flex).
auto make_fixed_leaf(int width, int height, int margin = 0) -> LayoutNode {
    LayoutNode n;
    n.width = width;
    n.height = height;
    n.margin = margin;
    return n;
}

// Helper: build a flex-grow leaf (no fixed main-axis size).
auto make_flex_leaf(int flex, int margin = 0) -> LayoutNode {
    LayoutNode n;
    n.flex = flex;
    n.margin = margin;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Root layout
// ---------------------------------------------------------------------------

TEST(FixYoga, RootFillsContainerWhenNoExplicitSize) {
    LayoutNode root;
    std::vector<ComputedLayout> out = compute_layout(root, 100, 40);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].x, 0);
    EXPECT_EQ(out[0].y, 0);
    EXPECT_EQ(out[0].width, 100);
    EXPECT_EQ(out[0].height, 40);
}

TEST(FixYoga, RootRespectsExplicitWidthHeight) {
    LayoutNode root;
    root.width = 60;
    root.height = 20;
    std::vector<ComputedLayout> out = compute_layout(root, 100, 40);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].width, 60);
    EXPECT_EQ(out[0].height, 20);
}

TEST(FixYoga, RootMarginShrinksContentBox) {
    LayoutNode root;
    root.margin = 5;
    std::vector<ComputedLayout> out = compute_layout(root, 100, 40);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].x, 5);
    EXPECT_EQ(out[0].y, 5);
    EXPECT_EQ(out[0].width, 90);  // 100 - 2*5
    EXPECT_EQ(out[0].height, 30); // 40 - 2*5
}

TEST(FixYoga, EmptyRootProducesSingleResult) {
    LayoutNode root;
    std::vector<ComputedLayout> out = compute_layout(root, 80, 24);
    EXPECT_EQ(out.size(), 1u);
}

// ---------------------------------------------------------------------------
// Column direction (default) — main axis is height
// ---------------------------------------------------------------------------

TEST(FixYoga, ColumnStacksFixedChildrenTopToBottom) {
    LayoutNode root;
    root.direction = FlexDirection::Column;
    LayoutNode a = make_fixed_leaf(10, 4);
    LayoutNode b = make_fixed_leaf(20, 6);
    root.children = {&a, &b};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 100);
    ASSERT_EQ(out.size(), 3u);
    // children are in tree order after the root
    EXPECT_EQ(out[1].x, 0);
    EXPECT_EQ(out[1].y, 0);
    EXPECT_EQ(out[1].width, 10);
    EXPECT_EQ(out[1].height, 4);
    EXPECT_EQ(out[2].x, 0);
    EXPECT_EQ(out[2].y, 4); // stacked below first
    EXPECT_EQ(out[2].width, 20);
    EXPECT_EQ(out[2].height, 6);
}

TEST(FixYoga, ColumnFlexGrowDistributesRemainingMainSpace) {
    LayoutNode root;
    root.direction = FlexDirection::Column;
    LayoutNode f1 = make_flex_leaf(1); // flex-grow 1
    LayoutNode f2 = make_flex_leaf(3); // flex-grow 3
    root.children = {&f1, &f2};

    // Container height 100, no fixed children -> 100 split 1:3 -> 25 / 75
    std::vector<ComputedLayout> out = compute_layout(root, 100, 100);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].height, 25);
    EXPECT_EQ(out[2].height, 75);
}

TEST(FixYoga, FlexGrowOnlyAppliesToRemainingSpaceAfterFixedChildren) {
    LayoutNode root;
    root.direction = FlexDirection::Column;
    LayoutNode fixed = make_fixed_leaf(10, 40); // consumes 40 of 100
    LayoutNode f = make_flex_leaf(1);
    root.children = {&fixed, &f};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 100);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].height, 40); // fixed
    EXPECT_EQ(out[2].height, 60); // remaining 100 - 40
}

// ---------------------------------------------------------------------------
// Row direction — main axis is width
// ---------------------------------------------------------------------------

TEST(FixYoga, RowStacksFixedChildrenLeftToRight) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    LayoutNode a = make_fixed_leaf(15, 10);
    LayoutNode b = make_fixed_leaf(25, 10);
    root.children = {&a, &b};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 100);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].x, 0);
    EXPECT_EQ(out[1].width, 15);
    EXPECT_EQ(out[2].x, 15);
    EXPECT_EQ(out[2].width, 25);
}

TEST(FixYoga, RowFlexGrowDistributesRemainingWidth) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    LayoutNode f1 = make_flex_leaf(2);
    LayoutNode f2 = make_flex_leaf(1);
    root.children = {&f1, &f2};

    // Container width 90 split 2:1 -> 60 / 30
    std::vector<ComputedLayout> out = compute_layout(root, 90, 20);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].width, 60);
    EXPECT_EQ(out[2].width, 30);
}

// ---------------------------------------------------------------------------
// Justify-content (main axis)
// ---------------------------------------------------------------------------

TEST(FixYoga, JustifyCenterCentersChildrenOnMainAxis) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.justify = Justify::Center;
    LayoutNode a = make_fixed_leaf(10, 10);
    LayoutNode b = make_fixed_leaf(10, 10);
    root.children = {&a, &b};

    // 100 wide, 20 of children -> start offset (100-20)/2 = 40
    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].x, 40);
    EXPECT_EQ(out[2].x, 50);
}

TEST(FixYoga, JustifyEndPacksChildrenToEnd) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.justify = Justify::End;
    LayoutNode a = make_fixed_leaf(10, 10);
    root.children = {&a};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].x, 90); // 100 - 10
}

TEST(FixYoga, JustifySpaceBetweenInsertsGapBetweenChildrenOnly) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.justify = Justify::SpaceBetween;
    LayoutNode a = make_fixed_leaf(10, 10);
    LayoutNode b = make_fixed_leaf(10, 10);
    LayoutNode c = make_fixed_leaf(10, 10);
    root.children = {&a, &b, &c};

    // 100 wide, 30 of children, 2 gaps -> gap = (100-30)/2 = 35
    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[1].x, 0);   // first at start
    EXPECT_EQ(out[2].x, 45);  // 10 + 35
    EXPECT_EQ(out[3].x, 90);  // 45 + 10 + 35
}

TEST(FixYoga, JustifySpaceAroundHasHalfGapAtEdges) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.justify = Justify::SpaceAround;
    LayoutNode a = make_fixed_leaf(10, 10);
    LayoutNode b = make_fixed_leaf(10, 10);
    root.children = {&a, &b};

    // gap = (100 - 20) / 2 = 40, start offset = gap/2 = 20
    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].x, 20); // gap/2
    EXPECT_EQ(out[2].x, 70); // 20 + 10 + 40
}

// ---------------------------------------------------------------------------
// Align-items (cross axis)
// ---------------------------------------------------------------------------

TEST(FixYoga, AlignCenterCentersChildrenOnCrossAxis) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.align = Align::Center;
    LayoutNode a = make_fixed_leaf(10, 4); // 4 tall in 20 tall cross space
    root.children = {&a};

    // cross_offset = (20 - 4) / 2 = 8
    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].y, 8);
    EXPECT_EQ(out[1].height, 4);
}

TEST(FixYoga, AlignEndPacksChildrenToEndOfCrossAxis) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.align = Align::End;
    LayoutNode a = make_fixed_leaf(10, 4);
    root.children = {&a};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].y, 16); // 20 - 4
}

TEST(FixYoga, AlignStretchFillsCrossAxisWhenChildHasNoCrossSize) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    root.align = Align::Stretch;
    LayoutNode a; // no width/height
    a.flex = 1;
    root.children = {&a};

    // main axis = width (flex 1 fills 100); cross axis = height -> stretched
    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].height, 20); // cross space
}

// ---------------------------------------------------------------------------
// Padding insets the content box for children
// ---------------------------------------------------------------------------

TEST(FixYoga, PaddingInsetsChildOrigin) {
    LayoutNode root;
    root.direction = FlexDirection::Column;
    root.padding = 4;
    LayoutNode a = make_fixed_leaf(10, 6);
    root.children = {&a};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 100);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].x, 4); // content_x = 0 + padding
    EXPECT_EQ(out[1].y, 4); // content_y = 0 + padding
}

// ---------------------------------------------------------------------------
// Child margin offsets the child from its allocated slot
// ---------------------------------------------------------------------------

TEST(FixYoga, ChildMarginShiftsOriginAndConsumesSpace) {
    LayoutNode root;
    root.direction = FlexDirection::Row;
    LayoutNode a = make_fixed_leaf(10, 10, /*margin=*/3);
    root.children = {&a};

    std::vector<ComputedLayout> out = compute_layout(root, 100, 20);
    ASSERT_EQ(out.size(), 2u);
    // child_main_sizes[0] = 10 + 3*2 = 16; child layout x = 0 + 0 + 3 = 3
    EXPECT_EQ(out[1].x, 3);
    EXPECT_EQ(out[1].width, 10);
}
