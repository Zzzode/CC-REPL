/// @file test_prompt_dialog.cpp
/// @brief PromptDialog tests (SELECT + FREE-TEXT) against the TS contract.
///
/// Coverage (TS spec reference):
///   1. PromptDialogPayload field existence test (options / tool_input_summary
///      / on_abort / selected_index) — see lines 125-143.
///   2. Rendering goldens (3 NEW required by spec):
///        * prompt_dialog_select_3_options
///        * prompt_dialog_select_focus_on_second_row
///        * prompt_dialog_with_tool_input_summary
///   3. Keyboard: j/k/ArrowUp/Down with wrap, 1..9 shortcut commits,
///      Enter on focused, Escape -> on_response(nullopt),
///      Ctrl+C ({0x03}) -> on_abort (separate code path from Esc).
///   4. Mode dispatch: options non-empty => SELECT, options empty => FREE-TEXT.

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

import cc.ui.dialogs.prompt_dialog;

namespace {

// Module-scope flag so FREE-TEXT Ctrl+C test can distinguish the response
// callback from the abort callback without relying on captured state being
// identical across re-assignment.
inline bool response_called_global = false;

namespace fs = std::filesystem;
using namespace ftxui;
using cc::ui::dialogs::prompt_dialog::MakePromptDialog;
using cc::ui::dialogs::prompt_dialog::PromptDialogPayload;
using cc::ui::dialogs::prompt_dialog::PromptOption;

// ---------------------------------------------------------------------------
// Test harness helpers
// ---------------------------------------------------------------------------

/// Render an FTXUI Component to a plain-string snapshot.
std::string RenderComponent(const Component& component,
                            int width = 92,
                            int height = 28)
{
    auto screen = Screen::Create(Dimension::Fixed(width),
                                 Dimension::Fixed(height));
    auto doc = component->Render();
    Render(screen, doc);
    return screen.ToString();
}

/// Strip ANSI CSI sequences so that golden tests compare semantic content
/// rather than colour/style bytes.
std::string StripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
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

/// Resolve the golden output directory: tests/golden next to this file.
/// We anchor on __FILE__ so the resolution is stable no matter what the
/// process CWD is (build directory, source tree, CTest runner, etc.).
fs::path GoldenDir() {
    static const fs::path dir = [] {
        // __FILE__ expands to an absolute or source-tree relative path of
        // this compilation unit (tests/test_prompt_dialog.cpp). Walk up to
        // its parent directory (tests/) then append "golden".
        const fs::path this_file = fs::path{__FILE__}.lexically_normal();
        fs::path tests_dir = this_file.parent_path();
        if (tests_dir.filename() != "tests") {
            // Fallback: walk up looking for tests/.
            fs::path p = this_file.parent_path();
            while (!p.empty() && p != p.parent_path()) {
                if (fs::is_directory(p / "tests")) {
                    tests_dir = p / "tests";
                    break;
                }
                p = p.parent_path();
            }
        }
        const auto golden = tests_dir / "golden";
        std::error_code ec;
        fs::create_directories(golden, ec);
        return golden;
    }();
    return dir;
}

/// Returns true iff UPDATE_GOLDENS=1 in the environment.
bool ShouldUpdateGoldens() {
    const char* v = std::getenv("UPDATE_GOLDENS");
    return v != nullptr && std::string_view{v} == "1";
}

/// Normalize both content and the on-disk golden to LF line endings so
/// git autocrlf conversions never spurious-fail the comparison.
[[nodiscard]] static std::string normalize_line_endings(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            // Skip '\r' — the following '\n' (if any) becomes a plain LF.
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

/// Run a golden snapshot check.  When UPDATE_GOLDENS=1 we write the
/// content to disk and succeed; otherwise we compare and report diff.
void ExpectGolden(std::string_view name, std::string_view content) {
    const auto path = GoldenDir() / (std::string{name} + ".txt");
    const std::string normalized = normalize_line_endings(
        std::string{content} + "\n");

    if (ShouldUpdateGoldens()) {
        fs::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(out.good()) << "Failed to write golden: " << path;
        out.write(normalized.data(),
                  static_cast<std::streamsize>(normalized.size()));
        SUCCEED() << "Wrote golden: " << path;
        return;
    }

    ASSERT_TRUE(fs::is_regular_file(path))
        << "Missing golden file " << path
        << " (re-run with UPDATE_GOLDENS=1 to create)";

    std::ifstream in{path, std::ios::binary};
    std::string expected_raw(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    const std::string expected = normalize_line_endings(expected_raw);

    EXPECT_EQ(normalized, expected)
        << "Golden mismatch for " << name << "\n"
        << "--- expected (" << path << ")\n"
        << expected
        << "--- actual\n"
        << normalized
        << "---\nRe-run with UPDATE_GOLDENS=1 to regenerate.";
}

// ---------------------------------------------------------------------------
// Spec test: PromptDialogPayload field existence (lines 125-143)
// ---------------------------------------------------------------------------

TEST(PromptDialogPayload, RequiredFieldsExist) {
    // Lines 125-143 of the original test reference fields that the spec
    // mandates MUST exist as named members:
    //   options, tool_input_summary, on_abort, on_response, on_select,
    //   selected_index.
    PromptDialogPayload payload;

    // Explicit references to guarantee the fields are present. If any
    // field is missing or misnamed, this test will fail to compile.
    payload.options = std::vector<PromptOption>{
        PromptOption{"Yes", "yes", "Agree"},
        PromptOption{"No",  "no",  "Disagree"},
    };
    payload.selected_index = 1;
    payload.tool_input_summary = "run tests";

    std::atomic<bool> abort_called{false};
    std::atomic<bool> response_called{false};
    std::atomic<bool> select_called{false};
    std::string selected_key;
    std::optional<std::string> response_value;

    payload.on_abort = [&] { abort_called.store(true); };
    payload.on_response = [&](std::optional<std::string> v) {
        response_called.store(true);
        response_value = std::move(v);
    };
    payload.on_select = [&](const std::string& key) {
        select_called.store(true);
        selected_key = key;
    };

    // Struct offsets / sizes are implementation-defined but the named
    // references above are the real compile-time contract.
    EXPECT_EQ(payload.selected_index, 1);
    ASSERT_EQ(payload.options.size(), 2u);
    EXPECT_EQ(payload.options[0].key, "yes");
    EXPECT_TRUE(payload.tool_input_summary.has_value());
    EXPECT_EQ(*payload.tool_input_summary, "run tests");

    ASSERT_NE(payload.on_abort, nullptr);
    ASSERT_NE(payload.on_response, nullptr);
    ASSERT_NE(payload.on_select, nullptr);

    payload.on_abort();
    payload.on_response("hello");
    payload.on_select("yes");
    EXPECT_TRUE(abort_called.load());
    EXPECT_TRUE(response_called.load());
    EXPECT_TRUE(select_called.load());
    EXPECT_EQ(selected_key, "yes");
    ASSERT_TRUE(response_value.has_value());
    EXPECT_EQ(*response_value, "hello");
}

// ---------------------------------------------------------------------------
// Mode dispatch: SELECT vs FREE-TEXT
// ---------------------------------------------------------------------------

TEST(PromptDialog, ModeDispatch) {
    // SELECT mode: !options.empty()
    PromptDialogPayload select_payload;
    select_payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
    };
    select_payload.title = "Pick";
    Component select = MakePromptDialog(std::move(select_payload));
    EXPECT_NE(select, nullptr);

    // FREE-TEXT mode: options.empty()
    PromptDialogPayload free_payload;
    free_payload.title = "Reply";
    free_payload.options = {};
    Component free_text = MakePromptDialog(std::move(free_payload));
    EXPECT_NE(free_text, nullptr);

    // Sanity: both render without crashing.
    std::string s1 = StripAnsi(RenderComponent(select));
    std::string s2 = StripAnsi(RenderComponent(free_text));
    EXPECT_NE(s1.find("Pick"), std::string::npos);
    EXPECT_NE(s2.find("Reply"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SELECT: goldens
// ---------------------------------------------------------------------------

TEST(PromptDialogSelect, prompt_dialog_select_3_options) {
    PromptDialogPayload payload;
    payload.title = "What would you like to do?";
    payload.options = std::vector<PromptOption>{
        PromptOption{"Edit a file",      "edit",    "FileEditTool"},
        PromptOption{"Run a command",    "bash",    "BashTool"},
        PromptOption{"Search the web",   "web",     "WebSearchTool"},
    };
    payload.selected_index = 0;
    Component c = MakePromptDialog(std::move(payload));
    ExpectGolden("prompt_dialog_select_3_options", StripAnsi(RenderComponent(c)));
}

TEST(PromptDialogSelect, prompt_dialog_select_focus_on_second_row) {
    PromptDialogPayload payload;
    payload.title = "Pick an action";
    payload.options = std::vector<PromptOption>{
        PromptOption{"First",  "a", "option A"},
        PromptOption{"Second", "b", "option B"},
        PromptOption{"Third",  "c", "option C"},
    };
    payload.selected_index = 1; // focus on second row (1-based index 2)
    Component c = MakePromptDialog(std::move(payload));
    ExpectGolden("prompt_dialog_select_focus_on_second_row",
                 StripAnsi(RenderComponent(c)));
}

TEST(PromptDialogSelect, prompt_dialog_with_tool_input_summary) {
    PromptDialogPayload payload;
    payload.title = "Allow this command?";
    payload.options = std::vector<PromptOption>{
        PromptOption{"Allow once",    "once",   "Run now"},
        PromptOption{"Always allow",  "always", "Remember"},
        PromptOption{"Deny",          "deny",   "Reject"},
    };
    payload.selected_index = 0;
    payload.tool_input_summary = "BashTool: rm -rf /tmp/build-cache";
    Component c = MakePromptDialog(std::move(payload));
    ExpectGolden("prompt_dialog_with_tool_input_summary",
                 StripAnsi(RenderComponent(c)));
}

// ---------------------------------------------------------------------------
// SELECT: keyboard behaviour
// ---------------------------------------------------------------------------

TEST(PromptDialogSelect, ArrowDownAndUpMoveFocusWithWrap) {
    std::string fired_key;
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
        PromptOption{"C", "c", ""},
    };
    payload.selected_index = 0;
    payload.on_select = [&](const std::string& k) { fired_key = k; };

    Component c = MakePromptDialog(std::move(payload));

    // ArrowDown twice => focus on C (key "c").
    (void)c->OnEvent(Event::ArrowDown);
    (void)c->OnEvent(Event::ArrowDown);
    (void)c->OnEvent(Event::Return);
    EXPECT_EQ(fired_key, "c");

    // ArrowUp from 0 => wrap to 2 (C) directly.
    fired_key.clear();
    PromptDialogPayload payload2;
    payload2.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
        PromptOption{"C", "c", ""},
    };
    payload2.selected_index = 0;
    payload2.on_select = [&](const std::string& k) { fired_key = k; };
    Component c2 = MakePromptDialog(std::move(payload2));
    (void)c2->OnEvent(Event::ArrowUp); // wraps to C
    (void)c2->OnEvent(Event::Return);
    EXPECT_EQ(fired_key, "c");
}

TEST(PromptDialogSelect, JAndKMoveFocus) {
    std::string fired_key;
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
        PromptOption{"C", "c", ""},
    };
    payload.selected_index = 0;
    payload.on_select = [&](const std::string& k) { fired_key = k; };

    Component c = MakePromptDialog(std::move(payload));
    // k from 0 => wraps to 2 (C).
    (void)c->OnEvent(Event::Character('k'));
    (void)c->OnEvent(Event::Return);
    EXPECT_EQ(fired_key, "c");

    // j twice from 0 => index 2 (C), j again => wrap to 0 (A).
    fired_key.clear();
    PromptDialogPayload payload2;
    payload2.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
        PromptOption{"C", "c", ""},
    };
    payload2.selected_index = 0;
    payload2.on_select = [&](const std::string& k) { fired_key = k; };
    Component c2 = MakePromptDialog(std::move(payload2));
    (void)c2->OnEvent(Event::Character('j')); // 1
    (void)c2->OnEvent(Event::Character('j')); // 2
    (void)c2->OnEvent(Event::Character('j')); // wrap to 0
    (void)c2->OnEvent(Event::Return);
    EXPECT_EQ(fired_key, "a");
}

TEST(PromptDialogSelect, NumericShortcutsCommitImmediately) {
    std::string fired_key;
    std::optional<std::string> resp;
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"One",   "key-1", ""},
        PromptOption{"Two",   "key-2", ""},
        PromptOption{"Three", "key-3", ""},
    };
    payload.selected_index = 0;
    payload.on_select = [&](const std::string& k) { fired_key = k; };
    payload.on_response = [&](std::optional<std::string> r) { resp = std::move(r); };

    Component c = MakePromptDialog(std::move(payload));
    // Press "2" (1-based) => index 1 => key "key-2".
    (void)c->OnEvent(Event::Character('2'));
    EXPECT_EQ(fired_key, "key-2");
    // on_response is NOT fired for numeric shortcuts (SELECT mode uses
    // on_select exclusively when wired).
    EXPECT_FALSE(resp.has_value());
}

TEST(PromptDialogSelect, EnterFiresOnSelectWithFocusedKey) {
    std::string fired_key;
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
    };
    payload.selected_index = 1;
    payload.on_select = [&](const std::string& k) { fired_key = k; };
    Component c = MakePromptDialog(std::move(payload));
    (void)c->OnEvent(Event::Return);
    EXPECT_EQ(fired_key, "b");
}

TEST(PromptDialogSelect, EscapeCallsOnResponseNullopt) {
    std::atomic<bool> abort_called{false};
    std::optional<std::string> resp = "unset";
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
    };
    payload.on_abort = [&] { abort_called.store(true); };
    payload.on_response = [&](std::optional<std::string> r) { resp = std::move(r); };
    Component c = MakePromptDialog(std::move(payload));
    (void)c->OnEvent(Event::Escape);
    EXPECT_TRUE(resp.has_value() == false); // nullopt
    EXPECT_FALSE(abort_called.load()); // abort is separate code path
}

TEST(PromptDialogSelect, CtrlCCallsOnAbortSeparateFromEscape) {
    std::atomic<bool> abort_called{false};
    std::atomic<bool> response_called{false};
    PromptDialogPayload payload;
    payload.options = std::vector<PromptOption>{
        PromptOption{"A", "a", ""},
        PromptOption{"B", "b", ""},
    };
    payload.on_abort = [&] { abort_called.store(true); };
    payload.on_response = [&](std::optional<std::string>) {
        response_called.store(true);
    };
    Component c = MakePromptDialog(std::move(payload));

    // ftxui::Event::CtrlC does not exist. We use Event::Special({0x03})
    // which is the byte-level encoding the terminal emits for Ctrl+C.
    (void)c->OnEvent(Event::Special({0x03}));
    EXPECT_TRUE(abort_called.load());
    EXPECT_FALSE(response_called.load()); // distinct from Esc path
}

// ---------------------------------------------------------------------------
// FREE-TEXT mode: minimal sanity keyboard coverage
// ---------------------------------------------------------------------------

TEST(PromptDialogFreeText, EnterSubmitsEscapeCancelsCtrlCAborts) {
    std::optional<std::string> resp;
    std::atomic<bool> abort_called{false};
    PromptDialogPayload payload;
    payload.options = {}; // FREE-TEXT
    payload.title = "Your name";
    payload.placeholder = "Type your name…";
    payload.on_response = [&](std::optional<std::string> r) { resp = std::move(r); };
    payload.on_abort = [&] { abort_called.store(true); };
    Component c = MakePromptDialog(std::move(payload));

    // Type "hi" + Enter.
    (void)c->OnEvent(Event::Character('h'));
    (void)c->OnEvent(Event::Character('i'));
    (void)c->OnEvent(Event::Return);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(*resp, "hi");

    // Reset and test Escape.
    resp = "unset";
    PromptDialogPayload payload2;
    payload2.options = {};
    payload2.on_response = [&](std::optional<std::string> r) { resp = std::move(r); };
    payload2.on_abort = [&] { abort_called.store(true); };
    Component c2 = MakePromptDialog(std::move(payload2));
    (void)c2->OnEvent(Event::Escape);
    EXPECT_FALSE(resp.has_value()); // nullopt
    EXPECT_FALSE(abort_called.load());

    // Ctrl+C: abort only.
    abort_called.store(false);
    PromptDialogPayload payload3;
    payload3.options = {};
    payload3.on_response = [&](std::optional<std::string>) {
        response_called_global = true;
    };
    payload3.on_abort = [&] { abort_called.store(true); };
    Component c3 = MakePromptDialog(std::move(payload3));
    response_called_global = false;
    (void)c3->OnEvent(Event::Special({0x03}));
    EXPECT_TRUE(abort_called.load());
    EXPECT_FALSE(response_called_global);
}

} // namespace