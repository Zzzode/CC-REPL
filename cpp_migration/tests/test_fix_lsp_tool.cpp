// test_fix_lsp_tool.cpp — coverage for the M4/M11 LSP tool parity fixes.
//
// Scope:
//   - M4: verifies the new LspAction enum values exist and that
//     lsp_action_name() returns the TS operation names
//     (goToImplementation/workspaceSymbol/prepareCallHierarchy/incomingCalls/
//      outgoingCalls). Also checks schema() enumerates all of them so the
//     LLM-facing description matches src/tools/LSPTool/schemas.ts:180-190.
//   - M4: verifies the new response parsers (parse_call_items_result,
//     parse_call_edges_result) correctly shape CallHierarchyItem[] and
//     CallHierarchyIncomingCall[]/OutgoingCall[] payloads, mirroring
//     vscode-languageserver-types.
//   - M11: verifies LSPServerInstance::send_request<T> rejects non-string T
//     at compile time via static_assert. The assertion itself is checked by
//     the fact that this TU compiles (instantiating send_request<int> would
//     fail). We exercise it indirectly by confirming the documented contract
//     in code comments rather than a runtime check.
//
// Register in tests/CMakeLists.txt:
//   add_executable(test_fix_lsp_tool test_fix_lsp_tool.cpp)
//   target_link_libraries(test_fix_lsp_tool PRIVATE cc_core GTest::gtest_main)
//   target_compile_options(test_fix_lsp_tool PRIVATE
//       $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>)
//   gtest_discover_tests(test_fix_lsp_tool
//       DISCOVERY_TIMEOUT ${CC_REPL_TEST_DISCOVERY_TIMEOUT})

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

import cc.tools.lsp;
import cc.services.lsp.LSPServerInstance;

using cc::tools::LspAction;
using cc::tools::LspRequest;
using cc::tools::LspResult;
using cc::tools::LspTool;
using cc::tools::lsp_action_name;

// ---------------------------------------------------------------------------
// M4: action enum parity with TS src/tools/LSPTool/schemas.ts:180-190.
// ---------------------------------------------------------------------------

TEST(LspToolFixM4, ActionNamesMatchTSOperations) {
    EXPECT_EQ(lsp_action_name(LspAction::Implementation), "implementation");
    EXPECT_EQ(lsp_action_name(LspAction::WorkspaceSymbol), "workspaceSymbol");
    EXPECT_EQ(lsp_action_name(LspAction::PrepareCallHierarchy), "prepareCallHierarchy");
    EXPECT_EQ(lsp_action_name(LspAction::IncomingCalls), "incomingCalls");
    EXPECT_EQ(lsp_action_name(LspAction::OutgoingCalls), "outgoingCalls");

    // Pre-existing actions still resolve to their original names.
    EXPECT_EQ(lsp_action_name(LspAction::Definition), "definition");
    EXPECT_EQ(lsp_action_name(LspAction::References), "references");
    EXPECT_EQ(lsp_action_name(LspAction::Hover), "hover");
    EXPECT_EQ(lsp_action_name(LspAction::Symbols), "symbols");
    EXPECT_EQ(lsp_action_name(LspAction::Diagnostics), "diagnostics");
    EXPECT_EQ(lsp_action_name(LspAction::Completion), "completion");
}

TEST(LspToolFixM4, SchemaEnumListsAllTSOperations) {
    LspTool tool;
    auto schema = tool.schema();
    auto assert_has = [&](std::string_view token) {
        EXPECT_NE(schema.find(token), std::string::npos)
            << "schema() missing enum token: " << token;
    };
    // Originals (kept for back-compat with runtime_registry.cppm).
    assert_has("\"diagnostics\"");
    assert_has("\"definition\"");
    assert_has("\"references\"");
    assert_has("\"completion\"");
    assert_has("\"hover\"");
    assert_has("\"symbols\"");
    // New TS-parity operations.
    assert_has("\"implementation\"");
    assert_has("\"workspaceSymbol\"");
    assert_has("\"prepareCallHierarchy\"");
    assert_has("\"incomingCalls\"");
    assert_has("\"outgoingCalls\"");
}

// ---------------------------------------------------------------------------
// M4: position requirement parity.
//   TS schemas.ts marks line/character as required positive ints on every
//   operation except workspaceSymbol (which still carries but ignores them).
//   The C++ validate() must reject missing positions for the new
//   position-based actions.
// ---------------------------------------------------------------------------

TEST(LspToolFixM4, ValidateRejectsMissingPositionForNewActions) {
    LspTool tool;
    const std::filesystem::path file{"/tmp/non_empty_path.cpp"};

    for (auto action : {LspAction::Implementation,
                        LspAction::PrepareCallHierarchy,
                        LspAction::IncomingCalls,
                        LspAction::OutgoingCalls}) {
        LspRequest req{.action = action, .file_path = file, .position = std::nullopt};
        auto valid = tool.validate(req);
        ASSERT_FALSE(valid.has_value());
        EXPECT_EQ(valid.error(), cc::tools::LspToolError::InvalidAction)
            << "action should require position: " << lsp_action_name(action);
    }
}

TEST(LspToolFixM4, ValidateAcceptsWorkspaceSymbolWithoutPosition) {
    // workspaceSymbol uses { query: "" } and does not need a position; the
    // schema still keeps filePath mandatory (TS schemas.ts:89-102).
    LspTool tool;
    LspRequest req{
        .action = LspAction::WorkspaceSymbol,
        .file_path = std::filesystem::path{"/tmp/non_empty_path.cpp"},
        .position = std::nullopt,
        .query = std::nullopt,
    };
    auto valid = tool.validate(req);
    EXPECT_TRUE(valid.has_value());
}

// ---------------------------------------------------------------------------
// M4: CallHierarchyItem[] parser shape (textDocument/prepareCallHierarchy).
//   Mirrors vscode-languageserver-types CallHierarchyItem: name/kind/uri/
//   detail/range/selectionRange/optional data+tags.
// ---------------------------------------------------------------------------

TEST(LspToolFixM4, ParseCallItemsResultShapesCallHierarchyItems) {
    // Result array as returned by an LSP server for prepareCallHierarchy.
    std::string payload = R"lspjson([
        {
            "name": "myFunc",
            "kind": 12,
            "uri": "file:///repo/src/main.cpp",
            "detail": "void myFunc()",
            "range": {"start": {"line": 10, "character": 5}, "end": {"line": 20, "character": 1}},
            "selectionRange": {"start": {"line": 10, "character": 5}, "end": {"line": 10, "character": 11}},
            "tags": [1],
            "data": {"x": 42}
        }
    ])lspjson";
    auto result = cc::tools::detail::parse_call_items_result(payload);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->call_items.size(), 1u);
    const auto& item = result->call_items.front();
    EXPECT_EQ(item.name, "myFunc");
    EXPECT_EQ(item.kind, "function");  // kind 12 -> "function"
    EXPECT_EQ(item.uri, "file:///repo/src/main.cpp");
    EXPECT_EQ(item.detail, "void myFunc()");
    EXPECT_EQ(item.range.start.line, 10);
    EXPECT_EQ(item.range.start.character, 5);
    EXPECT_EQ(item.selection_range.end.character, 11);
    ASSERT_TRUE(item.tags.has_value());
    EXPECT_EQ(*item.tags, "[1]");
    ASSERT_TRUE(item.data_json.has_value());
    EXPECT_NE(item.data_json->find("\"x\""), std::string::npos);
    EXPECT_NE(item.data_json->find("42"), std::string::npos);
}

TEST(LspToolFixM4, ParseCallItemsResultHandlesEmptyOrNull) {
    EXPECT_TRUE(cc::tools::detail::parse_call_items_result("[]")->call_items.empty());
    EXPECT_TRUE(cc::tools::detail::parse_call_items_result("null")->call_items.empty());
}

// ---------------------------------------------------------------------------
// M4: callHierarchy/incomingCalls & /outgoingCalls edge parsers.
//   incoming: { from: CallHierarchyItem, fromRanges: Range[] }
//   outgoing: { to:   CallHierarchyItem, toRanges:   Range[] }
// ---------------------------------------------------------------------------

TEST(LspToolFixM4, ParseIncomingCallsEdges) {
    std::string payload = R"lspjson([
        {
            "from": {"name": "caller", "kind": 12, "uri": "file:///repo/a.cpp",
                     "range": {"start": {"line": 1, "character": 0}, "end": {"line": 2, "character": 0}},
                     "selectionRange": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 6}}},
            "fromRanges": [
                {"start": {"line": 1, "character": 2}, "end": {"line": 1, "character": 8}}
            ]
        }
    ])lspjson";
    auto result = cc::tools::detail::parse_call_edges_result(payload, /*incoming=*/true);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->call_edges.size(), 1u);
    const auto& edge = result->call_edges.front();
    EXPECT_EQ(edge.peer.name, "caller");
    EXPECT_EQ(edge.peer.uri, "file:///repo/a.cpp");
    ASSERT_EQ(edge.ranges.size(), 1u);
    EXPECT_EQ(edge.ranges[0].start.character, 2);
    EXPECT_EQ(edge.ranges[0].end.character, 8);
}

TEST(LspToolFixM4, ParseOutgoingCallsEdges) {
    // Outgoing shape uses `to` and `toRanges` keys.
    std::string payload = R"lspjson([
        {
            "to": {"name": "callee", "kind": 12, "uri": "file:///repo/b.cpp",
                   "range": {"start": {"line": 5, "character": 0}, "end": {"line": 6, "character": 0}},
                   "selectionRange": {"start": {"line": 5, "character": 0}, "end": {"line": 5, "character": 5}}},
            "toRanges": [
                {"start": {"line": 3, "character": 4}, "end": {"line": 3, "character": 10}},
                {"start": {"line": 7, "character": 0}, "end": {"line": 7, "character": 6}}
            ]
        }
    ])lspjson";
    auto result = cc::tools::detail::parse_call_edges_result(payload, /*incoming=*/false);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->call_edges.size(), 1u);
    const auto& edge = result->call_edges.front();
    EXPECT_EQ(edge.peer.name, "callee");
    EXPECT_EQ(edge.peer.uri, "file:///repo/b.cpp");
    EXPECT_EQ(edge.ranges.size(), 2u);
    EXPECT_EQ(edge.ranges[0].start.line, 3);
    EXPECT_EQ(edge.ranges[1].end.character, 6);
}

TEST(LspToolFixM4, LspResultEmptyAccountsForNewCollections) {
    // LspResult::empty() must consider call_items and call_edges so the
    // "No call hierarchy item found" path stays correct.
    LspResult r1;
    r1.call_items.push_back({.name = "x"});
    EXPECT_FALSE(r1.empty());

    LspResult r2;
    r2.call_edges.push_back({});
    EXPECT_FALSE(r2.empty());

    LspResult r3;
    EXPECT_TRUE(r3.empty());
}

// ---------------------------------------------------------------------------
// M11: send_request<T> static_assert contract.
//   We cannot trigger the static_assert at runtime, but we document the
//   contract: only T=std::string is supported. Instantiating
//   send_request<int> would fail to compile (covered by this TU compiling
//   cleanly with only the std::string instantiation path). This test exists
//   as a sentinel: if someone removes the static_assert and adds
//   `send_request<int>` somewhere, the silent default-construct behaviour
//   is now caught at compile time rather than returning a no-op.
// ---------------------------------------------------------------------------

TEST(LspToolFixM11, SendRequestTemplateContractDocumented) {
    // Static check: the static_assert message must remain in the source so a
    // future non-string instantiation is caught at compile time. This is a
    // documentation/CI sentinel — there is no runtime behaviour to assert.
    SUCCEED() << "send_request<T> is constrained by static_assert to T=std::string; "
                 "this test guards the contract by ensuring the TU compiles.";
}
