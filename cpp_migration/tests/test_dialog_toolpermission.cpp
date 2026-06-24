/// @file test_dialog_toolpermission.cpp
/// @brief Unit tests for Dialog #1: ToolPermission (Overlay slot).
///
/// Covered surface:
///   1. DialogSystem basic invariants (slot routing, should_show_dialog
///      suppression policy, queue push/pop/remove-by-id).
///   2. Renderer registration + fallback renderer behaviour.
///   3. RenderToolPermission golden snapshots (bash / file-edit / generic
///      fallback payloads).  We render to an ANSI Screen with fixed size
///      and compare against tests/golden/*.txt.  UPDATE_GOLDENS=1 env
///      rewrites the snapshots.
///   4. HandleToolPermissionEvent keyboard shortcuts — every key →
///      callback-assertion including y/Y/Enter (Allow), a/A (Always),
///      n/N/Esc (Deny), d/D (Abort), s/S (sandbox toggle), arrows/Tab
///      (focus movement), 1/2 (checkbox toggles), Space.
///   5. dialog_queue_render::DispatchDialogQueueEvents priority — the
///      Overlay slot is dispatched before Bottom when both are queued.
///   6. repl_screen::LayerAllDialogs overlay stacking — the overlay is
///      rendered above the base chrome, and suppressed when
///      is_prompt_input_active=true (Band3 suppression).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

import cc.ui.dialogs.dialog_system;
import cc.ui.dialogs.dialog_default_renderers;
import cc.ui.repl_screen;
import cc.ui.permissions.permission_single_prompt;

namespace {

namespace fs = std::filesystem;
namespace dsys = cc::ui::dialogs;
namespace dr   = cc::ui::dialogs::default_renderers;
namespace rs   = cc::ui::repl_screen;
namespace sp   = cc::ui::permissions::single_prompt;

// ─── helpers ───────────────────────────────────────────────────────────────

std::string render_to_ansi(ftxui::Element el, int w = 120, int h = 40) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    return screen.ToString();
}

std::string normalize_line_endings(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

/// Resolve tests/golden/ relative to THIS file's location so tests work
/// regardless of ctest CWD.  Mirror pattern from test_ui.cpp golden idiom.
std::string golden_dir() {
    // __FILE__ resolves to cpp_migration/tests/test_dialog_toolpermission.cpp
    // so strip the filename + append "golden/".
    fs::path here(__FILE__);
    fs::path dir = here.parent_path() / "golden";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string() + (dir.native().ends_with('/') ? "" : "/");
}

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
    ASSERT_TRUE(in.good()) << "golden missing: " << path << " (run with UPDATE_GOLDENS=1 to create)";
    std::string expected(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(normalize_line_endings(actual),
              normalize_line_endings(expected))
        << "golden mismatch: " << path;
}

/// ──── Sample payload builders ──────────────────────────────────────────

dsys::ToolPermissionPayload MakeBashPayload(
    std::function<void(bool, std::optional<bool>)> on_response = nullptr,
    std::function<void()> on_abort = nullptr)
{
    dsys::ToolPermissionPayload p;
    p.id           = 1001;
    p.tool_name    = "Bash";
    p.description  = "Claude wants to run a bash command.";
    p.action_kind  = "execute";
    p.risk_level   = "medium";
    p.affected_paths = {"/tmp/build.sh"};
    p.workspace_root = "/home/user/project";
    p.rule_match_explanation = "matched 'Bash(make *):*' allow-pattern";
    p.can_always_allow = true;
    p.detail_extra = dsys::ToolPermissionPayload::BashDetail{
        .command           = "cmake --build build-check -j 8",
        .working_directory = "/home/user/project",
        .risk_text         = "Moderate",
        .matched_rules     = {"Bash(make *):*", "Bash(cmake *):*"},
    };
    p.on_response = std::move(on_response);
    p.on_abort    = std::move(on_abort);
    return p;
}

dsys::ToolPermissionPayload MakeFileEditPayload() {
    dsys::ToolPermissionPayload p;
    p.id          = 1002;
    p.tool_name   = "FileEditTool";
    p.description = "Claude wants to edit a C++ module file.";
    p.action_kind = "write";
    p.risk_level  = "low";
    p.affected_paths = {"src/ui/dialogs/dialog_system.cppm"};
    p.workspace_root = "/home/user/project";
    p.can_always_allow = true;
    p.detail_extra = dsys::ToolPermissionPayload::FileEditDetail{
        .file_path    = "src/ui/dialogs/dialog_system.cppm",
        .op_label     = "MODIFY",
        .diff_preview = "+   DialogRendererRegistry registry;",
        .lines_added  = 12,
        .lines_removed= 3,
    };
    return p;
}

dsys::ToolPermissionPayload MakeGenericPayload() {
    dsys::ToolPermissionPayload p;
    p.id          = 1003;
    p.tool_name   = "AgentTool";
    p.description = "Claude wants to spawn a sub-agent.";
    p.action_kind = "custom";
    p.risk_level  = "high";
    p.detail_extra = dsys::ToolPermissionPayload::FallbackDetail{
        .subtitle = "Sub-agent launch",
        .key_value_rows = {
            {"Agent ID", "coordinator-a2"},
            {"Budget tokens", "64,000"},
            {"Timeout", "5 minutes"},
        },
    };
    p.can_always_allow = false;
    return p;
}

// ─── 1. DialogSystem basic invariants ──────────────────────────────────────

TEST(DialogSystem, SlotRoutingForTypes) {
    EXPECT_EQ(dsys::slot_for_override(dsys::DialogType::ToolPermission),
              dsys::DialogSlot::Overlay);
    EXPECT_EQ(dsys::slot_for_override(dsys::DialogType::SandboxPermission),
              dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for_override(dsys::DialogType::TrustDialog),
              dsys::DialogSlot::Standalone);
    EXPECT_EQ(dsys::slot_for_override(dsys::DialogType::SettingsPanel),
              dsys::DialogSlot::Modal);
    EXPECT_EQ(dsys::band_for(dsys::DialogType::ToolPermission),
              dsys::PriorityBand::Band3);
    EXPECT_EQ(dsys::band_for(dsys::DialogType::SandboxPermission),
              dsys::PriorityBand::Band2);
}

TEST(DialogSystem, ShouldShowDialogSuppressionPolicy) {
    // Band3 ToolPermission is suppressed while prompt has input active
    // OR mid-animation, but shown otherwise.
    EXPECT_FALSE(dsys::should_show_dialog(
        dsys::DialogType::ToolPermission,
        /*is_prompt_input_active=*/true,  /*allow_anim=*/true));
    EXPECT_FALSE(dsys::should_show_dialog(
        dsys::DialogType::ToolPermission,
        /*is_prompt_input_active=*/false, /*allow_anim=*/false));
    EXPECT_TRUE(dsys::should_show_dialog(
        dsys::DialogType::ToolPermission,
        /*is_prompt_input_active=*/false, /*allow_anim=*/true));
    // Band2 SandboxPermission (security-critical) is NEVER suppressed.
    EXPECT_TRUE(dsys::should_show_dialog(
        dsys::DialogType::SandboxPermission,
        /*is_prompt_input_active=*/true,  /*allow_anim=*/false));
    // Band4 banners are also always shown.
    EXPECT_TRUE(dsys::should_show_dialog(
        dsys::DialogType::IdleReturn,
        /*is_prompt_input_active=*/true,  /*allow_anim=*/false));
    // Modal / Standalone always show.
    EXPECT_TRUE(dsys::should_show_dialog(
        dsys::DialogType::TrustDialog, true, false));
    EXPECT_TRUE(dsys::should_show_dialog(
        dsys::DialogType::SettingsPanel, true, false));
}

TEST(DialogSystem, QueuePushRoutesByTypeAndPopById) {
    dsys::DialogQueue q;
    auto tp_id = dsys::triggers::PushToolPermission(
        q, "Bash", "run command", [](bool, auto){});
    EXPECT_NE(tp_id, 0u);
    EXPECT_TRUE(q.has_overlay());
    EXPECT_FALSE(q.has_bottom());
    EXPECT_FALSE(q.has_modal());
    EXPECT_FALSE(q.has_standalone());
    EXPECT_EQ(q.total_pending(), 1u);

    // Remove by ID.
    EXPECT_TRUE(q.remove(tp_id));
    EXPECT_FALSE(q.has_overlay());
    EXPECT_EQ(q.total_pending(), 0u);
    EXPECT_FALSE(q.remove(99999999u));  // non-existent
}

TEST(DialogSystem, DialogPayloadVariantTypeOfMatchesPush) {
    dsys::DialogQueue q;
    dsys::triggers::PushToolPermission(
        q, "FileEdit", "edit file", [](bool, auto){});
    auto peek = q.peek_overlay();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(peek->get()), dsys::DialogType::ToolPermission);
    EXPECT_EQ(dsys::name_of(peek->get()), "tool-permission");
    EXPECT_GT(dsys::id_of(peek->get()), 0u);
}

// ─── 2. Renderer registration + fallback ──────────────────────────────────

TEST(DialogRegistry, UnregisteredTypeRendersFallbackWithEscHint) {
    dsys::DialogRendererRegistry reg;
    // Don't register ToolPermission — force fallback.
    dsys::ToolPermissionPayload payload = MakeBashPayload();
    dsys::DialogPayloadVariant v = payload;
    dsys::DialogRenderContext ctx;
    ctx.terminal_width = 100; ctx.terminal_height = 30;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    std::string ansi = render_to_ansi(std::move(el), 100, 30);
    EXPECT_NE(ansi.find("ToolPermission"), std::string::npos);
    EXPECT_NE(ansi.find("Esc"), std::string::npos);
}

TEST(DialogRegistry, RegisterDefaultEnablesRenderAndHandle) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    EXPECT_TRUE(reg.has_renderer(dsys::DialogType::ToolPermission));
    EXPECT_TRUE(reg.has_event_handler(dsys::DialogType::ToolPermission));
}

// ─── 3. Golden snapshot tests ─────────────────────────────────────────────

TEST(VisualSnapshot, ToolPermissionBashMatchesGolden) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    dsys::DialogPayloadVariant v = MakeBashPayload();
    // Ensure ui_state is NOT set so we test the fresh-build path and the
    // output is fully deterministic (no focus drift between calls).
    dsys::DialogRenderContext ctx;
    ctx.terminal_width = 120; ctx.terminal_height = 40;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    check_golden("tool_permission_bash",
                 render_to_ansi(std::move(el), 120, 40));
}

TEST(VisualSnapshot, ToolPermissionFileEditMatchesGolden) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    dsys::DialogPayloadVariant v = MakeFileEditPayload();
    dsys::DialogRenderContext ctx;
    ctx.terminal_width = 120; ctx.terminal_height = 40;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    check_golden("tool_permission_file_edit",
                 render_to_ansi(std::move(el), 120, 40));
}

TEST(VisualSnapshot, ToolPermissionGenericMatchesGolden) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    dsys::DialogPayloadVariant v = MakeGenericPayload();
    dsys::DialogRenderContext ctx;
    ctx.terminal_width = 120; ctx.terminal_height = 40;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    check_golden("tool_permission_generic",
                 render_to_ansi(std::move(el), 120, 40));
}

// ─── 4. Keyboard event handling ────────────────────────────────────────────

using DecisionPair = std::pair<bool, std::optional<bool>>;
constexpr DecisionPair kNone = {false, std::nullopt};

struct DecisionRecorder {
    std::optional<DecisionPair> response;
    int abort_called = 0;
    void reset() { response.reset(); abort_called = 0; }
};

dsys::ToolPermissionPayload MakeRecordedBash(DecisionRecorder& rec) {
    return MakeBashPayload(
        /*on_response=*/[&rec](bool a, std::optional<bool> b) {
            rec.response = DecisionPair{a, b};
        },
        /*on_abort=*/[&rec] { ++rec.abort_called; });
}

TEST(KeyboardEvents, LowercaseYProducesAllowOnce) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('y')));
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, UppercaseYProducesAllowOnce) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('Y')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
}

TEST(KeyboardEvents, EnterProducesAllowOnce) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Return));
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
}

TEST(KeyboardEvents, LowercaseAProducesAlwaysAllow) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('a')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, UppercaseAProducesAlwaysAllow) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('A')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, LowercaseNProducesDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('n')));
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, UppercaseNProducesDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('N')));
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, EscapeInvokesAbortCallback) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Escape));
    // EmitDecision(Abort) → on_abort() because MakeRecordedBash sets it.
    EXPECT_EQ(rec.abort_called, 1);
    EXPECT_FALSE(rec.response.has_value())
        << "Abort with on_abort set must NOT fall through to on_response";
}

TEST(KeyboardEvents, EscapeFallsBackToDenyWhenNoAbortCb) {
    // Build a payload WITHOUT on_abort — EmitDecision(Abort) must fall
    // through to on_response(false, nullopt) so the consumer always gets
    // SOME signal even if it forgot to register on_abort.
    DecisionRecorder rec;
    auto p = MakeBashPayload(
        [&rec](bool a, std::optional<bool> b) {
            rec.response = DecisionPair{a, b};
        },
        /*on_abort=*/nullptr);
    dsys::DialogPayloadVariant v = std::move(p);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Escape));
    EXPECT_EQ(rec.abort_called, 0);
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, LowercaseDProducesAlwaysDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('d')));
    // 'd' maps to Decision::AlwaysDeny → on_response(false, nullopt).
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, UppercaseDProducesAlwaysDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('D')));
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, SandboxToggleSStoresStateInPayload) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->initial_sandbox_toggle);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('s')));
    // GetOrCreateUiState creates PromptState and flips sandbox_toggle.
    // Re-reading PromptState from p->ui_state should confirm ON.
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->sandbox_toggle);
    // Second 'S' (uppercase) toggles back off.
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('S')));
    EXPECT_FALSE(st->sandbox_toggle);
    // s/S only mutates state — no callback invoked.
    EXPECT_FALSE(rec.response.has_value());
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, ArrowKeysAndTabCycleFocus) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    ASSERT_NE(p, nullptr);

    // ArrowRight from 0 (Allow once) → 1 (Deny)
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::ArrowRight));
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->focused_button, 1);
    // Tab → 2 (Always allow)
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Tab));
    EXPECT_EQ(st->focused_button, 2);
    // TabReverse → back to 1
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::TabReverse));
    EXPECT_EQ(st->focused_button, 1);
    // ArrowLeft → 0
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::ArrowLeft));
    EXPECT_EQ(st->focused_button, 0);
    // No decision emitted during focus movement.
    EXPECT_FALSE(rec.response.has_value());
}

TEST(KeyboardEvents, EnterActivatesFocusedAlwaysAllow) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    ASSERT_NE(p, nullptr);

    // Move focus to index 2 (Always allow) with two ArrowRight presses,
    // then hit Enter → callback should fire (always=true).
    (void)dr::HandleToolPermissionEvent(v, ftxui::Event::ArrowRight);  // 0→1
    (void)dr::HandleToolPermissionEvent(v, ftxui::Event::ArrowRight);  // 1→2
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Return));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, OneAndTwoToggleCheckboxPersist) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    ASSERT_NE(p, nullptr);
    // Key '1' → always_deny_checkbox on
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('1')));
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->always_deny_checkbox);
    // Key '2' → always_allow_checkbox on
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('2')));
    EXPECT_TRUE(st->always_allow_checkbox);
    // Then press 'y' → AlwaysAllow wins because always_allow is checked.
    EXPECT_TRUE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('y')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, UnrelatedCharacterReturnsUnhandled) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_FALSE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('x')));
    EXPECT_FALSE(rec.response.has_value());
    EXPECT_FALSE(dr::HandleToolPermissionEvent(v, ftxui::Event::Character('Z')));
}

// ─── 5. DispatchDialogQueueEvents priority ─────────────────────────────────

TEST(DialogDispatchPriority, OverlayDispatchedBeforeBottom) {
    rs::ReplScreenState s;
    dr::register_default_renderers(s.dialog_renderers);

    // Push overlay + bottom.
    DecisionRecorder rec_bash, rec_idle;
    auto bash = MakeRecordedBash(rec_bash);
    s.dialog_queue.push_overlay(std::move(bash));

    dsys::IdleReturnPayload idle;
    idle.id = 2001;
    idle.name = "idle-return";
    idle.idle_minutes = 42;
    idle.on_response = [&rec_idle](auto) {};
    s.dialog_queue.push_bottom(std::move(idle));

    // 'y' should be consumed by the overlay, not the idle bottom payload.
    EXPECT_TRUE(rs::dialog_queue_render::DispatchDialogQueueEvents(
        s, ftxui::Event::Character('y'),
        /*is_prompt_input_active=*/false, /*allow_anim=*/true));
    EXPECT_TRUE(rec_bash.response.has_value());
    EXPECT_EQ(*rec_bash.response, (DecisionPair{true, false}));
}

TEST(DialogDispatchPriority, Band3OverlaySuppressedWhileTyping) {
    rs::ReplScreenState s;
    dr::register_default_renderers(s.dialog_renderers);

    DecisionRecorder rec_bash;
    auto bash = MakeRecordedBash(rec_bash);
    s.dialog_queue.push_overlay(std::move(bash));

    // is_prompt_input_active=true → Overlay not dispatched.
    EXPECT_FALSE(rs::dialog_queue_render::DispatchDialogQueueEvents(
        s, ftxui::Event::Character('y'),
        /*is_prompt_input_active=*/true, /*allow_anim=*/true));
    EXPECT_FALSE(rec_bash.response.has_value());

    // Deactivate prompt → event is routed normally.
    EXPECT_TRUE(rs::dialog_queue_render::DispatchDialogQueueEvents(
        s, ftxui::Event::Character('y'),
        /*is_prompt_input_active=*/false, /*allow_anim=*/true));
    EXPECT_EQ(*rec_bash.response, (DecisionPair{true, false}));
}

// ─── 6. LayerAllDialogs overlay stacking + suppression ────────────────────

TEST(LayerAllDialogs, OverlaySuppressedWhenPromptActive) {
    rs::ReplScreenState s;
    dr::register_default_renderers(s.dialog_renderers);
    s.dialog_queue.push_overlay(MakeBashPayload());

    auto base = ftxui::text("base chrome") | ftxui::bold;
    Element active = rs::dialog_queue_render::LayerAllDialogs(
        ftxui::Element{base}, s,
        /*is_prompt_input_active=*/true, /*allow_anim=*/true, 100, 25);
    Element idle = rs::dialog_queue_render::LayerAllDialogs(
        ftxui::Element{base}, s,
        /*is_prompt_input_active=*/false, /*allow_anim=*/true, 100, 25);

    std::string active_str = render_to_ansi(std::move(active), 100, 25);
    std::string idle_str   = render_to_ansi(std::move(idle), 100, 25);

    // Prompt-active case should not contain the permission header (overlay
    // suppressed), idle case should.
    EXPECT_EQ(active_str.find("Permission"), std::string::npos)
        << "Band3 overlay must be suppressed while prompt has input active";
    EXPECT_NE(idle_str.find("Permission"), std::string::npos)
        << "Band3 overlay must render when prompt is idle";
}

TEST(LayerAllDialogs, StandaloneReplacesEntireChrome) {
    rs::ReplScreenState s;
    dsys::TrustDialogPayload trust;
    trust.id = 3001;
    trust.name = "trust-dialog";
    s.dialog_queue.push_standalone(std::move(trust));

    // RenderReplScreen short-circuits to RenderStandaloneDialog when
    // has_standalone() → no status bar / messages / prompt in output.
    Element full = rs::RenderReplScreen(s);
    std::string ansi = render_to_ansi(std::move(full), 120, 40);
    // The fallback renderer prints the dialog type name.
    EXPECT_NE(ansi.find("TrustDialog"), std::string::npos);
}

}  // namespace