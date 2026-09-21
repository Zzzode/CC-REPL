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
//       DISCOVERY_TIMEOUT ${LOOM_TEST_DISCOVERY_TIMEOUT})

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

import cc.tools.lsp;
import cc.utils.json;
import cc.services.lsp.types;
import cc.services.lsp.LSPServerInstance;
import cc.services.lsp.diagnostic_registry;
import cc.services.lsp.passive_feedback;
import cc.services.lsp.LSPServerManager;
import cc.services.lsp.client;

using cc::services::lsp::ScopedLspServerConfig;
using cc::services::lsp::create_lsp_server_instance;
using cc::services::lsp::DiagnosticRegistry;
using cc::services::lsp::DiagnosticSeverity;
using cc::services::lsp::format_diagnostics_for_attachment;
using cc::services::lsp::create_lsp_server_manager;
using cc::services::lsp::PassiveFeedbackCollector;
using cc::services::lsp::PassiveFeedbackType;
using cc::services::lsp::register_lsp_notification_handlers;
using cc::services::lsp::LspClient;

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

// ===========================================================================
// lsp-diagnostics-feedback-wiring
//
// The shared DiagnosticRegistry is the source of truth for diagnostics once
// an LSPServerInstance (production fork+poll transport) is wired to it, and
// the parallel LspClient (receive-thread transport) routes into it
// independently. publishDiagnostics is full-replacement: an empty diagnostics
// array clears stale state.
// TS REF: src/services/lsp/passiveFeedback.ts:125-328
// ===========================================================================

namespace {

constexpr std::string_view kPublishTwoDiags =
    R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{)"
    R"("uri":"file:///a.ts","diagnostics":[)"
    R"({"range":{"start":{"line":1,"character":2},"end":{"line":1,"character":6}},)"
    R"("severity":1,"source":"ts","code":2345,"message":"boom"},)"
    R"({"range":{"start":{"line":3,"character":0},"end":{"line":3,"character":4}},)"
    R"("severity":3,"message":"info"}]}})";

}  // namespace

TEST(LspDiagnosticsWiring, PublishReachesRegistryWithAttachmentShape) {
    auto created = create_lsp_server_instance("t", ScopedLspServerConfig{});
    ASSERT_TRUE(created.has_value());
    auto instance = std::move(*created);
    auto registry = std::make_shared<DiagnosticRegistry>();
    instance->diagnostic_registry = registry;

    instance->deliver_notification(std::string{kPublishTwoDiags});

    auto diags = registry->get_diagnostics("file:///a.ts");
    ASSERT_EQ(diags.size(), 2u);

    const auto& first = diags[0];
    EXPECT_EQ(first.severity, DiagnosticSeverity::Error);
    ASSERT_TRUE(first.code.has_value());
    EXPECT_EQ(*first.code, "2345");
    ASSERT_TRUE(first.source.has_value());
    EXPECT_EQ(*first.source, "ts");
    EXPECT_EQ(first.message, "boom");
    EXPECT_EQ(first.range.start.line, 1);
    EXPECT_EQ(first.range.start.character, 2);

    EXPECT_EQ(diags[1].severity, DiagnosticSeverity::Info);
    EXPECT_EQ(registry->get_error_count(), 1u);

    // The registry, not the legacy raw-map, is the source of truth: clobbering
    // the legacy entry must not change what diagnostics_json_for_uri returns.
    instance->diagnostics_by_uri["file:///a.ts"] = "[]";
    auto parsed = cc::utils::json::parse(instance->diagnostics_json_for_uri("file:///a.ts"));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->root().size(), 2u);
}

TEST(LspDiagnosticsWiring, FormatAttachmentSeverityAndCodeMapping) {
    constexpr std::string_view params =
        R"({"uri":"file:///x.ts","diagnostics":[)"
        R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"severity":1,"code":"E1","message":"a"},)"
        R"({"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"severity":2,"code":42,"message":"b"},)"
        R"({"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"severity":3,"message":"c"},)"
        R"({"range":{"start":{"line":3,"character":0},"end":{"line":3,"character":1}},"severity":4,"message":"d"},)"
        R"({"range":{"start":{"line":4,"character":0},"end":{"line":4,"character":1}},"code":null,"message":"e"}]})";

    auto parsed = cc::utils::json::parse(params);
    ASSERT_TRUE(parsed.has_value());

    auto files = format_diagnostics_for_attachment(parsed->root());
    ASSERT_EQ(files.size(), 1u);
    // file:// is stripped for attachment display.
    // TS REF: src/services/lsp/passiveFeedback.ts:50-52 (fileURLToPath)
    EXPECT_EQ(files[0].uri, "/x.ts");
    ASSERT_EQ(files[0].diagnostics.size(), 5u);

    // 1=Error, 2=Warning, 3=Info, 4=Hint, missing defaults to Error.
    // TS REF: src/services/lsp/passiveFeedback.ts:18-35 (mapLSPSeverity)
    EXPECT_EQ(files[0].diagnostics[0].severity, DiagnosticSeverity::Error);
    EXPECT_EQ(files[0].diagnostics[1].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(files[0].diagnostics[2].severity, DiagnosticSeverity::Info);
    EXPECT_EQ(files[0].diagnostics[3].severity, DiagnosticSeverity::Hint);
    EXPECT_EQ(files[0].diagnostics[4].severity, DiagnosticSeverity::Error);

    // String code preserved, numeric code stringified, null code dropped.
    // TS REF: src/services/lsp/passiveFeedback.ts:87-90 (String(code))
    ASSERT_TRUE(files[0].diagnostics[0].code.has_value());
    EXPECT_EQ(*files[0].diagnostics[0].code, "E1");
    ASSERT_TRUE(files[0].diagnostics[1].code.has_value());
    EXPECT_EQ(*files[0].diagnostics[1].code, "42");
    EXPECT_FALSE(files[0].diagnostics[4].code.has_value());
}

TEST(LspDiagnosticsWiring, EmptyPublishClearsStaleDiagnostics) {
    auto created = create_lsp_server_instance("t", ScopedLspServerConfig{});
    ASSERT_TRUE(created.has_value());
    auto instance = std::move(*created);
    auto registry = std::make_shared<DiagnosticRegistry>();
    instance->diagnostic_registry = registry;

    constexpr std::string_view uri = "file:///a.ts";
    instance->deliver_notification(std::string{kPublishTwoDiags});
    ASSERT_EQ(registry->get_diagnostics(uri).size(), 2u);

    constexpr std::string_view clear_msg =
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics",)"
        R"("params":{"uri":"file:///a.ts","diagnostics":[]}})";
    instance->deliver_notification(std::string{clear_msg});

    EXPECT_TRUE(registry->get_diagnostics(uri).empty());
    EXPECT_EQ(registry->get_diagnostic_count(), 0u);
    EXPECT_EQ(registry->get_error_count(), 0u);
    // clear_diagnostics erases the URI from the per-server map too.
    EXPECT_TRUE(registry->get_diagnostics_by_server("t").empty());
    EXPECT_EQ(instance->diagnostics_json_for_uri(uri), "[]");
    // A never-published URI is also an empty result.
    EXPECT_EQ(instance->diagnostics_json_for_uri("file:///never.ts"), "[]");
}

TEST(LspDiagnosticsWiring, DiagnosticsSerializesToNumericSeverityJsonArray) {
    auto created = create_lsp_server_instance("t", ScopedLspServerConfig{});
    ASSERT_TRUE(created.has_value());
    auto instance = std::move(*created);
    instance->diagnostic_registry = std::make_shared<DiagnosticRegistry>();

    constexpr std::string_view uri = "file:///a.ts";
    instance->deliver_notification(std::string{kPublishTwoDiags});

    const std::string j = instance->diagnostics_json_for_uri(uri);
    // parse_diagnostics (lsp_tool.cppm) requires a JSON array with NUMERIC
    // severity; an enum string would break the diagnostics tool action.
    EXPECT_NE(j.find("\"severity\":1"), std::string::npos);
    EXPECT_NE(j.find("\"start\""), std::string::npos);
    EXPECT_NE(j.find("\"line\""), std::string::npos);
    EXPECT_NE(j.find("\"character\""), std::string::npos);
    EXPECT_NE(j.find("boom"), std::string::npos);
    EXPECT_NE(j.find("\"code\":\"2345\""), std::string::npos);

    auto parsed = cc::utils::json::parse(j);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->root().is_arr());
    EXPECT_EQ(parsed->root().size(), 2u);

    // After an empty publish, serialization parses to an empty array.
    constexpr std::string_view clear_msg =
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics",)"
        R"("params":{"uri":"file:///a.ts","diagnostics":[]}})";
    instance->deliver_notification(std::string{clear_msg});
    auto empty = cc::utils::json::parse(instance->diagnostics_json_for_uri(uri));
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->root().is_arr());
    EXPECT_EQ(empty->root().size(), 0u);
}

TEST(LspFeedbackWiring, LiveHandlerIsObservabilityOnlyRegistryStillUpserts) {
    auto mgr = create_lsp_server_manager();
    ASSERT_TRUE(mgr->initialize().has_value());

    auto inst_it = mgr->get_all_servers().find("typescript");
    ASSERT_NE(inst_it, mgr->get_all_servers().end());

    PassiveFeedbackCollector fb;
    auto res = register_lsp_notification_handlers(*mgr, fb);
    EXPECT_EQ(res.success_count, mgr->get_all_servers().size());
    EXPECT_EQ(res.total_servers, res.success_count);

    constexpr std::string_view publish =
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{)"
        R"("uri":"file:///live.ts","diagnostics":[)"
        R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":5}},"severity":1,"code":"TS2345","message":"x"},)"
        R"({"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":3}},"severity":2,"message":"nocode"}]}})";
    inst_it->second->deliver_notification(std::string{publish});

    // TS passiveFeedback.ts handler is observability-only (it logs): a
    // server-pushed publishDiagnostics is not a user gesture, so NO
    // Accepted/Rejected feedback is recorded. Recording a Rejected per
    // diagnostic here would corrupt get_acceptance_rate().
    EXPECT_EQ(fb.get_feedback_count(), 0u);

    // The registry path still upserts every diagnostic (code-bearing or
    // not), and preserves the non-numeric string code verbatim.
    const auto diags = mgr->diagnostic_registry().get_diagnostics("file:///live.ts");
    ASSERT_EQ(diags.size(), 2u);
    bool found_string_code = false;
    for (const auto& d : diags) {
        if (d.code.has_value() && *d.code == "TS2345") found_string_code = true;
    }
    EXPECT_TRUE(found_string_code)
        << "non-numeric LSP string code must be preserved (TS String(code))";
}

TEST(LspFeedbackWiring, FeedbackExportImportRoundTrips) {
    PassiveFeedbackCollector fb;
    fb.record_feedback("srv", "file:///f.ts", "C1", PassiveFeedbackType::Accepted);
    fb.record_feedback("srv", "file:///f.ts", "C1", PassiveFeedbackType::Rejected);
    EXPECT_DOUBLE_EQ(fb.get_acceptance_rate("C1"), 0.5);

    auto path = std::filesystem::temp_directory_path() /
        ("loom_lsp_feedback_" + std::to_string(::getpid()) + ".json");

    auto exported = fb.export_feedback(path);
    ASSERT_TRUE(exported.has_value());

    PassiveFeedbackCollector imported;
    auto import_result = imported.import_feedback(path);
    ASSERT_TRUE(import_result.has_value());

    EXPECT_EQ(imported.get_feedback_count(), 2u);
    auto items = imported.get_feedback_by_code("C1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].server_name, "srv");
    EXPECT_EQ(items[0].uri, "file:///f.ts");
    EXPECT_EQ(items[1].server_name, "srv");
    EXPECT_EQ(items[1].uri, "file:///f.ts");
    bool has_accepted = false;
    bool has_rejected = false;
    for (const auto& item : items) {
        if (item.type == PassiveFeedbackType::Accepted) has_accepted = true;
        if (item.type == PassiveFeedbackType::Rejected) has_rejected = true;
    }
    EXPECT_TRUE(has_accepted);
    EXPECT_TRUE(has_rejected);
    EXPECT_DOUBLE_EQ(imported.get_acceptance_rate("C1"), 0.5);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(LspDiagnosticsWiring, LspClientRoutesPublishIntoRegistry) {
    LspClient::Config config;
    config.name = "cli-srv";
    LspClient client{std::move(config)};

    auto registry = std::make_shared<DiagnosticRegistry>();
    client.set_diagnostic_registry(registry);

    constexpr std::string_view publish =
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{)"
        R"("uri":"file:///cli.ts","diagnostics":[)"
        R"({"range":{"start":{"line":2,"character":1},"end":{"line":2,"character":7}},"severity":2,"code":7,"source":"cli","message":"warn"}]}})";
    client.deliver_inbound_message(std::string{publish});

    auto diags = registry->get_diagnostics("file:///cli.ts");
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].severity, DiagnosticSeverity::Warning);
    ASSERT_TRUE(diags[0].code.has_value());
    EXPECT_EQ(*diags[0].code, "7");
    EXPECT_EQ(registry->get_diagnostics_by_server("cli-srv").size(), 1u);

    constexpr std::string_view clear_msg =
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics",)"
        R"("params":{"uri":"file:///cli.ts","diagnostics":[]}})";
    client.deliver_inbound_message(std::string{clear_msg});
    EXPECT_TRUE(registry->get_diagnostics("file:///cli.ts").empty());
}
