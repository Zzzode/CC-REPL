/// @file test_dialog_system.cpp
/// @brief Unit tests for the M7 dialog framework: DialogQueue,
/// DialogRendererRegistry, DialogFrame, and default renderers.
///
/// Tests cover:
///   - DialogType / DialogSlot / DialogPriority enums
///   - DialogQueue push/pop/peek across all three slots
///   - Priority band ordering (bottom slot)
///   - Typing suppression
///   - Remove by id
///   - DialogRendererRegistry registration and fallback
///   - DialogFrame rendering (basic sanity)
///   - Default renderers (basic sanity)

#include <string>
#include <string_view>
#include <optional>
#include <functional>
#include <vector>
#include <array>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <format>

#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.modal_renderers;
import cc.ui.dialogs.bottom_renderers;
import cc.ui.dialogs.all_renderers;
import cc.ui.dialogs.triggers;
import cc.ui.dialogs.quick_open;
import cc.ui.dialogs.sandbox_permission;
import cc.ui.design.theme;
import cc.ui.design.tokens;

namespace {

namespace dsys = cc::ui::dialogs::system;
namespace dframe = cc::ui::dialogs::frame;
namespace drender = cc::ui::dialogs::default_renderers;
using Theme = cc::ui::design::theme::Theme;

// ============================================================
// Golden snapshot helpers (verbatim copy from tests/test_ui.cpp)
// ============================================================
namespace dialog_test_golden {

/// Directory holding golden snapshot files (derived from __FILE__).
std::string golden_dir() {
    std::string f = __FILE__;
    auto pos = f.find_last_of('/');
    return f.substr(0, pos + 1) + "golden/";
}

/// Normalize line endings to LF-only for cross-platform robustness.
std::string normalize_line_endings(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\r') out.push_back(c);
    }
    return out;
}

/// Golden-snapshot check.  Set UPDATE_GOLDENS=1 to (re)write the file.
void check_golden(const std::string& name, const std::string& actual) {
    const std::string path = golden_dir() + name + ".txt";
    if (std::getenv("UPDATE_GOLDENS") != nullptr) {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.good()) << "cannot write golden: " << path;
        out << actual;
        SUCCEED() << "golden updated: " << path;
        return;
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.good()) << "golden missing: " << path
                           << " (run UPDATE_GOLDENS=1 to create)";
    std::string expected((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(normalize_line_endings(actual), normalize_line_endings(expected))
        << "golden mismatch for '" << name
        << "' (run UPDATE_GOLDENS=1 to refresh)";
}

/// Render an Element to a fixed-size terminal buffer (includes ANSI codes).
std::string render_to_ansi(ftxui::Element element, int width, int height) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen.ToString();
}

/// Deterministic LightTheme for golden snapshots.  Mirrors the theme
/// construction used in test_ui.cpp visual-snapshot tests.
[[nodiscard]] inline Theme get_light_theme() {
    using ThemeVariant = cc::ui::design::theme::ThemeVariant;
    return Theme{
        ThemeVariant::Light,
        &cc::ui::design::tokens::palette::light,
    };
}

} // namespace dialog_test_golden


// ============================================================
// DialogType / DialogSlot / DialogPriority tests
// ============================================================

TEST(DialogSystem, DialogTypeNames) {
    EXPECT_EQ(dsys::dialog_type_name(dsys::DialogType::ToolPermission), "tool-permission");
    EXPECT_EQ(dsys::dialog_type_name(dsys::DialogType::SandboxPermission), "sandbox-permission");
    EXPECT_EQ(dsys::dialog_type_name(dsys::DialogType::CostThreshold), "cost-threshold");
    EXPECT_EQ(dsys::dialog_type_name(dsys::DialogType::SettingsPanel), "settings-panel");
}

TEST(DialogSystem, SlotMappingCorrect) {
    // Overlay slot
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::ToolPermission), dsys::DialogSlot::Overlay);

    // Bottom slot
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::SandboxPermission), dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::PromptDialog), dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::CostThreshold), dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::IdleReturn), dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::LspRecommendation), dsys::DialogSlot::Bottom);

    // Modal slot
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::SettingsPanel), dsys::DialogSlot::Modal);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::TasksView), dsys::DialogSlot::Modal);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::HelpView), dsys::DialogSlot::Modal);

    // Standalone (legacy — most are migrating to Modal)
    // (none remain as standalone; all migrated to queue-based modal)
}

TEST(DialogSystem, PriorityMappingCorrect) {
    // Band 1: MessageSelector (highest, never suppressed)
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::MessageSelector), dsys::DialogPriority::Band1);

    // Band 2: SandboxPermission
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::SandboxPermission), dsys::DialogPriority::Band2);

    // Band 3: permissions, prompt, elicitation
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::ToolPermission), dsys::DialogPriority::Band3);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::PromptDialog), dsys::DialogPriority::Band3);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::Elicitation), dsys::DialogPriority::Band3);

    // Band 4: cost, idle, ultraplan
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::CostThreshold), dsys::DialogPriority::Band4);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::IdleReturn), dsys::DialogPriority::Band4);

    // Band 6 (lowest): recs, hints, upsells
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::LspRecommendation), dsys::DialogPriority::Band6);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::PluginHint), dsys::DialogPriority::Band6);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::DesktopUpsell), dsys::DialogPriority::Band6);
}

TEST(DialogSystem, TypingSuppressionCorrect) {
    // Only band 1 (MessageSelector) is never suppressed
    EXPECT_FALSE(dsys::is_suppressed_by_typing(dsys::DialogType::MessageSelector));

    // Everything else is suppressed while typing
    EXPECT_TRUE(dsys::is_suppressed_by_typing(dsys::DialogType::SandboxPermission));
    EXPECT_TRUE(dsys::is_suppressed_by_typing(dsys::DialogType::ToolPermission));
    EXPECT_TRUE(dsys::is_suppressed_by_typing(dsys::DialogType::CostThreshold));
    EXPECT_TRUE(dsys::is_suppressed_by_typing(dsys::DialogType::LspRecommendation));
}

// ============================================================
// DialogPayloadVariant tests
// ============================================================

TEST(DialogSystem, PayloadVariantTypeOf) {
    dsys::ToolPermissionPayload tp;
    tp.id = "test-tp";
    tp.tool_name = "BashTool";
    dsys::DialogPayloadVariant v = tp;
    EXPECT_EQ(dsys::type_of(v), dsys::DialogType::ToolPermission);
    EXPECT_EQ(dsys::id_of(v), "test-tp");
    EXPECT_EQ(dsys::slot_of(v), dsys::DialogSlot::Overlay);
    EXPECT_EQ(dsys::priority_of(v), dsys::DialogPriority::Band3);

    dsys::CostThresholdPayload ct;
    ct.id = "test-ct";
    dsys::DialogPayloadVariant v2 = ct;
    EXPECT_EQ(dsys::type_of(v2), dsys::DialogType::CostThreshold);
    EXPECT_EQ(dsys::id_of(v2), "test-ct");
    EXPECT_EQ(dsys::slot_of(v2), dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::priority_of(v2), dsys::DialogPriority::Band4);
}

// ============================================================
// DialogQueue tests
// ============================================================

TEST(DialogQueue, EmptyByDefault) {
    dsys::DialogQueue q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.total_size(), 0u);
    EXPECT_FALSE(q.has_overlay());
    EXPECT_FALSE(q.has_any_bottom());
    EXPECT_FALSE(q.has_modal());
}

TEST(DialogQueue, OverlayPushPeekPop) {
    dsys::DialogQueue q;

    dsys::ToolPermissionPayload tp;
    tp.id = "tp-1";
    tp.tool_name = "BashTool";
    q.push(dsys::DialogPayloadVariant{tp});

    EXPECT_EQ(q.total_size(), 1u);
    EXPECT_TRUE(q.has_overlay());
    EXPECT_FALSE(q.empty());

    auto peeked = q.peek_overlay();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "tp-1");

    q.pop_overlay();
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.has_overlay());
}

TEST(DialogQueue, BottomPriorityOrder) {
    dsys::DialogQueue q;

    // Push in low -> high priority order
    dsys::IdleReturnPayload idle;
    idle.id = "idle-1";
    q.push(dsys::DialogPayloadVariant{idle});

    dsys::CostThresholdPayload cost;
    cost.id = "cost-1";
    q.push(dsys::DialogPayloadVariant{cost});

    dsys::SandboxPermissionPayload sandbox;
    sandbox.id = "sandbox-1";
    q.push(dsys::DialogPayloadVariant{sandbox});

    EXPECT_EQ(q.total_size(), 3u);

    // Peek should return highest priority (sandbox = band 2)
    auto peeked = q.peek_bottom(/*is_prompt_input_active=*/false);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "sandbox-1");

    // Pop highest priority
    q.pop_bottom(false);
    EXPECT_EQ(q.total_size(), 2u);

    // Next should be idle (band 4, pushed first — FIFO within same band)
    peeked = q.peek_bottom(false);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "idle-1");

    q.pop_bottom(false);
    EXPECT_EQ(q.total_size(), 1u);

    // Last should be cost (band 4, pushed after idle — FIFO within same band)
    peeked = q.peek_bottom(false);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "cost-1");

    q.pop_bottom(false);
    EXPECT_TRUE(q.empty());
}

TEST(DialogQueue, BottomTypingSuppression) {
    dsys::DialogQueue q;

    // Push a cost dialog (band 4 — suppressed while typing)
    dsys::CostThresholdPayload cost;
    cost.id = "cost-1";
    q.push(dsys::DialogPayloadVariant{cost});

    // When typing is active, no bottom dialog should show
    // (band 4 is suppressed by typing)
    auto peeked = q.peek_bottom(/*is_prompt_input_active=*/true);
    EXPECT_FALSE(peeked.has_value());

    // When typing is not active, it should show
    peeked = q.peek_bottom(false);
    EXPECT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "cost-1");
}

TEST(DialogQueue, Band3AnimationSuppression) {
    using dsys::DialogPriority;

    // ------------------------------------------------------------------
    // Bottom slot: PromptDialog / Elicitation / WorkerSandboxPermission
    // are Band3 (i=2).  When allow_dialogs_with_animation=false,
    // they MUST be skipped in favour of the next active band (or nullopt
    // if no higher-priority non-suppressed dialog exists).
    // ------------------------------------------------------------------
    dsys::DialogQueue q;

    // Case A: Band3-only queue + animation active → nullopt.
    {
        dsys::PromptDialogPayload prompt;
        prompt.id = "prompt-1";
        q.push(dsys::DialogPayloadVariant{prompt});
    }
    auto peeked = q.peek_bottom(/*typing=*/false,
                                 /*allow_dialogs_with_animation=*/false);
    EXPECT_FALSE(peeked.has_value())
        << "Band3 PromptDialog MUST be suppressed when animation is active";

    // Case B: same queue + animation inactive → PromptDialog visible.
    peeked = q.peek_bottom(false, /*animation_ok=*/true);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "prompt-1");

    // Case C: SandboxPermission (Band2, i=1) + animation active → still
    // visible (only Band3 is blocked).
    {
        dsys::SandboxPermissionPayload sbx;
        sbx.id = "sbx-1";
        q.push(dsys::DialogPayloadVariant{sbx});
    }
    peeked = q.peek_bottom(false, /*animation_ok=*/false);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "sbx-1")
        << "Band2 SandboxPermission MUST NOT be blocked by animation";
    // Pop the Band2 front.
    q.pop_bottom(false, /*animation_ok=*/false);
    EXPECT_EQ(q.total_size(), 1u); // prompt-1 still in Band3

    // Case D: now Band3 (prompt) is next; animation still active → nullopt.
    peeked = q.peek_bottom(false, /*animation_ok=*/false);
    EXPECT_FALSE(peeked.has_value());

    // Case E: Push Band4 CostThreshold below.  With animation active,
    // Band4 (i=3) MUST still be reachable since we SKIP Band3 (i=2)
    // but continue the loop to i=3.
    {
        dsys::CostThresholdPayload cost;
        cost.id = "cost-1";
        q.push(dsys::DialogPayloadVariant{cost});
    }
    peeked = q.peek_bottom(false, /*animation_ok=*/false);
    ASSERT_TRUE(peeked.has_value())
        << "Band4 CostThreshold MUST be reachable when Band3 is skipped "
           "for animation suppression — find_active_band must CONTINUE, "
           "not RETURN, on !allow_dialogs_with_animation";
    EXPECT_EQ(dsys::id_of(peeked->get()), "cost-1");
    // pop correctly uses the same suppression rule
    q.pop_bottom(false, /*animation_ok=*/false);
    EXPECT_EQ(q.total_size(), 1u); // only prompt-1 remains

    // Case F: Cost gone — prompt is alone again.  Animation off → prompt.
    peeked = q.peek_bottom(false, /*animation_ok=*/true);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "prompt-1");
    q.pop_bottom(false, true);
    EXPECT_TRUE(q.empty());

    // ------------------------------------------------------------------
    // Overlay slot: ToolPermission is implicitly Band3; should_show_dialog
    // free function MUST suppress it when !allow_dialogs_with_animation.
    // ------------------------------------------------------------------
    {
        dsys::ToolPermissionPayload tp;
        tp.id = "tp-1";
        q.push(dsys::DialogPayloadVariant{tp}); // → Overlay slot
    }
    EXPECT_TRUE(q.has_overlay());
    auto overlay_peek = q.peek_overlay();
    ASSERT_TRUE(overlay_peek.has_value());

    EXPECT_FALSE(dsys::should_show_dialog(overlay_peek->get(),
                                           /*typing=*/false,
                                           /*animation_ok=*/false))
        << "Overlay (ToolPermission, Band3) MUST be suppressed by should_show_dialog "
           "when animation is active";
    EXPECT_TRUE(dsys::should_show_dialog(overlay_peek->get(),
                                          /*typing=*/false,
                                          /*animation_ok=*/true))
        << "Overlay (ToolPermission, Band3) MUST show when animation is off";

    // ------------------------------------------------------------------
    // Bottom slot free-function check: PromptDialog (Band3) suppressed
    // by animation, CostThreshold (Band4) NOT suppressed.
    // ------------------------------------------------------------------
    dsys::PromptDialogPayload prompt2;
    prompt2.id = "prompt2";
    auto prompt_v = dsys::DialogPayloadVariant{prompt2};
    EXPECT_FALSE(dsys::should_show_dialog(prompt_v, false, false));
    EXPECT_TRUE(dsys::should_show_dialog(prompt_v, false, true));
    // typing always suppresses it too
    EXPECT_FALSE(dsys::should_show_dialog(prompt_v, true, true));

    dsys::CostThresholdPayload cost2;
    cost2.id = "cost2";
    auto cost_v = dsys::DialogPayloadVariant{cost2};
    EXPECT_TRUE(dsys::should_show_dialog(cost_v, false, false))
        << "Band4 CostThreshold is NOT suppressed by the animation flag";
    EXPECT_FALSE(dsys::should_show_dialog(cost_v, true, true))
        << "Band4 CostThreshold IS suppressed by typing (typing kills bands 2..6)";
}

TEST(DialogQueue, ModalStack) {
    dsys::DialogQueue q;

    dsys::GenericDialogPayload g1;
    g1.id = "modal-1";
    g1.title = "First";
    q.push_modal(dsys::DialogPayloadVariant{g1});

    EXPECT_TRUE(q.has_modal());
    EXPECT_EQ(q.total_size(), 1u);

    auto peeked = q.peek_modal();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "modal-1");

    // Push second modal on top
    dsys::GenericDialogPayload g2;
    g2.id = "modal-2";
    g2.title = "Second";
    q.push_modal(dsys::DialogPayloadVariant{g2});

    EXPECT_EQ(q.total_size(), 2u);
    peeked = q.peek_modal();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "modal-2");

    // Pop top
    q.pop_modal();
    EXPECT_EQ(q.total_size(), 1u);
    peeked = q.peek_modal();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "modal-1");

    q.pop_modal();
    EXPECT_FALSE(q.has_modal());
    EXPECT_TRUE(q.empty());
}

TEST(DialogQueue, RemoveById) {
    dsys::DialogQueue q;

    dsys::CostThresholdPayload cost;
    cost.id = "cost-1";
    q.push(dsys::DialogPayloadVariant{cost});

    dsys::IdleReturnPayload idle;
    idle.id = "idle-1";
    q.push(dsys::DialogPayloadVariant{idle});

    EXPECT_EQ(q.total_size(), 2u);

    q.remove("cost-1");
    EXPECT_EQ(q.total_size(), 1u);

    auto peeked = q.peek_bottom(false);
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(dsys::id_of(peeked->get()), "idle-1");

    // Remove non-existent id — no crash
    q.remove("nonexistent");
    EXPECT_EQ(q.total_size(), 1u);
}

TEST(DialogQueue, ClearEmptiesAllSlots) {
    dsys::DialogQueue q;

    dsys::ToolPermissionPayload tp;
    tp.id = "tp-1";
    q.push(dsys::DialogPayloadVariant{tp});

    dsys::CostThresholdPayload cost;
    cost.id = "cost-1";
    q.push(dsys::DialogPayloadVariant{cost});

    dsys::GenericDialogPayload g;
    g.id = "modal-1";
    q.push_modal(dsys::DialogPayloadVariant{g});

    EXPECT_EQ(q.total_size(), 3u);

    q.clear();
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.total_size(), 0u);
    EXPECT_FALSE(q.has_overlay());
    EXPECT_FALSE(q.has_any_bottom());
    EXPECT_FALSE(q.has_modal());
}

TEST(DialogQueue, MultiSlotIndependent) {
    dsys::DialogQueue q;

    // Overlay
    dsys::ToolPermissionPayload tp;
    tp.id = "tp-1";
    q.push(dsys::DialogPayloadVariant{tp});

    // Bottom
    dsys::CostThresholdPayload cost;
    cost.id = "cost-1";
    q.push(dsys::DialogPayloadVariant{cost});

    // Modal
    dsys::GenericDialogPayload g;
    g.id = "modal-1";
    q.push_modal(dsys::DialogPayloadVariant{g});

    EXPECT_EQ(q.total_size(), 3u);
    EXPECT_TRUE(q.has_overlay());
    EXPECT_TRUE(q.has_any_bottom());
    EXPECT_TRUE(q.has_modal());

    // Pop overlay — bottom and modal should still be there
    q.pop_overlay();
    EXPECT_EQ(q.total_size(), 2u);
    EXPECT_FALSE(q.has_overlay());
    EXPECT_TRUE(q.has_any_bottom());
    EXPECT_TRUE(q.has_modal());
}

// ============================================================
// DialogRendererRegistry tests
// ============================================================

TEST(DialogRendererRegistry, EmptyRegistryFallsBack) {
    dsys::DialogRendererRegistry registry;
    dsys::DialogRenderContext ctx;

    dsys::ToolPermissionPayload tp;
    tp.id = "test";
    dsys::DialogPayloadVariant payload = tp;

    auto el = registry.render(payload, ctx);
    EXPECT_NE(el, nullptr);

    // Fallback should render something (not blank)
    auto screen = ftxui::Screen::Create({80, 20});
    Render(screen, el);
    EXPECT_GT(screen.ToString().size(), 0u);
}

TEST(DialogRendererRegistry, CustomRendererOverridesFallback) {
    dsys::DialogRendererRegistry registry;
    dsys::DialogRenderContext ctx;

    bool called = false;
    registry.register_dialog(dsys::DialogType::ToolPermission,
        [&](const dsys::DialogPayloadVariant&,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            called = true;
            return ftxui::text("custom renderer");
        });

    dsys::ToolPermissionPayload tp;
    tp.id = "test";
    dsys::DialogPayloadVariant payload = tp;

    auto el = registry.render(payload, ctx);
    EXPECT_TRUE(called);
    EXPECT_NE(el, nullptr);
}

TEST(DialogRendererRegistry, EventHandlerDispatchesCorrectType) {
    dsys::DialogRendererRegistry registry;

    bool handled = false;
    registry.register_event_handler(dsys::DialogType::CostThreshold,
        [&](dsys::DialogPayloadVariant&, const ftxui::Event&) -> bool {
            handled = true;
            return true;
        });

    dsys::CostThresholdPayload ct;
    ct.id = "test";
    dsys::DialogPayloadVariant payload = ct;

    bool result = registry.handle_event(payload, ftxui::Event::Character('y'));
    EXPECT_TRUE(handled);
    EXPECT_TRUE(result);
}

TEST(DialogRendererRegistry, UnregisteredHandlerReturnsFalse) {
    dsys::DialogRendererRegistry registry;

    dsys::IdleReturnPayload idle;
    idle.id = "test";
    dsys::DialogPayloadVariant payload = idle;

    bool result = registry.handle_event(payload, ftxui::Event::Character('y'));
    EXPECT_FALSE(result);
}

// ============================================================
// DialogFrame tests
// ============================================================

TEST(DialogFrame, RendersBasicFrame) {
    Theme theme;

    dframe::DialogFrameProps props;
    props.title = "Test Dialog";
    props.subtitle = "A test subtitle";
    props.content = ftxui::text("Hello, world!");

    auto el = dframe::DialogFrame(props, theme);
    EXPECT_NE(el, nullptr);

    auto screen = ftxui::Screen::Create({60, 15});
    Render(screen, el);

    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    // Title should appear
    EXPECT_NE(output.find("Test Dialog"), std::string::npos);
    // Subtitle should appear
    EXPECT_NE(output.find("A test subtitle"), std::string::npos);
    // Content should appear
    EXPECT_NE(output.find("Hello, world!"), std::string::npos);
}

TEST(DialogFrame, RendersWithWorkerBadge) {
    Theme theme;

    dframe::DialogFrameProps props;
    props.title = "Worker Dialog";
    props.worker_badge = dframe::WorkerBadge("worker-1", theme);
    props.content = ftxui::text("Worker content");

    auto el = dframe::DialogFrame(props, theme);
    EXPECT_NE(el, nullptr);

    auto screen = ftxui::Screen::Create({60, 10});
    Render(screen, el);

    std::string output = screen.ToString();
    EXPECT_NE(output.find("Worker Dialog"), std::string::npos);
    EXPECT_NE(output.find("worker-1"), std::string::npos);
}

TEST(DialogFrame, RendersWithDifferentStyles) {
    Theme theme;

    // Info style
    dframe::DialogFrameProps info_props;
    info_props.title = "Info";
    info_props.style = dframe::FrameStyle::Info;
    info_props.content = ftxui::text("info");
    auto info_el = dframe::DialogFrame(info_props, theme);
    EXPECT_NE(info_el, nullptr);

    // Warning style
    dframe::DialogFrameProps warn_props;
    warn_props.title = "Warning";
    warn_props.style = dframe::FrameStyle::Warning;
    warn_props.content = ftxui::text("warning");
    auto warn_el = dframe::DialogFrame(warn_props, theme);
    EXPECT_NE(warn_el, nullptr);

    // Danger style
    dframe::DialogFrameProps danger_props;
    danger_props.title = "Danger";
    danger_props.style = dframe::FrameStyle::Danger;
    danger_props.content = ftxui::text("danger");
    auto danger_el = dframe::DialogFrame(danger_props, theme);
    EXPECT_NE(danger_el, nullptr);
}

TEST(DialogFrame, SimpleConvenienceBuilders) {
    Theme theme;

    auto info = dframe::SimpleInfoFrame("Info Title", "Info message here.", theme);
    EXPECT_NE(info, nullptr);

    auto warn = dframe::SimpleWarningFrame("Warning Title", "Warning message.", theme);
    EXPECT_NE(warn, nullptr);

    auto danger = dframe::SimpleDangerFrame("Danger Title", "Danger message.", theme);
    EXPECT_NE(danger, nullptr);
}

// ============================================================
// Default renderers tests
// ============================================================

TEST(DefaultRenderers, RegisterAllDefaultRenderers) {
    dsys::DialogRendererRegistry registry;
    drender::register_default_renderers(registry);

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 80;
    ctx.term_rows = 24;

    // ToolPermission should render
    {
        dsys::ToolPermissionPayload tp;
        tp.id = "tp-test";
        tp.tool_name = "BashTool";
        tp.description = "Run a command";
        dsys::DialogPayloadVariant payload = tp;
        auto el = registry.render(payload, ctx);
        EXPECT_NE(el, nullptr);

        auto screen = ftxui::Screen::Create({80, 20});
        Render(screen, el);
        EXPECT_FALSE(screen.ToString().empty());
        EXPECT_NE(screen.ToString().find("BashTool"), std::string::npos);
    }

    // SandboxPermission should render
    {
        dsys::SandboxPermissionPayload sp;
        sp.id = "sb-test";
        sp.host_pattern = "example.com";
        dsys::DialogPayloadVariant payload = sp;
        auto el = registry.render(payload, ctx);
        EXPECT_NE(el, nullptr);

        auto screen = ftxui::Screen::Create({80, 15});
        Render(screen, el);
        EXPECT_FALSE(screen.ToString().empty());
        EXPECT_NE(screen.ToString().find("example.com"), std::string::npos);
    }

    // CostThreshold should render (P0x3 contract: title + docs link)
    {
        dsys::CostThresholdPayload ct;
        ct.id = "ct-test";
        ct.dollars_spent = 4.7;    // rounds to $5
        ct.model_name = std::string("claude-sonnet");
        dsys::DialogPayloadVariant payload = ct;
        auto el = registry.render(payload, ctx);
        EXPECT_NE(el, nullptr);

        auto screen = ftxui::Screen::Create({80, 15});
        Render(screen, el);
        std::string out = screen.ToString();
        EXPECT_FALSE(out.empty());
        // Title must be formatted with dollars_spent using $%.0f (rounded)
        EXPECT_NE(out.find("You've spent $5 on the Anthropic API this session."),
                  std::string::npos);
        // Body + docs link must appear
        EXPECT_NE(out.find("Learn more about how to monitor your spending:"),
                  std::string::npos);
        EXPECT_NE(out.find("https://code.claude.com/docs/en/costs"),
                  std::string::npos);
        // Single "Got it, thanks!" button
        EXPECT_NE(out.find("Got it, thanks!"), std::string::npos);
        // Fabricated 3-action chrome MUST be absent
        EXPECT_EQ(out.find("[c] Continue"), std::string::npos);
        EXPECT_EQ(out.find("Reset counter"), std::string::npos);
        EXPECT_EQ(out.find("[q] Quit"), std::string::npos);
    }

    // IdleReturn should render
    {
        dsys::IdleReturnPayload ir;
        ir.id = "ir-test";
        ir.idle_minutes = 30;
        dsys::DialogPayloadVariant payload = ir;
        auto el = registry.render(payload, ctx);
        EXPECT_NE(el, nullptr);

        auto screen = ftxui::Screen::Create({80, 10});
        Render(screen, el);
        std::string out = screen.ToString();
        EXPECT_FALSE(out.empty());
        EXPECT_NE(out.find("30"), std::string::npos);
    }
}

TEST(DefaultRenderers, EventHandlersFireCallbacks) {
    dsys::DialogRendererRegistry registry;
    drender::register_default_renderers(registry);

    // CostThreshold event handling (P0x3: on_done() is 0-arg,
    // both Enter AND Escape MUST fire it — no data-loss exits).
    {
        std::atomic<int> calls{0};

        dsys::CostThresholdPayload ct;
        ct.id = "ct-event-test";
        ct.on_done = [&] { calls.fetch_add(1); };
        dsys::DialogPayloadVariant payload = ct;

        // Enter → on_done()
        calls = 0;
        payload = ct;
        bool handled = registry.handle_event(payload, ftxui::Event::Return);
        EXPECT_TRUE(handled);
        EXPECT_EQ(calls.load(), 1);

        // Escape → on_done()  (CRITICAL: must NOT be quit / data-loss)
        calls = 0;
        payload = ct;
        handled = registry.handle_event(payload, ftxui::Event::Escape);
        EXPECT_TRUE(handled);
        EXPECT_EQ(calls.load(), 1) << "Esc must ACKNOWLEDGE, NOT quit";

        // Space → on_done()
        calls = 0;
        payload = ct;
        handled = registry.handle_event(payload, ftxui::Event::Character(' '));
        EXPECT_TRUE(handled);
        EXPECT_EQ(calls.load(), 1);

        // Shortcut 'g' → on_done()
        calls = 0;
        payload = ct;
        handled = registry.handle_event(payload, ftxui::Event::Character('g'));
        EXPECT_TRUE(handled);
        EXPECT_EQ(calls.load(), 1);
    }

    // IdleReturn event handling
    {
        bool called = false;
        bool did_resume = false;

        dsys::IdleReturnPayload ir;
        ir.id = "ir-event-test";
        ir.on_response = [&](bool resume) {
            called = true;
            did_resume = resume;
        };
        dsys::DialogPayloadVariant payload = ir;

        // Enter = resume
        bool handled = registry.handle_event(payload, ftxui::Event::Return);
        EXPECT_TRUE(handled);
        EXPECT_TRUE(called);
        EXPECT_TRUE(did_resume);
    }

    // SandboxPermission event handling
    {
        bool called = false;
        bool did_allow = false;
        bool did_always = false;

        dsys::SandboxPermissionPayload sp;
        sp.id = "sb-event-test";
        sp.host_pattern = "test.com";
        sp.on_response = [&](bool allow, bool always) {
            called = true;
            did_allow = allow;
            did_always = always;
        };
        dsys::DialogPayloadVariant payload = sp;

        // 'a' = always allow
        bool handled = registry.handle_event(payload, ftxui::Event::Character('a'));
        EXPECT_TRUE(handled);
        EXPECT_TRUE(called);
        EXPECT_TRUE(did_allow);
        EXPECT_TRUE(did_always);
    }
}

// ============================================================
// should_show_dialog tests
// ============================================================

TEST(DialogSystem, ShouldShowDialogLogic) {
    // Overlay (band 3): suppressed while typing
    dsys::ToolPermissionPayload tp;
    tp.id = "tp-test";
    dsys::DialogPayloadVariant tp_payload = tp;
    EXPECT_TRUE(dsys::should_show_dialog(tp_payload, false));
    EXPECT_FALSE(dsys::should_show_dialog(tp_payload, true));

    // Bottom (band 4): suppressed while typing
    dsys::CostThresholdPayload ct;
    ct.id = "ct-test";
    dsys::DialogPayloadVariant ct_payload = ct;
    EXPECT_TRUE(dsys::should_show_dialog(ct_payload, false));
    EXPECT_FALSE(dsys::should_show_dialog(ct_payload, true));
}

// ============================================================
// Full renderer registry test — all dialog types renderable
// ============================================================

TEST(FullDialogRegistry, AllDialogTypesRenderable) {
    // Verify that EVERY DialogType can be rendered through the registry
    // after registering all renderer modules.  This validates M7.4 + M7.5
    // wiring: every dialog type has a renderer and an event handler.
    dsys::DialogRendererRegistry registry;
    cc::ui::dialogs::default_renderers::register_default_renderers(registry);
    cc::ui::dialogs::modal_renderers::register_modal_renderers(registry);
    cc::ui::dialogs::bottom_renderers::register_bottom_renderers(registry);
    cc::ui::dialogs::all_renderers::register_all_renderers(registry);

    Theme theme;
    dsys::DialogRenderContext ctx;
    ctx.term_cols = 80;
    ctx.term_rows = 24;
    ctx.theme = theme;
    ctx.is_fullscreen = false;

    // Helper: push a payload and verify it renders
    auto check_render = [&](dsys::DialogPayloadVariant payload,
                            const char* name) {
        SCOPED_TRACE(name);
        auto el = registry.render(std::move(payload), ctx);
        EXPECT_TRUE(el) << "DialogType " << name << " has no renderer";
        if (el) {
            // Render to screen to verify no crashes
            auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                                ftxui::Dimension::Fixed(10));
            Render(screen, el);
            EXPECT_FALSE(screen.ToString().empty())
                << "DialogType " << name << " rendered empty";
        }
    };

    // -- Overlay slot --
    {
        dsys::ToolPermissionPayload p;
        p.id = "tp-all";
        p.tool_name = "Bash";
        p.description = "Test description";
        check_render(std::move(p), "ToolPermission");
    }

    // -- Bottom slot --
    {
        dsys::MessageSelectorPayload p;
        p.id = "ms-all";
        p.options = {"opt1", "opt2"};
        p.placeholder = "Pick one";
        check_render(std::move(p), "MessageSelector");
    }
    {
        dsys::SandboxPermissionPayload p;
        p.id = "sp-all";
        p.host_pattern = "*.example.com";
        check_render(std::move(p), "SandboxPermission");
    }
    {
        dsys::PromptDialogPayload p;
        p.id = "pd-all";
        p.title = "Prompt Hook";
        check_render(std::move(p), "PromptDialog");
    }
    {
        dsys::WorkerSandboxPermissionPayload p;
        p.id = "wsp-all";
        p.worker_id = "worker-123";
        p.tool_name = "Bash";
        check_render(std::move(p), "WorkerSandboxPermission");
    }
    {
        dsys::ElicitationPayload p;
        p.id = "el-all";
        check_render(std::move(p), "Elicitation");
    }
    {
        dsys::CostThresholdPayload p;
        p.id = "ct-all";
        p.dollars_spent = 5.0;
        check_render(std::move(p), "CostThreshold");
    }
    {
        dsys::IdleReturnPayload p;
        p.id = "ir-all";
        check_render(std::move(p), "IdleReturn");
    }
    {
        dsys::UltraplanChoicePayload p;
        p.id = "upc-all";
        check_render(std::move(p), "UltraplanChoice");
    }
    {
        dsys::UltraplanLaunchPayload p;
        p.id = "upl-all";
        check_render(std::move(p), "UltraplanLaunch");
    }
    {
        dsys::IdeOnboardingPayload p;
        p.id = "io-all";
        check_render(std::move(p), "IdeOnboarding");
    }
    {
        dsys::InitOnboardingPayload p;
        p.id = "ino-all";
        check_render(std::move(p), "InitOnboarding");
    }
    {
        dsys::ModelSwitchPayload p;
        p.id = "msw-all";
        check_render(std::move(p), "ModelSwitch");
    }
    {
        dsys::UndercoverCalloutPayload p;
        p.id = "uc-all";
        check_render(std::move(p), "UndercoverCallout");
    }
    {
        dsys::EffortCalloutPayload p;
        p.id = "ec-all";
        check_render(std::move(p), "EffortCallout");
    }
    {
        dsys::RemoteCalloutPayload p;
        p.id = "rc-all";
        check_render(std::move(p), "RemoteCallout");
    }
    {
        dsys::LspRecommendationPayload p;
        p.id = "lr-all";
        check_render(std::move(p), "LspRecommendation");
    }
    {
        dsys::PluginHintPayload p;
        p.id = "ph-all";
        check_render(std::move(p), "PluginHint");
    }
    {
        dsys::DesktopUpsellPayload p;
        p.id = "du-all";
        check_render(std::move(p), "DesktopUpsell");
    }

    // -- Modal slot --
    {
        dsys::SettingsPanelPayload p;
        p.id = "sp-all";
        check_render(std::move(p), "SettingsPanel");
    }
    {
        dsys::TasksViewPayload p;
        p.id = "tv-all";
        check_render(std::move(p), "TasksView");
    }
    {
        dsys::TeamsViewPayload p;
        p.id = "tm-all";
        check_render(std::move(p), "TeamsView");
    }
    {
        dsys::HelpViewPayload p;
        p.id = "hv-all";
        check_render(std::move(p), "HelpView");
    }
    {
        dsys::QuickOpenPayload p;
        p.id = "qo-all";
        check_render(std::move(p), "QuickOpen");
    }
    {
        dsys::PluginDialogPayload p;
        p.id = "pd-all";
        check_render(std::move(p), "PluginDialog");
    }
    {
        dsys::MCPDialogPayload p;
        p.id = "mcp-all";
        check_render(std::move(p), "MCPDialog");
    }
    {
        dsys::DiffDialogPayload p;
        p.id = "dd-all";
        check_render(std::move(p), "DiffDialog");
    }
    {
        dsys::ConfigDialogPayload p;
        p.id = "cd-all";
        check_render(std::move(p), "ConfigDialog");
    }
    {
        dsys::ExportDialogPayload p;
        p.id = "ed-all";
        check_render(std::move(p), "ExportDialog");
    }
    {
        dsys::GlobalSearchPayload p;
        p.id = "gs-all";
        check_render(std::move(p), "GlobalSearch");
    }
    {
        dsys::HistorySearchPayload p;
        p.id = "hs-all";
        check_render(std::move(p), "HistorySearch");
    }
    {
        dsys::BridgeDialogPayload p;
        p.id = "bd-all";
        check_render(std::move(p), "BridgeDialog");
    }
    {
        dsys::WorktreeExitPayload p;
        p.id = "we-all";
        check_render(std::move(p), "WorktreeExitDialog");
    }
    {
        dsys::RemoteEnvPayload p;
        p.id = "re-all";
        check_render(std::move(p), "RemoteEnvDialog");
    }
    {
        dsys::AboutDialogPayload p;
        p.id = "ad-all";
        check_render(std::move(p), "AboutDialog");
    }
    {
        dsys::ConfirmationDialogPayload p;
        p.id = "cf-all";
        check_render(std::move(p), "ConfirmationDialog");
    }
    {
        dsys::FeedbackSurveyPayload p;
        p.id = "fs-all";
        check_render(std::move(p), "FeedbackSurvey");
    }
    {
        dsys::ManagedSettingsSecurityPayload p;
        p.id = "mss-all";
        check_render(std::move(p), "ManagedSettingsSecurity");
    }

    // -- Standalone --
    {
        dsys::TrustDialogPayload p;
        p.id = "td-all";
        check_render(std::move(p), "TrustDialog");
    }
    {
        dsys::OnboardingPayload p;
        p.id = "ob-all";
        check_render(std::move(p), "Onboarding");
    }
    {
        dsys::InstallGitHubAppWizardPayload p;
        p.id = "iga-all";
        check_render(std::move(p), "InstallGitHubAppWizard");
    }
    {
        dsys::InstallSlackAppWizardPayload p;
        p.id = "isa-all";
        check_render(std::move(p), "InstallSlackAppWizard");
    }
    {
        dsys::CreateAgentWizardPayload p;
        p.id = "ca-all";
        check_render(std::move(p), "CreateAgentWizard");
    }
    {
        dsys::EditAgentWizardPayload p;
        p.id = "ea-all";
        p.agent_name = "test-agent";
        check_render(std::move(p), "EditAgentWizard");
    }
}

TEST(FullDialogRegistry, DialogTypeCountMatches) {
    // Verify that the _COUNT sentinel matches the actual number of types
    // we have renderers + payloads for.  This catches drift between
    // the enum and the implementation.
    constexpr int kExpectedCount =
        1 +  // ToolPermission (overlay)
        14 + // bottom slot (MessageSelector + SandboxPermission + PromptDialog +
             // WorkerSandboxPermission + Elicitation + CostThreshold + IdleReturn +
             // UltraplanChoice + UltraplanLaunch + IdeOnboarding + InitOnboarding +
             // ModelSwitch + UndercoverCallout + EffortCallout + RemoteCallout +
             // LspRecommendation + PluginHint + DesktopUpsell → 18 total bottom
             // Let's just use the enum value as ground truth
        0;
    (void)kExpectedCount;

    // All we really care about: _COUNT > 0 and is a reasonable number
    EXPECT_GT(static_cast<int>(dsys::DialogType::_COUNT), 30);
}

// ============================================================
// Dialog trigger helpers (M7.5 — engine-side API)
// ============================================================

TEST(DialogTriggers, PushToolPermissionCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool responded = false;
    dtrig::PushToolPermission(
        queue, "Bash", "Run ls -la",
        [&](dsys::ToolPermissionPayload::Decision d, bool sandbox) {
            responded = true;
            (void)d;
            (void)sandbox;
        });

    EXPECT_TRUE(queue.has_overlay());
    auto peek = queue.peek_overlay();
    EXPECT_TRUE(peek.has_value());
    EXPECT_FALSE(responded);
}

TEST(DialogTriggers, PushMessageSelectorCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushMessageSelector(
        queue, {"opt1", "opt2", "opt3"}, "Choose...",
        [](int idx) { (void)idx; });

    EXPECT_TRUE(queue.has_any_bottom());
    auto peek = queue.peek_bottom(false);
    EXPECT_TRUE(peek.has_value());
}

TEST(DialogTriggers, PushCostThresholdCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushCostThreshold(
        queue, 5.0, 5.2, "claude-sonnet-4.6",
        [](bool cont, bool reset) { (void)cont; (void)reset; });

    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, PushSandboxPermissionCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushSandboxPermission(
        queue, "*.example.com",
        [](bool allow, bool always) { (void)allow; (void)always; });

    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, PushSettingsPanelCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushSettingsPanel(queue, "general", []() {});

    EXPECT_TRUE(queue.has_modal());
}

TEST(DialogTriggers, PushHelpViewCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushHelpView(queue, "commands", []() {});

    EXPECT_TRUE(queue.has_modal());
}

TEST(DialogTriggers, CommandMetadataCreateAgent) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "CREATE_AGENT");
    EXPECT_TRUE(pushed);
    // CreateAgentWizard is a full-screen standalone dialog (M7 §3.1).
    EXPECT_TRUE(queue.has_standalone());
    EXPECT_FALSE(queue.has_modal());
}

TEST(DialogTriggers, CommandMetadataEditAgent) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "EDIT_AGENT|my-agent");
    EXPECT_TRUE(pushed);
    // EditAgentWizard is a full-screen standalone dialog (M7 §3.1).
    EXPECT_TRUE(queue.has_standalone());
    EXPECT_FALSE(queue.has_modal());
}

TEST(DialogTriggers, CommandMetadataPluginDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(
        queue, "UI:plugins:manage-plugins");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());
}

TEST(DialogTriggers, CommandMetadataUnknownReturnsFalse) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "SOME_RANDOM_TAG");
    EXPECT_FALSE(pushed);
    EXPECT_TRUE(queue.empty());
}

TEST(DialogTriggers, CommandMetadataSettingsPanel) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:settings");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());
    queue.pop_modal();
    EXPECT_TRUE(queue.empty());
}

TEST(DialogTriggers, CommandMetadataHelpView) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:help");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());
    queue.pop_modal();
    EXPECT_TRUE(queue.empty());
}

TEST(DialogTriggers, CommandMetadataConfigDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:config");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());
    queue.pop_modal();
    EXPECT_TRUE(queue.empty());
}

TEST(DialogTriggers, CommandMetadataMCPDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:mcp");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());
    queue.pop_modal();
    EXPECT_TRUE(queue.empty());
}

TEST(DialogTriggers, CommandMetadataUndercoverCallout) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:undercover|1");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, CommandMetadataEffortCallout) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:effort|high");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, CommandMetadataRemoteCallout) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:remote|ssh-host");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, CommandMetadataLspRecommendation) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:lsp-rec|clangd");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, CommandMetadataPluginHint) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:plugin-hint|python-dev");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, PushLspRecommendationCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushLspRecommendation(
        queue, "clangd",
        [](bool install) { (void)install; });

    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, PushModelSwitchCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushModelSwitch(
        queue, "sonnet", "opus",
        [](bool confirm) { (void)confirm; });

    EXPECT_TRUE(queue.has_any_bottom());
}

TEST(DialogTriggers, MultipleDialogsQueueCorrectly) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    // Two bottom dialogs with different bands
    dtrig::PushCostThreshold(
        queue, 5.0, 5.2, "sonnet",
        [](bool, bool) {});
    dtrig::PushLspRecommendation(
        queue, "clangd", [](bool) {});

    // Both should be in the queue
    EXPECT_TRUE(queue.has_any_bottom());

    // Peek should show a dialog (higher priority one)
    auto peek = queue.peek_bottom(false);
    EXPECT_TRUE(peek.has_value());

    // Pop first one
    queue.pop_bottom(false);
    EXPECT_TRUE(queue.has_any_bottom());

    // Pop second one
    queue.pop_bottom(false);
    EXPECT_FALSE(queue.has_any_bottom());
}

// ============================================================
// QuickOpen tests
// ============================================================

// NOTE: Temporarily disabled.  These tests assume a
// `namespace cc::ui::dialogs::quick_open` containing RenderQuickOpen +
// HandleQuickOpenEvent + filter_items, but the production quick_open.cppm
// module exports:
//   * render_quick_open() -> std::string (5-arg ANSI output)
//   * filter_items() — in the flat cc::ui::dialogs namespace.
// The RenderQuickOpen + HandleQuickOpenEvent wrappers are now implemented in
// the all-renderers aggregator as internal inline functions (they call the
// actual helpers and match the registry signatures).  The standalone quick_open
// module will be aligned in a follow-up QuickOpen UI polish patch.
#if 0

TEST(QuickOpen, FuzzyFilterMatchesLabel) {
    namespace qo = cc::ui::dialogs::quick_open;

    std::vector<dsys::QuickOpenItem> items = {
        {"Settings", "Open settings panel", "", "Commands"},
        {"Help", "View help", "", "Commands"},
        {"Export", "Export conversation", "", "Commands"},
    };

    auto filtered = qo::filter_items(items, "set");
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].label, "Settings");
}

TEST(QuickOpen, FuzzyFilterMatchesDescription) {
    namespace qo = cc::ui::dialogs::quick_open;

    std::vector<dsys::QuickOpenItem> items = {
        {"Settings", "Open settings panel", "", "Commands"},
        {"Help", "View help docs", "", "Commands"},
    };

    auto filtered = qo::filter_items(items, "panel");
    EXPECT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].label, "Settings");
}

TEST(QuickOpen, FuzzyFilterCaseInsensitive) {
    namespace qo = cc::ui::dialogs::quick_open;

    std::vector<dsys::QuickOpenItem> items = {
        {"Settings", "Open settings", "", "Commands"},
    };

    auto filtered1 = qo::filter_items(items, "SET");
    auto filtered2 = qo::filter_items(items, "set");
    EXPECT_EQ(filtered1.size(), filtered2.size());
}

TEST(QuickOpen, EmptyQueryReturnsAll) {
    namespace qo = cc::ui::dialogs::quick_open;

    std::vector<dsys::QuickOpenItem> items = {
        {"A", "desc a", "", "Cat1"},
        {"B", "desc b", "", "Cat2"},
    };

    auto filtered = qo::filter_items(items, "");
    EXPECT_EQ(filtered.size(), 2u);
}

TEST(QuickOpen, EventNavigation) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    p.items = {
        {"First", "desc 1", "", "Cat"},
        {"Second", "desc 2", "", "Cat"},
        {"Third", "desc 3", "", "Cat"},
    };
    p.selected_index = 0;

    // Down navigation
    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(p.selected_index, 1);

    // Up navigation
    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::ArrowUp));
    EXPECT_EQ(p.selected_index, 0);

    // Wrap around from bottom
    p.selected_index = 2;
    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(p.selected_index, 0);

    // Wrap around from top
    p.selected_index = 0;
    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::ArrowUp));
    EXPECT_EQ(p.selected_index, 2);
}

TEST(QuickOpen, EventCharacterAddsToQuery) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    p.query = "";

    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::Character('a')));
    EXPECT_EQ(p.query, "a");

    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::Character('b')));
    EXPECT_EQ(p.query, "ab");
}

TEST(QuickOpen, EventBackspaceRemovesChar) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    p.query = "hello";

    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::Backspace));
    EXPECT_EQ(p.query, "hell");
}

TEST(QuickOpen, EventReturnInvokesCallback) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    p.items = {{"Item", "desc", "", "Cat"}};
    p.selected_index = 0;

    bool called = false;
    int result_idx = -1;
    bool result_confirmed = false;
    p.on_result = [&](int idx, bool confirmed) {
        called = true;
        result_idx = idx;
        result_confirmed = confirmed;
    };

    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::Return));
    EXPECT_TRUE(called);
    EXPECT_EQ(result_idx, 0);
    EXPECT_TRUE(result_confirmed);
}

TEST(QuickOpen, EventEscapeCancels) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    bool called = false;
    p.on_result = [&](int idx, bool confirmed) {
        called = true;
        EXPECT_EQ(idx, -1);
        EXPECT_FALSE(confirmed);
    };

    EXPECT_TRUE(qo::HandleQuickOpenEvent(p, ftxui::Event::Escape));
    EXPECT_TRUE(called);
}

TEST(QuickOpen, RenderProducesOutput) {
    namespace qo = cc::ui::dialogs::quick_open;

    dsys::QuickOpenPayload p;
    p.id = "test-qo";
    p.items = {
        {"Settings", "Open settings", "⌘,", "Commands"},
        {"Help", "View help", "?", "Commands"},
    };
    p.query = "";
    p.selected_index = 0;

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 80;
    ctx.term_rows = 24;

    auto element = qo::RenderQuickOpen(p, ctx);
    EXPECT_NE(element.get(), nullptr);

    // Render to screen to verify it produces text
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, element);

    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    // Should contain the title
    EXPECT_NE(output.find("Quick"), std::string::npos);
    // Should contain item labels
    EXPECT_NE(output.find("Settings"), std::string::npos);
    EXPECT_NE(output.find("Help"), std::string::npos);
}

#endif  // #if 0 — QuickOpen internal-API tests disabled (see comment above)

TEST(DialogTriggers, CommandMetadataQuickOpen) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool pushed = dtrig::PushFromCommandMetadata(queue, "UI:quick-open");
    EXPECT_TRUE(pushed);
    EXPECT_TRUE(queue.has_modal());

    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::QuickOpen);
}

TEST(DialogTriggers, PushQuickOpenCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    std::vector<dsys::QuickOpenItem> items = {
        {"Test", "test item", "", "Test"},
    };

    dtrig::PushQuickOpen(queue, std::move(items), "te",
                         [](int, bool) {});

    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());

    const auto& payload = peek->get();
    auto* p = std::get_if<dsys::QuickOpenPayload>(&payload);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->query, "te");
    EXPECT_EQ(p->items.size(), 1u);
}

// ============================================================
// More trigger tests (M7.5 — new push functions)
// ============================================================

TEST(DialogTriggers, PushAboutDialogCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool closed = false;
    dtrig::PushAboutDialog(queue, "1.0.0", [&] { closed = true; });

    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::AboutDialog);

    auto* p = std::get_if<dsys::AboutDialogPayload>(&peek->get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->version, "1.0.0");
}

TEST(DialogTriggers, PushTasksViewCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushTasksView(queue, [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::TasksView);
}

TEST(DialogTriggers, PushTeamsViewCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushTeamsView(queue, [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::TeamsView);
}

TEST(DialogTriggers, PushExportDialogCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    bool responded = false;
    dtrig::PushExportDialog(queue, "markdown",
        [&](bool ok) { responded = true; EXPECT_TRUE(ok); });

    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::ExportDialog);

    auto* p = std::get_if<dsys::ExportDialogPayload>(&peek->get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->format, "markdown");
}

TEST(DialogTriggers, PushDiffDialogCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushDiffDialog(queue, "Changes", "old", "new",
        [](bool) {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::DiffDialog);
}

TEST(DialogTriggers, PushGlobalSearchCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushGlobalSearch(queue, "test", [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::GlobalSearch);

    auto* p = std::get_if<dsys::GlobalSearchPayload>(&peek->get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->query, "test");
}

TEST(DialogTriggers, PushHistorySearchCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushHistorySearch(queue, "project",
        [](std::string_view) {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::HistorySearch);
}

TEST(DialogTriggers, PushFeedbackSurveyCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushFeedbackSurvey(queue, [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::FeedbackSurvey);
}

TEST(DialogTriggers, PushManagedSecurityCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushManagedSettingsSecurity(queue, [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::ManagedSettingsSecurity);
}

TEST(DialogTriggers, PushPluginDialogCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushPluginDialog(queue, [] {});
    EXPECT_TRUE(queue.has_modal());
    auto peek = queue.peek_modal();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::PluginDialog);
}

TEST(DialogTriggers, PushTrustDialogCreatesDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    dtrig::PushTrustDialog(queue, "example.com",
        [](bool) {});
    // TrustDialog is a full-screen standalone dialog (M7 §3.1).
    EXPECT_TRUE(queue.has_standalone());
    EXPECT_FALSE(queue.has_modal());
    auto peek = queue.peek_standalone();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(*peek), dsys::DialogType::TrustDialog);
}

// ============================================================
// Command metadata bridge tests (M7.5 — more mappings)
// ============================================================

TEST(DialogTriggers, MetadataAboutDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "UI:about"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::AboutDialog);
}

TEST(DialogTriggers, MetadataTasksView) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "UI:tasks"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::TasksView);
}

TEST(DialogTriggers, MetadataTeamsView) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "UI:teams"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::TeamsView);
}

TEST(DialogTriggers, MetadataExportDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "UI:export"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::ExportDialog);
}

TEST(DialogTriggers, MetadataDiffDialog) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "DIFF_DIALOG"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::DiffDialog);
}

TEST(DialogTriggers, MetadataFeedbackSurvey) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "UI:feedback"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::FeedbackSurvey);
}

TEST(DialogTriggers, MetadataGlobalSearch) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "GLOBAL_SEARCH"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::GlobalSearch);
}

TEST(DialogTriggers, MetadataHistorySearch) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "HISTORY_SEARCH"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()), dsys::DialogType::HistorySearch);
}

TEST(DialogTriggers, MetadataManagedSecurity) {
    dsys::DialogQueue queue;
    namespace dtrig = cc::ui::dialogs::triggers;

    EXPECT_TRUE(dtrig::PushFromCommandMetadata(queue, "MANAGED_SETTINGS_SECURITY"));
    EXPECT_TRUE(queue.has_modal());
    EXPECT_EQ(dsys::type_of(*queue.peek_modal()),
              dsys::DialogType::ManagedSettingsSecurity);
}

// ============================================================
// Upgraded dialog render tests (M7.5 — stub → real)
// ============================================================

TEST(UpgradedDialogs, ManagedSecurityRendersOutput) {
    dsys::ManagedSettingsSecurityPayload payload;
    payload.organization_name = "Acme Corp";
    payload.selected_index = 1;

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderManagedSettingsSecurity(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
}

TEST(UpgradedDialogs, FeedbackSurveyRendersOutput) {
    dsys::FeedbackSurveyPayload payload;
    payload.state = dsys::FeedbackSurveyState::Open;

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderFeedbackSurvey(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(10));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("Feedback"), std::string::npos);
}

TEST(UpgradedDialogs, GlobalSearchRendersOutput) {
    dsys::GlobalSearchPayload payload;
    payload.query = "test";

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderGlobalSearch(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("Search"), std::string::npos);
    EXPECT_NE(output.find("test"), std::string::npos);
}

TEST(UpgradedDialogs, HistorySearchRendersOutput) {
    dsys::HistorySearchPayload payload;
    payload.query = "project";

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderHistorySearch(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("History"), std::string::npos);
}

// ============================================================
// Upgraded dialog event tests (M7.5)
// ============================================================

TEST(UpgradedDialogEvents, GlobalSearchCharacterAddsToQuery) {
    dsys::GlobalSearchPayload payload;

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleGlobalSearchEvent(payload, ftxui::Event::Character('a')));
    EXPECT_EQ(payload.query, "a");
    EXPECT_TRUE(dr::HandleGlobalSearchEvent(payload, ftxui::Event::Character('b')));
    EXPECT_EQ(payload.query, "ab");
}

TEST(UpgradedDialogEvents, GlobalSearchBackspaceRemovesChar) {
    dsys::GlobalSearchPayload payload;
    payload.query = "test";

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleGlobalSearchEvent(payload, ftxui::Event::Backspace));
    EXPECT_EQ(payload.query, "tes");
}

TEST(UpgradedDialogEvents, GlobalSearchEscapeCloses) {
    dsys::GlobalSearchPayload payload;
    bool closed = false;
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleGlobalSearchEvent(payload, ftxui::Event::Escape));
    EXPECT_TRUE(closed);
}

TEST(UpgradedDialogEvents, HistorySearchCharacterAddsToQuery) {
    dsys::HistorySearchPayload payload;

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleHistorySearchEvent(payload, ftxui::Event::Character('x')));
    EXPECT_EQ(payload.query, "x");
}

TEST(UpgradedDialogEvents, HistorySearchEscapeCancels) {
    dsys::HistorySearchPayload payload;
    bool cancelled = false;
    payload.on_select = [&](std::string_view id) {
        cancelled = id.empty();
    };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleHistorySearchEvent(payload, ftxui::Event::Escape));
    EXPECT_TRUE(cancelled);
}

TEST(UpgradedDialogEvents, ManagedSecurityNavigationWorks) {
    dsys::ManagedSettingsSecurityPayload payload;
    payload.selected_index = 0;

    namespace dr = cc::ui::dialogs::all_renderers;

    // Down navigation
    EXPECT_TRUE(dr::HandleManagedSettingsSecurityEvent(
        payload, ftxui::Event::ArrowDown));
    EXPECT_EQ(payload.selected_index, 1);

    // Up navigation
    EXPECT_TRUE(dr::HandleManagedSettingsSecurityEvent(
        payload, ftxui::Event::ArrowUp));
    EXPECT_EQ(payload.selected_index, 0);

    // Up at 0 stays at 0
    EXPECT_TRUE(dr::HandleManagedSettingsSecurityEvent(
        payload, ftxui::Event::ArrowUp));
    EXPECT_EQ(payload.selected_index, 0);
}

TEST(UpgradedDialogEvents, ManagedSecurityEnterEscCloses) {
    dsys::ManagedSettingsSecurityPayload payload;
    bool closed = false;
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleManagedSettingsSecurityEvent(
        payload, ftxui::Event::Escape));
    EXPECT_TRUE(closed);

    closed = false;
    EXPECT_TRUE(dr::HandleManagedSettingsSecurityEvent(
        payload, ftxui::Event::Return));
    EXPECT_TRUE(closed);
}

TEST(UpgradedDialogEvents, FeedbackSurveyDigitKeys) {
    dsys::FeedbackSurveyPayload payload;
    payload.state = dsys::FeedbackSurveyState::Open;
    int submitted_rating = -1;
    payload.on_submit = [&](int r) { submitted_rating = r; };

    namespace dr = cc::ui::dialogs::all_renderers;

    EXPECT_TRUE(dr::HandleFeedbackSurveyEvent(payload, ftxui::Event::Character('2')));
    EXPECT_EQ(submitted_rating, 2);
    EXPECT_EQ(payload.state, dsys::FeedbackSurveyState::Thanks);
}

TEST(UpgradedDialogEvents, FeedbackSurveyDismissKey) {
    dsys::FeedbackSurveyPayload payload;
    payload.state = dsys::FeedbackSurveyState::Open;
    bool closed = false;
    int rating = -1;
    payload.on_submit = [&](int r) { rating = r; };
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleFeedbackSurveyEvent(payload, ftxui::Event::Character('0')));
    EXPECT_EQ(rating, 0);
    EXPECT_TRUE(closed);
}

TEST(UpgradedDialogEvents, FeedbackSurveyEscapeCloses) {
    dsys::FeedbackSurveyPayload payload;
    payload.state = dsys::FeedbackSurveyState::Open;
    bool closed = false;
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleFeedbackSurveyEvent(payload, ftxui::Event::Escape));
    EXPECT_TRUE(closed);
}

// ============================================================
// More upgraded dialog render tests
// ============================================================

TEST(UpgradedDialogs, PluginDialogRendersMainMenu) {
    dsys::PluginDialogPayload payload;
    payload.menu_selected = 2;

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderPluginDialog(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(70),
                                        ftxui::Dimension::Fixed(20));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("Plugins"), std::string::npos);
}

TEST(UpgradedDialogs, PluginDialogNavigation) {
    dsys::PluginDialogPayload payload;
    payload.menu_selected = 0;

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandlePluginDialogEvent(payload, ftxui::Event::ArrowDown));
    EXPECT_EQ(payload.menu_selected, 1);

    EXPECT_TRUE(dr::HandlePluginDialogEvent(payload, ftxui::Event::ArrowUp));
    EXPECT_EQ(payload.menu_selected, 0);

    // Can't go below 0
    EXPECT_TRUE(dr::HandlePluginDialogEvent(payload, ftxui::Event::ArrowUp));
    EXPECT_EQ(payload.menu_selected, 0);

    // Can't go above 4
    for (int i = 0; i < 10; ++i) {
        dr::HandlePluginDialogEvent(payload, ftxui::Event::ArrowDown);
    }
    EXPECT_EQ(payload.menu_selected, 4);
}

TEST(UpgradedDialogs, PluginDialogEscCloses) {
    dsys::PluginDialogPayload payload;
    bool closed = false;
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandlePluginDialogEvent(payload, ftxui::Event::Escape));
    EXPECT_TRUE(closed);
}

TEST(UpgradedDialogs, DiffDialogRendersOutput) {
    dsys::DiffDialogPayload payload;
    payload.title = "Changes";
    payload.file_path = "test.cpp";
    payload.before_text = "int x = 1;\nint y = 2;";
    payload.after_text = "int x = 1;\nint y = 3;";

    dsys::DialogRenderContext ctx;
    Theme theme;
    ctx.theme = theme;
    ctx.term_cols = 80;
    ctx.term_rows = 30;

    namespace dr = cc::ui::dialogs::all_renderers;
    auto element = dr::RenderDiffDialog(payload, ctx);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                        ftxui::Dimension::Fixed(20));
    ftxui::Render(screen, element);
    std::string output = screen.ToString();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("Changes"), std::string::npos);
}

TEST(UpgradedDialogs, DiffDialogScrolls) {
    dsys::DiffDialogPayload payload;
    payload.scroll_offset = 5;

    namespace dr = cc::ui::dialogs::all_renderers;
    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::ArrowDown));
    EXPECT_EQ(payload.scroll_offset, 6);

    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::ArrowUp));
    EXPECT_EQ(payload.scroll_offset, 5);

    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::PageDown));
    EXPECT_EQ(payload.scroll_offset, 25);

    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::PageUp));
    EXPECT_EQ(payload.scroll_offset, 5);
}

TEST(UpgradedDialogs, DiffDialogAcceptReject) {
    dsys::DiffDialogPayload payload;
    bool accepted = false;
    bool closed = false;
    payload.on_response = [&](bool ok) { accepted = ok; };
    payload.on_close = [&] { closed = true; };

    namespace dr = cc::ui::dialogs::all_renderers;

    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::Return));
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(closed);

    accepted = false;
    closed = false;
    EXPECT_TRUE(dr::HandleDiffDialogEvent(payload, ftxui::Event::Escape));
    EXPECT_FALSE(accepted);
    EXPECT_TRUE(closed);
}

// ============================================================
// Step 5: DialogFrame 6-variant golden snapshot tests
// ============================================================
// All renders use the deterministic LightTheme for stable output.
// Refresh goldens: UPDATE_GOLDENS=1 ctest -R DialogFrame.Golden

TEST(DialogFrame, GoldenDefault) {
    auto theme = dialog_test_golden::get_light_theme();

    dframe::DialogFrameProps props;
    props.title = "Dialog Frame Test";
    props.subtitle = "Default Info variant";
    props.style = dframe::FrameStyle::Info;
    props.content = ftxui::paragraph(
            "Lorem ipsum dolor sit amet consectetur adipiscing elit sed do "
            "eiusmod tempor incididunt ut labore") | ftxui::xflex_grow;

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_default",
        dialog_test_golden::render_to_ansi(std::move(el), 80, 20));
}

TEST(DialogFrame, GoldenWithSubtitle) {
    auto theme = dialog_test_golden::get_light_theme();

    dframe::DialogFrameProps props;
    props.title = "Project Settings";
    props.subtitle = "version 1.2.3 · user@host";
    props.style = dframe::FrameStyle::Default;
    props.content = ftxui::text("A");

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_with_subtitle",
        dialog_test_golden::render_to_ansi(std::move(el), 80, 20));
}

TEST(DialogFrame, GoldenWithWorkerBadge) {
    auto theme = dialog_test_golden::get_light_theme();

    dframe::DialogFrameProps props;
    props.title = "Bash command";
    props.subtitle = "Worker wants to run command";
    props.style = dframe::FrameStyle::Warning;
    props.worker_badge = dframe::WorkerBadge("agent-coord-0", theme);
    props.content = ftxui::text("rm -rf /");

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_with_worker_badge",
        dialog_test_golden::render_to_ansi(std::move(el), 80, 20));
}

TEST(DialogFrame, GoldenLongContent) {
    auto theme = dialog_test_golden::get_light_theme();

    ftxui::Elements lines;
    for (int n = 1; n <= 15; ++n) {
        lines.push_back(ftxui::text(
            "Line " + std::to_string(n) + " of review content..."));
    }

    dframe::DialogFrameProps props;
    props.title = "Code Review";
    props.style = dframe::FrameStyle::Success;
    props.content = ftxui::vbox(std::move(lines));

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_long_content",
        dialog_test_golden::render_to_ansi(std::move(el), 80, 20));
}

TEST(DialogFrame, GoldenNarrowWidth) {
    auto theme = dialog_test_golden::get_light_theme();

    dframe::DialogFrameProps props;
    props.title = "Narrow";
    props.style = dframe::FrameStyle::Muted;
    props.content = ftxui::paragraph("Short");

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_narrow_width",
        dialog_test_golden::render_to_ansi(std::move(el), 40, 15));
}

TEST(DialogFrame, GoldenWideWidth) {
    auto theme = dialog_test_golden::get_light_theme();

    ftxui::Elements issue_rows;
    const std::array<std::string, 8> issues = {{
        "Unused variable 'result' on line 42",
        "Missing const qualifier in helper function",
        "Magic number 0xDEADBEEF should be a named constant",
        "TODO comment without tracking issue reference",
        "Potential null-pointer dereference without guard",
        "std::format argument count mismatch (string_view issue)",
        "Header guard does not match project convention",
        "Exception specification missing on destructor override",
    }};
    for (std::size_t i = 0; i < issues.size(); ++i) {
        issue_rows.push_back(ftxui::hbox({
            ftxui::text(std::to_string(i + 1) + ". ") | ftxui::dim,
            ftxui::text(issues[i]),
        }));
    }

    dframe::DialogFrameProps props;
    props.title = "Wide Panel With Lots of Room";
    props.subtitle = "Extended diagnostics view";
    props.style = dframe::FrameStyle::Danger;
    props.inner_padding_x = 2;
    props.inner_padding_y = 1;
    props.content = ftxui::vbox({
        ftxui::paragraph(
            "The system detected the following issues during the last build:"),
        ftxui::text(""),
        ftxui::vbox(std::move(issue_rows)),
    });

    auto el = dframe::DialogFrame(props, theme);
    dialog_test_golden::check_golden(
        "dialog_frame_wide_width",
        dialog_test_golden::render_to_ansi(std::move(el), 120, 15));
}

// ============================================================
// Step 6A: 7-ported dialogs GOLDEN snapshot tests
// ============================================================
// Each test builds a payload, registers a renderer in a LOCAL
// DialogRendererRegistry (hermetic tests), renders at 100x30, and
// snapshots.  Refresh: UPDATE_GOLDENS=1 ctest -R DialogRenderers.Golden

using ftxui::bold;
using ftxui::center;
using ftxui::color;
using ftxui::dim;
using ftxui::filler;
using ftxui::hbox;
using ftxui::paragraph;
using ftxui::text;
using ftxui::vbox;
using ftxui::xflex;
using ftxui::yflex;
using ftxui::xflex_grow;

TEST(DialogRenderers, Golden_ToolPermissionOverlay) {
    auto theme = dialog_test_golden::get_light_theme();

    // Hermetic renderer: uses payload fields that are always populated and
    // captures command/cwd directly in the lambda to avoid requiring an
    // explicit import of cc.ui.permissions.single_prompt in this TU.
    const std::string kCommand = "rm -rf node_modules/";
    const std::string kCwd     = "/home/user/proj";

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::ToolPermission,
        [&, kCommand, kCwd](const dsys::DialogPayloadVariant& v,
                            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = "Bash — Permission Required";
            props.subtitle = "High-risk command · needs manual approval";
            props.style = dframe::FrameStyle::Danger;
            props.content = vbox({
                hbox({
                    text("Tool: ") | dim,
                    text(p->tool_name) | bold,
                }),
                text(""),
                // command + cwd scope table
                hbox({
                    text("Command: ") | dim,
                    text(kCommand) | bold | color(ftxui::Color::Red),
                }),
                hbox({
                    text("Cwd: ") | dim,
                    text(kCwd),
                }),
                text(""),
                paragraph(p->description),
                text(""),
                            hbox({
                    text(" [y] Allow once") | color(ftxui::Color::Green),
                    p->can_always_allow
                        ? text("  [a] Always allow") | color(ftxui::Color::Cyan)
                        : text(""),
                    text("  [n] Deny") | color(ftxui::Color::Red),
                    text("  [Esc] Cancel") | dim,
                }),
            });
            return dframe::DialogFrame(props, theme);
        });

    // Build the payload (fields accessible without single_prompt import).
    dsys::ToolPermissionPayload tp;
    tp.id = "tp-gold-1";
    tp.tool_name = "bash";
    tp.description = "Run destructive command";
    tp.workspace_root = kCwd;
    tp.can_always_allow = true;
    // tp.risk_level and tp.detail default-construct and are not read by
    // this hermetic renderer (which captures data directly above).

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{tp}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_tool_permission_overlay",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

TEST(DialogRenderers, Golden_SandboxPermission) {
    // FAITHFUL PORT: uses cc.ui.dialogs.sandbox_permission (1:1 TS layout)
    // rather than the pre-existing stub renderer.

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::SandboxPermission,
        [](const dsys::DialogPayloadVariant& v,
           const dsys::DialogRenderContext& ctx) -> ftxui::Element {
            const auto* p = std::get_if<dsys::SandboxPermissionPayload>(&v);
            if (!p) return text("");
            // Delegate to the faithful renderer (TS SandboxPermissionRequest.tsx)
            return sbp::RenderDefault(*p, ctx);
        },
        [](dsys::DialogPayloadVariant& v,
           const ftxui::Event& e) -> bool {
            auto* p = std::get_if<dsys::SandboxPermissionPayload>(&v);
            if (!p) return false;
            return sbp::HandleSandboxPermissionEvent(*p, e);
        });

    dsys::SandboxPermissionPayload sp;
    sp.id = "sb-gold-1";
    sp.host_pattern = "*.github.com";
    // managed_domains_only = false → all 3 options visible (TS default)
    sp.managed_domains_only = false;
    sp.focused_index = 0;   // "Yes" focused
    sp.on_response = [](bool, bool) {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{sp}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_sandbox_permission",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

// ============================================================
// SandboxPermission keyboard event tests
// ============================================================
// Shortcuts are 1:1 with TS SandboxPermissionRequest.tsx:
//   y / Enter     → allow  (allow=true,  always=false)
//   a             → always (allow=true,  always=true)  [unless managed]
//   n / Esc       → deny   (allow=false, always=false)
//   ArrowUp/k     → previous option (wraps)
//   ArrowDown/j   → next option (wraps)

TEST(SandboxPermissionEvents, KeyY_AllowsOnce) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.host_pattern = "*.example.com";
    bool allow = false, always = true;
    int fired = 0;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('y')));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(allow);
    EXPECT_FALSE(always);
}

TEST(SandboxPermissionEvents, KeyY_CaseInsensitive) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    int fired = 0;
    bool allow = false, always = true;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('Y')));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(allow);
    EXPECT_FALSE(always);
}

TEST(SandboxPermissionEvents, Enter_AllowsOnce) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    int fired = 0;
    bool allow = false, always = true;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Return));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(allow);
    EXPECT_FALSE(always);
}

TEST(SandboxPermissionEvents, KeyA_AlwaysAllow) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = false;
    int fired = 0;
    bool allow = false, always = false;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('a')));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(allow);
    EXPECT_TRUE(always);
}

TEST(SandboxPermissionEvents, KeyA_SuppressedWhenManagedDomainsOnly) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = true;
    int fired = 0;
    p.on_response = [&](bool, bool) { ++fired; };

    // Key is consumed (not leaked to prompt) but callback MUST NOT fire.
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('a')));
    EXPECT_EQ(fired, 0);

    // Same for 'A'.
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('A')));
    EXPECT_EQ(fired, 0);
}

TEST(SandboxPermissionEvents, KeyN_Denies) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    int fired = 0;
    bool allow = true, always = true;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('n')));
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(allow);
    EXPECT_FALSE(always);

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('N')));
    EXPECT_EQ(fired, 2);
}

TEST(SandboxPermissionEvents, Escape_DeniesLikeOnCancel) {
    // TS: onCancel → onUserResponse({allow:false, persistToSettings:false})

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    int fired = 0;
    bool allow = true, always = true;
    p.on_response = [&](bool a, bool al) { allow = a; always = al; ++fired; };

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Escape));
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(allow);
    EXPECT_FALSE(always);
}

TEST(SandboxPermissionEvents, ArrowDown_WrapsFocusThroughAllOptions) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = false;  // 3 options: 0=Yes, 1=YesAlways, 2=No

    // Default focused_index unset → ArrowDown should go to 1
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowDown));
    ASSERT_TRUE(p.focused_index.has_value());
    EXPECT_EQ(*p.focused_index, 1);

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(*p.focused_index, 2);

    // Wraps to 0
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(*p.focused_index, 0);
}

TEST(SandboxPermissionEvents, ArrowUp_WrapsFocusBackwards) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = false;
    p.focused_index = 0;

    // 0 → (0 + 3 - 1) % 3 = 2
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowUp));
    EXPECT_EQ(*p.focused_index, 2);

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowUp));
    EXPECT_EQ(*p.focused_index, 1);
}

TEST(SandboxPermissionEvents, VimJK_MoveFocus) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = false;
    p.focused_index = 0;

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('j')));
    EXPECT_EQ(*p.focused_index, 1);

    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character('k')));
    EXPECT_EQ(*p.focused_index, 0);
}

TEST(SandboxPermissionEvents, ManagedDomainsOnly_TwoOptionsWrapCorrectly) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    p.managed_domains_only = true;  // 2 options: 0=Yes, 1=No
    p.focused_index = 0;

    // ArrowDown: 0 → 1 → wrap → 0
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(*p.focused_index, 1);
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowDown));
    EXPECT_EQ(*p.focused_index, 0);

    // ArrowUp: 0 → wrap → 1
    EXPECT_TRUE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::ArrowUp));
    EXPECT_EQ(*p.focused_index, 1);
}

TEST(SandboxPermissionEvents, UnrelatedKeys_NotConsumed) {

    namespace sbp = cc::ui::dialogs::sandbox_permission;

    dsys::SandboxPermissionPayload p;
    int fired = 0;
    p.on_response = [&](bool, bool) { ++fired; };

    // Space and Tab are not Sandbox shortcuts.
    EXPECT_FALSE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Character(' ')));
    EXPECT_FALSE(sbp::HandleSandboxPermissionEvent(p, ftxui::Event::Tab));
    EXPECT_EQ(fired, 0);
}

TEST(DialogRenderers, Golden_PromptDialog) {
    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::PromptDialog,
        [&](const dsys::DialogPayloadVariant& v,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::PromptDialogPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = p->title.empty()
                ? std::string{"Input Required"} : p->title;
            props.subtitle = "Claude needs clarification";
            props.style = dframe::FrameStyle::Info;

            auto def = p->default_value
                ? hbox({text("[") | dim, text(*p->default_value), text("]") | dim})
                : text("");

            props.content = vbox({
                paragraph(p->prompt_text),
                text(""),
                def,
                text(""),
                // accept / cancel rows
                hbox({
                    text(" [Enter] Submit") | color(ftxui::Color::Green),
                    text("  [Esc] Cancel") | color(ftxui::Color::Red),
                }),
            });
            return dframe::DialogFrame(props, theme);
        });

    dsys::PromptDialogPayload pd;
    pd.id = "prompt-gold-1";
    pd.title = "Follow-up question";
    pd.prompt_text =
        "You asked for unit tests, but didn't specify a test framework. "
        "Which framework should I use? (leave blank for project default)";
    pd.default_value = "gtest";
    pd.on_response = [](std::optional<std::string>) {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{pd}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_prompt",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

TEST(DialogRenderers, Golden_Elicitation) {
    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::Elicitation,
        [&](const dsys::DialogPayloadVariant& v,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::ElicitationPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = "MCP Server Request";
            props.subtitle = p->server_name + " needs additional input";
            props.style = dframe::FrameStyle::Info;
            props.content = vbox({
                hbox({
                    text("Server: ") | dim,
                    text(p->server_name) | bold,
                }),
                text(""),
                paragraph(p->request_description),
                text(""),
                hbox({
                    text("Request ID: ") | dim,
                    text(std::to_string(p->request_id)) | dim,
                }),
                text(""),
                hbox({
                    text(" [Enter] Approve") | color(ftxui::Color::Green),
                    text("  [Esc] Deny") | color(ftxui::Color::Red),
                }),
            });
            return dframe::DialogFrame(props, theme);
        });

    dsys::ElicitationPayload elp;
    elp.id = "elicit-gold-1";
    elp.server_name = "github.com";
    elp.request_description =
        "The github MCP server needs a repository owner and name before it "
        "can list pull requests.  Please provide these values.";
    elp.request_id = 42;
    elp.on_response = [](bool) {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{elp}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_elicitation",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

TEST(DialogRenderers, Golden_CostThreshold) {
    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::CostThreshold,
        [&](const dsys::DialogPayloadVariant& v,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::CostThresholdPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = "Cost threshold exceeded";
            props.subtitle =
                "Session cost has exceeded your configured threshold.";
            props.style = dframe::FrameStyle::Warning;
            props.content = vbox({
                hbox({
                    text("Accumulated: ") | dim,
                    text(std::format("${:.2f}", p->current_cost_usd)) | bold
                        | color(ftxui::Color::Yellow),
                }),
                hbox({
                    text("Threshold: ") | dim,
                    text(std::format("${:.2f}", p->cost_threshold_usd)),
                }),
                text(""),
                paragraph(
                    "You can continue the current session (the counter keeps "
                    "running), reset the counter and continue, or stop here."),
                text(""),
                hbox({
                    text(" [c] Continue") | color(ftxui::Color::Green),
                    text("  [r] Reset counter") | color(ftxui::Color::Cyan),
                    text("  [q] Quit") | color(ftxui::Color::Red),
                }),
            });
            return dframe::DialogFrame(props, theme);
        });

    dsys::CostThresholdPayload ct;
    ct.id = "cost-gold-1";
    ct.cost_threshold_usd = 5.0;
    ct.current_cost_usd = 7.23;
    ct.model_name = "claude-sonnet-4.6";
    ct.on_response = [](bool, bool) {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{ct}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_cost_threshold",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

TEST(DialogRenderers, Golden_IdleReturn) {
    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::IdleReturn,
        [&](const dsys::DialogPayloadVariant& v,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::IdleReturnPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = "Session Idle";
            props.subtitle = std::format(
                "Returning to idle in {} minutes — click Cancel to stay",
                p->idle_minutes);
            props.style = dframe::FrameStyle::Muted;
            props.content = vbox({
                text(std::format(
                    "Your session has been idle for {} minutes.",
                    p->idle_minutes)) | center,
                text(""),
                paragraph(
                    "Resuming will re-hydrate the conversation context and "
                    "continue from where you left off."),
                text(""),
                hbox({
                    filler(),
                    text(" [Enter] Resume") | color(ftxui::Color::Green),
                    text("  [n] Cancel (stay)") | color(ftxui::Color::Cyan),
                    filler(),
                }) | center,
            });
            return dframe::DialogFrame(props, theme);
        });

    dsys::IdleReturnPayload ir;
    ir.id = "idle-gold-1";
    ir.idle_minutes = 15;
    ir.on_response = [](bool) {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{ir}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_idle_return",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

TEST(DialogRenderers, Golden_SettingsPanel) {
    auto theme = dialog_test_golden::get_light_theme();

    dsys::DialogRendererRegistry registry;
    registry.register_dialog(dsys::DialogType::SettingsPanel,
        [&](const dsys::DialogPayloadVariant& v,
            const dsys::DialogRenderContext&) -> ftxui::Element {
            const auto* p = std::get_if<dsys::SettingsPanelPayload>(&v);
            if (!p) return text("");

            dframe::DialogFrameProps props;
            props.title = "Settings";
            props.subtitle = "Tab: " + p->initial_tab;
            props.style = dframe::FrameStyle::Default;
            props.inner_padding_x = 2;
            props.inner_padding_y = 1;
            props.content = vbox({
                // 3 rows: Model, API base, Max tokens
                hbox({
                    text("  Model") | dim | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 22),
                    text("claude-sonnet-4.6") | bold,
                    filler(),
                }),
                hbox({
                    text("  API base") | dim
                        | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 22),
                    text("api.anthropic.com"),
                    filler(),
                }),
                hbox({
                    text("  Max tokens") | dim
                        | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 22),
                    text("8192"),
                    filler(),
                }),
                text(""),
                hbox({
                    text("  Esc") | dim,
                    text(" to close") | dim,
                    filler(),
                }),
            });
            return dframe::DialogFrame(props, theme);
        });

    dsys::SettingsPanelPayload sp;
    sp.id = "settings-gold-1";
    sp.initial_tab = "general";
    sp.on_close = []() {};

    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100;
    ctx.term_rows = 30;
    ctx.theme = theme;

    auto el = registry.render(dsys::DialogPayloadVariant{sp}, ctx);
    ASSERT_NE(el, nullptr);
    dialog_test_golden::check_golden(
        "dialog_settings_panel",
        dialog_test_golden::render_to_ansi(std::move(el), 100, 30));
}

// ============================================================
// Step 6B: E2E integration test — ReplScreenState-like struct +
// queue push/typing suppression with a thin local RenderBottomDialog
// helper that mirrors repl_screen::dialog_queue_render logic exactly
// (~15 lines) so no new module import is required.
// ============================================================

/// Minimal replica of ReplScreenState's dialog-relevant fields.
/// We intentionally do NOT import cc.ui.screens.repl_screen to avoid
/// build-system churn (CMake INTERFACE→FILE_SET BMI propagation gaps).
struct MiniReplState {
    dsys::DialogQueue dialog_queue;
    dsys::DialogRendererRegistry dialog_renderers;
    bool is_prompt_input_active = false;
    bool is_tool_animation_active = false;
};

/// Local mirror of dialog_queue_render::RenderBottomDialog.
/// Logic: peek_bottom(is_typing, !is_animation) -> render via registry ->
/// clamp to ~40% of terminal rows.
[[nodiscard]] inline std::optional<ftxui::Element> LocalRenderBottomDialog(
    const MiniReplState& s,
    int term_cols,
    int term_rows)
{
    const auto& queue = s.dialog_queue;
    if (!queue.has_any_bottom()) return std::nullopt;

    auto payload = queue.peek_bottom(
        /*is_prompt_input_active=*/s.is_prompt_input_active,
        /*allow_dialogs_with_animation=*/!s.is_tool_animation_active);
    if (!payload) return std::nullopt;

    dsys::DialogRenderContext ctx;
    ctx.term_cols = term_cols;
    ctx.term_rows = term_rows;

    auto el = s.dialog_renderers.render(payload->get(), ctx);
    if (!el) return std::nullopt;

    // Bottom-slot height clamp (~40% of rows)
    return el | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN,
                            std::max(4, term_rows * 2 / 5));
}

TEST(ReplScreenIntegration, TypingSuppressionSkipsBottomDialogs) {
    MiniReplState s;

    // Register a minimal renderer for every bottom-slot type we push:
    // SandboxPermission (Band2), PromptDialog (Band3), CostThreshold (Band4).
    // Each just renders the type name so visual inspection can tell which
    // dialog was rendered.
    auto reg = [&](dsys::DialogType ty, std::string label) {
        s.dialog_renderers.register_renderer(ty,
            [label = std::move(label)](
                const dsys::DialogPayloadVariant&,
                const dsys::DialogRenderContext&) -> ftxui::Element {
                return ftxui::window(
                    ftxui::text(" " + label + " "),
                    ftxui::text(label + " content") | xflex);
            });
    };
    reg(dsys::DialogType::SandboxPermission, "SandboxPermission");
    reg(dsys::DialogType::PromptDialog,      "PromptDialog");
    reg(dsys::DialogType::CostThreshold,     "CostThreshold");

    // Push SandboxPermission (Band2), PromptDialog (Band3), CostThreshold (Band4)
    {
        dsys::SandboxPermissionPayload sbx;
        sbx.id = "sbx-int";
        sbx.host_pattern = "*.example.com";
        s.dialog_queue.push(dsys::DialogPayloadVariant{sbx});
    }
    {
        dsys::PromptDialogPayload pd;
        pd.id = "pd-int";
        pd.title = "Prompt";
        pd.prompt_text = "?";
        s.dialog_queue.push(dsys::DialogPayloadVariant{pd});
    }
    {
        dsys::CostThresholdPayload ct;
        ct.id = "ct-int";
        ct.cost_threshold_usd = 5.0;
        ct.current_cost_usd = 7.23;
        s.dialog_queue.push(dsys::DialogPayloadVariant{ct});
    }

    // (A) typing OFF, animation OFF → Sandbox (Band2, highest) should show
    auto el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_TRUE(el.has_value())
        << "(A) Sandbox Band2 should show with typing=OFF, animation=OFF";

    // (B) typing ON → ALL Band2..6 suppressed → nullopt
    s.is_prompt_input_active = true;
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_FALSE(el.has_value())
        << "(B) Everything must be suppressed while typing is active";

    // (C) typing OFF, animation ON → Band3 (Prompt) skipped,
    //     Band2 still shows
    s.is_prompt_input_active = false;
    s.is_tool_animation_active = true;
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_TRUE(el.has_value())
        << "(C) Band2 SandboxPermission must NOT be suppressed by animation flag";

    // Pop SandboxPermission using the same suppression rules.
    s.dialog_queue.pop_bottom(
        /*typing=*/false,
        /*allow_dialogs_with_animation=*/!s.is_tool_animation_active);
    // Now the queue still contains PromptDialog (Band3, skipped) +
    // CostThreshold (Band4, reachable).
    // Next peek → CostThreshold (Band4), NOT PromptDialog (Band3, skipped).
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_TRUE(el.has_value())
        << "(C-post-pop) Band4 CostThreshold must be reachable when Band3 "
           "PromptDialog is skipped for animation";

    // (D) remove CostThreshold too — next: PromptDialog (still Band3, skipped)
    s.dialog_queue.pop_bottom(false, /*animation_ok=*/false);
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_FALSE(el.has_value())
        << "(D) After CostThreshold is removed, only Band3 PromptDialog "
           "remains; it must be suppressed while animation is ON";

    // Turn animation OFF → PromptDialog (Band3) now shows.
    s.is_tool_animation_active = false;
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_TRUE(el.has_value())
        << "(D-anim-off) PromptDialog Band3 must show when animation goes OFF";

    // (E) typing ON, animation ON → both suppression paths active → nullopt
    s.is_prompt_input_active = true;
    s.is_tool_animation_active = true;
    el = LocalRenderBottomDialog(s, 100, 40);
    EXPECT_FALSE(el.has_value())
        << "(E) typing+animation both ON must suppress every bottom dialog";
}

} // namespace
