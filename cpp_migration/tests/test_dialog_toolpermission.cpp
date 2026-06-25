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

import cc.ui.dialogs.system;
import cc.ui.dialogs.default_renderers;
import cc.ui.dialogs.triggers;
import cc.ui.repl_screen;
import cc.ui.permissions.single_prompt;

namespace {

namespace fs   = std::filesystem;
namespace dsys = cc::ui::dialogs::system;
namespace dtrig = cc::ui::dialogs::triggers;
namespace dr   = cc::ui::dialogs::default_renderers;
namespace rs   = cc::ui::repl_screen;
namespace sp   = cc::ui::permissions::single_prompt;

using Element = ftxui::Element;

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
    p.id           = "1001";
    p.tool_name    = "Bash";
    p.description  = "Claude wants to run a bash command.";
    p.action_kind  = sp::ActionKind::Execute;
    p.risk_level   = sp::RiskLevel::Medium;
    p.affected_paths = {"/tmp/build.sh"};
    p.workspace_root = "/home/user/project";
    p.rule_match_explanation = "matched 'Bash(make *):*' allow-pattern";
    p.can_always_allow = true;
    p.detail = sp::DetailBash{
        .command           = "cmake --build build-check -j 8",
        .working_dir       = "/home/user/project",
        .is_destructive    = false,
        .destructive_reason = "",
    };
    p.on_response = [cb = std::move(on_response)](sp::Decision d, bool sandbox) {
        if (!cb) return;
        if (d == sp::Decision::AlwaysAllow || d == sp::Decision::AllowOnce)
            cb(true,  /*always=*/d == sp::Decision::AlwaysAllow);
        else
            cb(false, std::nullopt);
        (void)sandbox;
    };
    p.on_abort    = std::move(on_abort);
    return p;
}

dsys::ToolPermissionPayload MakeFileEditPayload() {
    dsys::ToolPermissionPayload p;
    p.id          = "1002";
    p.tool_name   = "FileEditTool";
    p.description = "Claude wants to edit a C++ module file.";
    p.action_kind = sp::ActionKind::Write;
    p.risk_level  = sp::RiskLevel::Low;
    p.affected_paths = {"src/ui/dialogs/dialog_system.cppm"};
    p.workspace_root = "/home/user/project";
    p.can_always_allow = true;
    p.detail = sp::DetailFileEdit{
        .file_path    = "src/ui/dialogs/dialog_system.cppm",
        .old_snippet  = "   // old code\n",
        .new_snippet  = "   DialogRendererRegistry registry;\n   // new code\n",
        .creates_file = false,
    };
    return p;
}

dsys::ToolPermissionPayload MakeGenericPayload() {
    dsys::ToolPermissionPayload p;
    p.id          = "1003";
    p.tool_name   = "AgentTool";
    p.description = "Claude wants to spawn a sub-agent.";
    p.action_kind = sp::ActionKind::Other;
    p.risk_level  = sp::RiskLevel::High;
    p.detail = sp::DetailGeneric{
        .description =
            "Agent ID: coordinator-a2\n"
            "Budget tokens: 64,000\n"
            "Timeout: 5 minutes",
    };
    p.can_always_allow = false;
    return p;
}


// ─── 1. DialogSystem basic invariants ──────────────────────────────────────

TEST(DialogSystem, SlotRoutingForTypes) {
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::ToolPermission),
              dsys::DialogSlot::Overlay);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::SandboxPermission),
              dsys::DialogSlot::Bottom);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::TrustDialog),
              dsys::DialogSlot::Standalone);
    EXPECT_EQ(dsys::slot_for(dsys::DialogType::SettingsPanel),
              dsys::DialogSlot::Modal);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::ToolPermission),
              dsys::DialogPriority::Band3);
    EXPECT_EQ(dsys::priority_for(dsys::DialogType::SandboxPermission),
              dsys::DialogPriority::Band2);
}

TEST(DialogSystem, ShouldShowDialogSuppressionPolicy) {
    // Build minimal payload variants to test should_show_dialog (which takes
    // DialogPayloadVariant, not DialogType, so it can inspect priority too).
    auto make_variant = [](dsys::DialogType t) -> dsys::DialogPayloadVariant {
        using namespace dsys;
        switch (t) {
            case DialogType::ToolPermission: {
                ToolPermissionPayload p; p.id = "tp"; return p;
            }
            case DialogType::SandboxPermission: {
                SandboxPermissionPayload p; p.id = "sp"; return p;
            }
            case DialogType::IdleReturn: {
                IdleReturnPayload p; p.id = "ir"; return p;
            }
            case DialogType::TrustDialog: {
                TrustDialogPayload p; p.id = "td"; return p;
            }
            case DialogType::SettingsPanel: {
                SettingsPanelPayload p; p.id = "set"; return p;
            }
            default: {
                ToolPermissionPayload p; p.id = "x"; return p;
            }
        }
    };

    auto tp  = make_variant(dsys::DialogType::ToolPermission);
    auto sb  = make_variant(dsys::DialogType::SandboxPermission);
    auto ir  = make_variant(dsys::DialogType::IdleReturn);
    auto td  = make_variant(dsys::DialogType::TrustDialog);
    auto set = make_variant(dsys::DialogType::SettingsPanel);

    // Band3 ToolPermission is suppressed while prompt has input active
    // OR mid-animation, but shown otherwise.
    EXPECT_FALSE(dsys::should_show_dialog(
        tp, /*is_prompt_input_active=*/true,  /*allow_anim=*/true));
    EXPECT_FALSE(dsys::should_show_dialog(
        tp, /*is_prompt_input_active=*/false, /*allow_anim=*/false));
    EXPECT_TRUE(dsys::should_show_dialog(
        tp, /*is_prompt_input_active=*/false, /*allow_anim=*/true));
    // Band2 SandboxPermission (Bottom): suppressed while typing (only
    // Band1 bottom dialogs survive typing) but NOT suppressed by the
    // animation flag (animation suppression only targets Band3).
    EXPECT_FALSE(dsys::should_show_dialog(
        sb, /*is_prompt_input_active=*/true,  /*allow_anim=*/false));
    EXPECT_TRUE(dsys::should_show_dialog(
        sb, /*is_prompt_input_active=*/false, /*allow_anim=*/false));
    // Band4 banners: same typing-suppression rule (typing kills bands 2..6).
    // Animation suppression never touches Band4.
    EXPECT_FALSE(dsys::should_show_dialog(
        ir, /*is_prompt_input_active=*/true,  /*allow_anim=*/false));
    EXPECT_TRUE(dsys::should_show_dialog(
        ir, /*is_prompt_input_active=*/false, /*allow_anim=*/false));
    // Modal / Standalone always show.
    EXPECT_TRUE(dsys::should_show_dialog(td, true, false));
    EXPECT_TRUE(dsys::should_show_dialog(set, true, false));
}

// ─── Helpers: extract ToolPermissionPayload from a variant  ───────────────
// dr::HandleToolPermissionEvent(...) accepts dsys::ToolPermissionPayload&,
// not DialogPayloadVariant&.  These helpers keep the test body readable.
inline dsys::ToolPermissionPayload& tp(dsys::DialogPayloadVariant& v) {
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    if (!p) {
        ADD_FAILURE() << "Variant does not hold ToolPermissionPayload";
        static dsys::ToolPermissionPayload dummy; // crash-safe fallback
        return dummy;
    }
    return *p;
}
inline const dsys::ToolPermissionPayload& tp(const dsys::DialogPayloadVariant& v) {
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    if (!p) {
        ADD_FAILURE() << "Variant does not hold ToolPermissionPayload (const)";
        static const dsys::ToolPermissionPayload dummy; // crash-safe fallback
        return dummy;
    }
    return *p;
}

TEST(DialogSystem, QueuePushRoutesByTypeAndPopById) {
    dsys::DialogQueue q;
    bool cb_fired = false;
    dtrig::PushToolPermission(
        q, "Bash", "run command",
        [&cb_fired](dsys::ToolPermissionPayload::Decision, bool) { cb_fired = true; });
    EXPECT_TRUE(q.has_overlay());
    EXPECT_FALSE(q.has_any_bottom());
    EXPECT_FALSE(q.has_modal());
    EXPECT_FALSE(q.has_standalone());
    EXPECT_EQ(q.total_size(), 1u);

    // Remove by ID.  PushToolPermission hardcodes id="tool-permission".
    EXPECT_TRUE(q.contains_type(dsys::DialogType::ToolPermission));
    q.remove("tool-permission");
    EXPECT_FALSE(q.has_overlay());
    EXPECT_EQ(q.total_size(), 0u);
    // remove() is noexcept; check contains_type becomes false.
    EXPECT_FALSE(q.contains_type(dsys::DialogType::ToolPermission));
}

TEST(DialogSystem, DialogPayloadVariantTypeOfMatchesPush) {
    dsys::DialogQueue q;
    dtrig::PushToolPermission(
        q, "FileEdit", "edit file",
        [](dsys::ToolPermissionPayload::Decision, bool) {});
    auto peek = q.peek_overlay();
    ASSERT_TRUE(peek.has_value());
    EXPECT_EQ(dsys::type_of(peek->get()), dsys::DialogType::ToolPermission);
    EXPECT_EQ(dsys::dialog_type_name(dsys::type_of(peek->get())), "tool-permission");
    EXPECT_GT(dsys::id_of(peek->get()).size(), 0u);
}

// ─── 2. Renderer registration + fallback ──────────────────────────────────

TEST(DialogRegistry, UnregisteredTypeRendersFallbackWithEscHint) {
    dsys::DialogRendererRegistry reg;
    // Don't register ToolPermission — force fallback.
    dsys::ToolPermissionPayload payload = MakeBashPayload();
    dsys::DialogPayloadVariant v = payload;
    dsys::DialogRenderContext ctx;
    ctx.term_cols = 100; ctx.term_rows = 30;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    std::string ansi = render_to_ansi(std::move(el), 100, 30);
    EXPECT_NE(ansi.find("tool-permission"), std::string::npos);
    EXPECT_NE(ansi.find("Esc"), std::string::npos);
}

TEST(DialogRegistry, RegisterDefaultEnablesRenderAndHandle) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    // Query via render + handle_event round-trip (registry does not expose
    // separate contains() / has_renderer() accessors).
    dsys::DialogPayloadVariant v = MakeBashPayload();
    dsys::DialogRenderContext ctx;
    ctx.term_cols = 80; ctx.term_rows = 24;
    Element el = reg.render(v, ctx);
    ASSERT_NE(el, nullptr);
    // Renderer is registered when it returns something richer than the
    // fallback (which contains "Esc" but also a "No renderer registered"
    // line).  We simply verify non-blank non-fallback output here.
    std::string ansi = render_to_ansi(std::move(el), 80, 24);
    EXPECT_FALSE(ansi.empty());
    // Event handler must be wired and handle a recognized keystroke (y).
    EXPECT_TRUE(reg.handle_event(v, ftxui::Event::Character('y')));
}

// ─── 3. Golden snapshot tests ─────────────────────────────────────────────

TEST(VisualSnapshot, ToolPermissionBashMatchesGolden) {
    dsys::DialogRendererRegistry reg;
    dr::register_default_renderers(reg);
    dsys::DialogPayloadVariant v = MakeBashPayload();
    // Ensure ui_state is NOT set so we test the fresh-build path and the
    // output is fully deterministic (no focus drift between calls).
    dsys::DialogRenderContext ctx;
    ctx.term_cols = 120; ctx.term_rows = 40;
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
    ctx.term_cols = 120; ctx.term_rows = 40;
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
    ctx.term_cols = 120; ctx.term_rows = 40;
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
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('y')));
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, UppercaseYProducesAllowOnce) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('Y')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
}

TEST(KeyboardEvents, EnterProducesAllowOnce) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Return));
    EXPECT_EQ(*rec.response, (DecisionPair{true, false}));
}

TEST(KeyboardEvents, LowercaseAProducesAlwaysAllow) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('a')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, UppercaseAProducesAlwaysAllow) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('A')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, LowercaseNProducesDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('n')));
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, UppercaseNProducesDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('N')));
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, EscapeInvokesAbortCallback) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Escape));
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
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Escape));
    EXPECT_EQ(rec.abort_called, 0);
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
}

TEST(KeyboardEvents, LowercaseDProducesAlwaysDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('d')));
    // 'd' maps to Decision::AlwaysDeny → on_response(false, nullopt).
    ASSERT_TRUE(rec.response.has_value());
    EXPECT_EQ(*rec.response, (DecisionPair{false, std::nullopt}));
    EXPECT_EQ(rec.abort_called, 0);
}

TEST(KeyboardEvents, UppercaseDProducesAlwaysDeny) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('D')));
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
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('s')));
    // GetOrCreateUiState creates PromptState and flips sandbox_toggle.
    // Re-reading PromptState from p->ui_state should confirm ON.
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->sandbox_toggle);
    // Second 'S' (uppercase) toggles back off.
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('S')));
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
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::ArrowRight));
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->focused_button, 1);
    // Tab → 2 (Always allow)
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Tab));
    EXPECT_EQ(st->focused_button, 2);
    // TabReverse → back to 1
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::TabReverse));
    EXPECT_EQ(st->focused_button, 1);
    // ArrowLeft → 0
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::ArrowLeft));
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
    (void)dr::HandleToolPermissionEvent(tp(v), ftxui::Event::ArrowRight);  // 0→1
    (void)dr::HandleToolPermissionEvent(tp(v), ftxui::Event::ArrowRight);  // 1→2
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Return));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, OneAndTwoToggleCheckboxPersist) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    auto* p = std::get_if<dsys::ToolPermissionPayload>(&v);
    ASSERT_NE(p, nullptr);
    // Key '1' → always_deny_checkbox on
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('1')));
    auto st = std::static_pointer_cast<sp::PromptState>(p->ui_state);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->always_deny_checkbox);
    // Key '2' → always_allow_checkbox on
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('2')));
    EXPECT_TRUE(st->always_allow_checkbox);
    // Then press 'y' → AlwaysAllow wins because always_allow is checked.
    EXPECT_TRUE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('y')));
    EXPECT_EQ(*rec.response, (DecisionPair{true, true}));
}

TEST(KeyboardEvents, UnrelatedCharacterReturnsUnhandled) {
    DecisionRecorder rec;
    dsys::DialogPayloadVariant v = MakeRecordedBash(rec);
    EXPECT_FALSE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('x')));
    EXPECT_FALSE(rec.response.has_value());
    EXPECT_FALSE(dr::HandleToolPermissionEvent(tp(v), ftxui::Event::Character('Z')));
}

// ─── 5. DispatchDialogQueueEvents priority ─────────────────────────────────

TEST(DialogDispatchPriority, OverlayDispatchedBeforeBottom) {
    rs::ReplScreenState s;
    dr::register_default_renderers(s.dialog_renderers);

    // Push overlay + bottom.  queue.push() auto-routes by DialogType.
    DecisionRecorder rec_bash, rec_idle;
    auto bash = MakeRecordedBash(rec_bash);
    s.dialog_queue.push(std::move(bash));

    dsys::IdleReturnPayload idle;
    idle.id = "idle-return";
    idle.idle_minutes = 42;
    idle.on_response = [&rec_idle](auto) {};
    s.dialog_queue.push(std::move(idle));

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
    s.dialog_queue.push(std::move(bash));

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
    s.dialog_queue.push(MakeBashPayload());

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
    trust.id = "trust-dialog";
    s.dialog_queue.push_standalone(std::move(trust));

    // RenderReplScreen short-circuits to RenderStandaloneDialog when
    // has_standalone() → no status bar / messages / prompt in output.
    Element full = rs::RenderReplScreen(s);
    std::string ansi = render_to_ansi(std::move(full), 120, 40);
    // The fallback renderer prints the dialog type name.
    EXPECT_NE(ansi.find("trust-dialog"), std::string::npos);
}

}  // namespace