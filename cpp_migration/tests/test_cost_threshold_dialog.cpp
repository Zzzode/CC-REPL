/// @file test_cost_threshold_dialog.cpp
/// @brief CostThreshold dialog contract tests (P0x3):
///
///   LAYER-1 CRITICAL FIX — Contract (17 cases):
///   - Payload  = { double dollars_spent, optional<string> model_name,
///                 function<void()> on_done }.
///   - Render   = Title sprintf("You've spent $%.0f on the Anthropic API
///                       this session.", dollars_spent)
///                Body   "Learn more about how to monitor your spending:"
///                       + external docs link
///                       + optional (model: ...) line
///                Actions = ONE single button "Got it, thanks!" — on_done().
///   - Keyboard = Enter  -> on_done()
///                Escape -> on_done()   (CRITICAL: NO data-loss exit!)
///   - DELETED  = fabricated 3-action Continue/Reset/Quit chrome entirely.
///              = on_response(bool,bool) signature — removed.
///              = Three inconsistent renderers — unified to one.
///
/// Golden snapshots:
///   - cost_threshold_title_with_interpolated_dollars
///   - cost_threshold_with_docs_link_rendered

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/event.hpp>
#include <gtest/gtest.h>

import cc.ui.dialogs.cost_threshold_dialog;

namespace {

namespace fs = std::filesystem;
namespace ct = cc::ui::dialogs::cost_threshold;

// ============================================================
// Helpers
// ============================================================

std::string render_to_plain_text(ftxui::Element element, int width = 80, int height = 20) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, element);
    return screen.ToString();
}

/// Strip ANSI CSI sequences + FTXUI \r (CR) so golden comparisons are
/// purely logical content (LF line endings, no escape codes).
std::string strip_ansi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\r') { ++i; continue; }
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E)) ++i;
            if (i < s.size()) ++i;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

std::string read_file_text(const fs::path& p) {
    std::ifstream ifs(p);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

fs::path golden_dir() {
#ifdef CC_REPL_TESTS_ROOT
    return fs::path(CC_REPL_TESTS_ROOT) / "golden";
#else
    return fs::path(__FILE__).parent_path() / "golden";
#endif
}

/// Strip trailing spaces from each line so golden comparisons are
/// width-agnostic at column edges.
std::string strip_trailing_ws_per_line(std::string s) {
    std::string out;
    out.reserve(s.size());
    std::size_t line_start = 0;
    while (line_start < s.size()) {
        auto nl = s.find('\n', line_start);
        if (nl == std::string::npos) nl = s.size();
        std::size_t end = nl;
        while (end > line_start && s[end - 1] == ' ') --end;
        out.append(s, line_start, end - line_start);
        if (nl < s.size()) out.push_back('\n');
        line_start = nl + 1;
    }
    return out;
}

std::string rstrip_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// Normalise a golden comparison (strip ANSI + trailing WS + EOF newline).
std::string norm(std::string&& s) {
    return rstrip_newline(
        strip_trailing_ws_per_line(
            strip_ansi(std::move(s))));
}

// ============================================================
// Group 1: format_title() / dollar interpolation (4 tests)
// ============================================================

// Test 1 — integer dollar amount renders as "$N" without decimals.
TEST(CostThreshold, FormatTitleIntegerDollars) {
    EXPECT_EQ(ct::format_title(5.0),
              "You've spent $5 on the Anthropic API this session.");
}

// Test 2 — fractional dollars round to nearest whole (%.0f semantics).
TEST(CostThreshold, FormatTitleRoundsFractionalDollars) {
    EXPECT_EQ(ct::format_title(4.7),
              "You've spent $5 on the Anthropic API this session.");
    EXPECT_EQ(ct::format_title(4.3),
              "You've spent $4 on the Anthropic API this session.");
}

// Test 3 — zero spend allowed (boundary).
TEST(CostThreshold, FormatTitleZeroSpend) {
    EXPECT_EQ(ct::format_title(0.0),
              "You've spent $0 on the Anthropic API this session.");
}

// Test 4 — large dollar amounts don't overflow or error.
TEST(CostThreshold, FormatTitleLargeDollarAmounts) {
    EXPECT_EQ(ct::format_title(999.99),
              "You've spent $1000 on the Anthropic API this session.");
    EXPECT_EQ(ct::format_title(12345.67),
              "You've spent $12346 on the Anthropic API this session.");
}

// ============================================================
// Group 2: Payload / state contract (3 tests)
// ============================================================

// Test 5 — CostThresholdPayload has the P0x3 fields: double, optional<string>, void().
//          on_response(bool,bool) MUST NOT exist.
TEST(CostThreshold, PayloadFieldsMatchP0Contract) {
    static_assert(std::is_same_v<decltype(ct::CostThresholdPayload::dollars_spent), double>,
                  "dollars_spent must be double");
    static_assert(std::is_same_v<decltype(ct::CostThresholdPayload::model_name),
                                 std::optional<std::string>>,
                  "model_name must be optional<string>");
    // on_done is function<void()> — NO parameters (not on_response(bool,bool)).
    static_assert(std::is_same_v<decltype(ct::CostThresholdPayload::on_done),
                                 std::function<void()>>,
                  "on_done must be 0-arg void callback, NOT on_response(bool,bool)");
}

// Test 6 — state_from_payload round-trips all fields.
TEST(CostThreshold, StateFromPayloadCopiesAllFields) {
    ct::CostThresholdPayload p{
        .dollars_spent = 7.6,
        .model_name    = std::string("claude-3-5-sonnet-20241022"),
        .on_done       = [] { /* no-op */ },
    };
    ct::CostThresholdState st = ct::state_from_payload(p);
    EXPECT_DOUBLE_EQ(st.dollars_spent, 7.6);
    ASSERT_TRUE(st.model_name.has_value());
    EXPECT_EQ(*st.model_name, "claude-3-5-sonnet-20241022");
    EXPECT_EQ(st.selected_index, 0);
    EXPECT_TRUE(static_cast<bool>(st.on_done));
}

// Test 7 — payload without model_name produces nullopt state.
TEST(CostThreshold, PayloadWithoutModelNameYieldsEmptyOptional) {
    ct::CostThresholdPayload p{
        .dollars_spent = 3.0,
        .model_name    = std::nullopt,
        .on_done       = nullptr,
    };
    ct::CostThresholdState st = ct::state_from_payload(p);
    EXPECT_FALSE(st.model_name.has_value());
}

// ============================================================
// Group 3: Renderer (5 tests)
// ============================================================

// Test 8 — Renderer includes interpolated title.
TEST(CostThreshold, RenderShowsInterpolatedTitle) {
    ct::CostThresholdState st{.dollars_spent = 5.0, .selected_index = 0};
    auto plain = strip_ansi(render_to_plain_text(ct::RenderCostThreshold(st), 78, 14));
    EXPECT_NE(plain.find("You've spent $5 on the Anthropic API this session."),
              std::string::npos);
}

// Test 9 — Renderer includes docs URL (body paragraph + external link).
TEST(CostThreshold, RenderShowsDocsLink) {
    ct::CostThresholdState st{.dollars_spent = 12.0, .selected_index = 0};
    auto plain = strip_ansi(render_to_plain_text(ct::RenderCostThreshold(st), 78, 14));
    EXPECT_NE(plain.find("Learn more about how to monitor your spending:"),
              std::string::npos);
    EXPECT_NE(plain.find(std::string(ct::kDocsUrl)), std::string::npos);
}

// Test 10 — Renderer shows model_name when present.
TEST(CostThreshold, RenderShowsModelNameWhenPresent) {
    ct::CostThresholdState st{
        .dollars_spent = 5.0,
        .model_name    = std::string("claude-opus-3"),
        .selected_index = 0,
    };
    auto plain = strip_ansi(render_to_plain_text(ct::RenderCostThreshold(st), 78, 14));
    EXPECT_NE(plain.find("(model: claude-opus-3)"), std::string::npos);
}

// Test 11 — Renderer OMITs model line when model_name is empty/absent.
TEST(CostThreshold, RenderOmitsModelLineWhenEmpty) {
    ct::CostThresholdState st{
        .dollars_spent = 12.0,
        .model_name    = std::nullopt,
        .selected_index = 0,
    };
    auto plain = strip_ansi(render_to_plain_text(ct::RenderCostThreshold(st), 78, 14));
    EXPECT_EQ(plain.find("(model:"), std::string::npos);
}

// Test 12 — Renderer has EXACTLY one Select-style option ("Got it, thanks!").
//            NO fabricated "Continue / Reset / Quit" 3-action chrome.
TEST(CostThreshold, RenderHasSingleGotItThanksButtonNo3ActionChrome) {
    ct::CostThresholdState st{.dollars_spent = 5.0, .selected_index = 0};
    auto plain = strip_ansi(render_to_plain_text(ct::RenderCostThreshold(st), 78, 14));

    // The single correct button label must appear.
    EXPECT_NE(plain.find("Got it, thanks!"), std::string::npos);
    // Fabricated 3-action chrome MUST be absent (P0).
    EXPECT_EQ(plain.find("Continue"),      std::string::npos);
    EXPECT_EQ(plain.find("Reset counter"), std::string::npos);
    EXPECT_EQ(plain.find("Reset"),         std::string::npos);
    EXPECT_EQ(plain.find("[q] Quit"),      std::string::npos);
    EXPECT_EQ(plain.find("Quit"),          std::string::npos);
}

// ============================================================
// Group 4: Golden snapshots (2 tests)
// ============================================================

// Test 13 — Golden: cost_threshold_title_with_interpolated_dollars
TEST(CostThreshold, Golden_TitleWithInterpolatedDollars) {
    ct::CostThresholdState st{
        .dollars_spent  = 4.7,  // rounds to $5
        .model_name     = std::string("claude-3-5-sonnet-20241022"),
        .selected_index = 0,
    };
    const auto actual = norm(render_to_plain_text(ct::RenderCostThreshold(st), 78, 12));
    const auto expected = norm(read_file_text(
        golden_dir() / "cost_threshold_title_with_interpolated_dollars.txt"));
    EXPECT_EQ(actual, expected);
}

// Test 14 — Golden: cost_threshold_with_docs_link_rendered
TEST(CostThreshold, Golden_WithDocsLinkRendered) {
    ct::CostThresholdState st{
        .dollars_spent  = 12.0,
        .model_name     = std::nullopt,
        .selected_index = 0,
    };
    const auto actual = norm(render_to_plain_text(ct::RenderCostThreshold(st), 78, 11));
    const auto expected = norm(read_file_text(
        golden_dir() / "cost_threshold_with_docs_link_rendered.txt"));
    EXPECT_EQ(actual, expected);
}

// ============================================================
// Group 5: Keyboard event handler (3 tests)
// ============================================================

// Test 15 — Enter, Escape, and Space ALL call on_done() exactly once.
//           CRITICAL: Escape must acknowledge (NOT quit / cause data loss).
TEST(CostThreshold, Keyboard_EnterEscapeSpaceAllTriggerOnDone) {
    auto count_events = [](const ftxui::Event& ev) -> int {
        std::atomic<int> calls{0};
        ct::CostThresholdState st;
        st.on_done = [&calls] { calls.fetch_add(1); };
        bool consumed = ct::HandleCostThresholdEvent(st, ev);
        EXPECT_TRUE(consumed);
        return calls.load();
    };
    EXPECT_EQ(count_events(ftxui::Event::Return), 1)
        << "Enter must trigger on_done";
    EXPECT_EQ(count_events(ftxui::Event::Escape), 1)
        << "Esc must trigger on_done (CRITICAL: NOT quit / data-loss!)";
    EXPECT_EQ(count_events(ftxui::Event::Character(' ')), 1)
        << "Space must trigger on_done";
}

// Test 16 — Character shortcuts (g, y, o, k) trigger on_done.
//           Arrow keys + Tab are swallowed (no on_done, no crash).
TEST(CostThreshold, Keyboard_ShortcutsFire_ArrowsSwallowed) {
    for (char c : std::string_view("gyokGYOK")) {
        std::atomic<int> calls{0};
        ct::CostThresholdState st;
        st.on_done = [&calls] { calls.fetch_add(1); };
        bool consumed = ct::HandleCostThresholdEvent(st, ftxui::Event::Character(c));
        EXPECT_TRUE(consumed) << "shortcut '" << c << "' should be consumed";
        EXPECT_EQ(calls.load(), 1) << "shortcut '" << c << "' should fire on_done";
    }
    // Arrow keys: swallowed (consumed=true) but on_done NOT called.
    for (const auto* ev : {&ftxui::Event::ArrowUp,   &ftxui::Event::ArrowDown,
                           &ftxui::Event::ArrowLeft, &ftxui::Event::ArrowRight}) {
        std::atomic<int> calls{0};
        ct::CostThresholdState st;
        st.on_done = [&calls] { calls.fetch_add(1); };
        bool consumed = ct::HandleCostThresholdEvent(st, *ev);
        EXPECT_TRUE(consumed) << "arrow keys must be swallowed";
        EXPECT_EQ(calls.load(), 0) << "arrow keys must NOT fire on_done";
    }
    // Tab: swallowed (consumed=true) but on_done NOT called.
    {
        std::atomic<int> calls{0};
        ct::CostThresholdState st;
        st.on_done = [&calls] { calls.fetch_add(1); };
        bool consumed = ct::HandleCostThresholdEvent(st, ftxui::Event::Tab);
        EXPECT_TRUE(consumed);
        EXPECT_EQ(calls.load(), 0);
    }
}

// Test 17 — Stray / arbitrary characters are swallowed (no prompt-leak).
TEST(CostThreshold, Keyboard_StrayCharsSwallowedNoCrash) {
    for (char c : std::string_view("abcdef123456!@#$%")) {
        std::atomic<int> calls{0};
        ct::CostThresholdState st;
        st.on_done = [&calls] { calls.fetch_add(1); };
        bool consumed = ct::HandleCostThresholdEvent(st, ftxui::Event::Character(c));
        EXPECT_TRUE(consumed) << "stray char '" << c << "' must be swallowed";
        EXPECT_EQ(calls.load(), 0) << "stray char '" << c << "' must NOT fire on_done";
    }
}

} // anonymous namespace
