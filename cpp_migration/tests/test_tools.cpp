/// @file test_tools.cpp
/// @brief Tool registry smoke tests aligned with current C++ modules.

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import cc.tools.bash;
import cc.tools.computer_use;
import cc.tools.web_fetch;
import cc.tools.web_search;
import cc.tools.web_browser;
import cc.tools.mcp;
import cc.tools.agent;
import cc.tools.agent_runtime;
import cc.tools.notebook;
import cc.tools.registry;
import cc.tools.runtime_registry;
import cc.tools.tool;
import cc.utils.json;

namespace fs = std::filesystem;

namespace {

struct CurrentPathGuard {
    fs::path previous;

    explicit CurrentPathGuard(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};

struct EnvironmentGuard {
    std::string name;
    std::optional<std::string> previous;

    EnvironmentGuard(std::string key, const std::string& value) : name(std::move(key)) {
        if (const char* existing = std::getenv(name.c_str())) {
            previous = existing;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~EnvironmentGuard() {
        if (previous) {
            setenv(name.c_str(), previous->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

} // namespace

TEST(ToolRegistry, ListsBuiltInTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    EXPECT_FALSE(names.empty());
}

TEST(ToolRegistry, ContainsExpectedTools) {
    auto names = cc::tools::registry::builtin_tool_names();
    ASSERT_FALSE(names.empty());

    // Check that known tools are present
    bool has_bash = false;
    bool has_computer_use = false;
    bool has_lsp = false;
    bool has_skill = false;
    bool has_task_create = false;
    bool has_web_browser = false;
    for (const auto& name : names) {
        if (name == "Bash") has_bash = true;
        if (name == "computer_use") has_computer_use = true;
        if (name == "lsp") has_lsp = true;
        if (name == "skill") has_skill = true;
        if (name == "task_create") has_task_create = true;
        if (name == "web_browser") has_web_browser = true;
    }
    EXPECT_TRUE(has_bash);
    EXPECT_TRUE(has_computer_use);
    EXPECT_TRUE(has_lsp);
    EXPECT_TRUE(has_skill);
    EXPECT_TRUE(has_task_create);
    EXPECT_TRUE(has_web_browser);
}

TEST(ToolRegistry, CoreRegistryCanBeConstructed) {
    cc::tools::registry::ToolRegistry registry;
    EXPECT_EQ(registry.size(), 0u);  // Empty by default
}

TEST(ToolRegistry, RegistersRuntimeTools) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    EXPECT_GT(registry.size(), 0u);
    EXPECT_TRUE(registry.contains("Bash"));
    EXPECT_TRUE(registry.contains("computer_use"));
    EXPECT_TRUE(registry.contains("Read"));
    EXPECT_TRUE(registry.contains("web_browser"));
    EXPECT_TRUE(registry.contains("mcp"));
    EXPECT_TRUE(registry.contains("lsp"));
    EXPECT_TRUE(registry.contains("skill"));
    EXPECT_TRUE(registry.contains("task_create"));
}

TEST(Tools, WebBrowserToolUsesScreenshotBackend) {
    bool called = false;
    cc::tools::WebBrowserTool tool([&](
        const cc::tools::BrowserRequest& request,
        const cc::tools::PageState& state) -> std::expected<std::string, cc::tools::BrowserError> {
        called = true;
        EXPECT_EQ(request.action, cc::tools::BrowserAction::Screenshot);
        EXPECT_TRUE(state.url().empty());
        return std::string("iVBORw0KGgo=");
    });

    auto result = tool.execute(cc::tools::BrowserRequest{
        .action = cc::tools::BrowserAction::Screenshot,
        .url = "https://example.test",
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(called);
    EXPECT_EQ(result->content, "Captured browser screenshot.");
    ASSERT_TRUE(result->screenshot_base64.has_value());
    EXPECT_EQ(*result->screenshot_base64, "iVBORw0KGgo=");
    EXPECT_EQ(result->media_type, std::optional<std::string>{"image/png"});
}

TEST(Tools, ComputerUseManagerUsesCaptureProviderForScreenshot) {
    using namespace cc::core::computer_use;

    bool saw_region = false;
    ComputerUseManager manager(ScreenCapture([&](std::optional<Rect> region)
        -> std::expected<ImageData, std::string> {
        saw_region = region.has_value();
        if (region) {
            EXPECT_EQ(region->x, 1);
            EXPECT_EQ(region->y, 2);
            EXPECT_EQ(region->width, 3u);
            EXPECT_EQ(region->height, 4u);
        }
        return ImageData{
            .pixels = {1, 2, 3, 4},
            .width = 3,
            .height = 4,
            .format = "rgba",
        };
    }));

    auto result = manager.execute_action(ComputerAction{
        .type = ActionType::Screenshot,
        .position = std::nullopt,
        .drag_end = std::nullopt,
        .text = std::nullopt,
        .region = Rect{.x = 1, .y = 2, .width = 3, .height = 4},
        .keys = {},
    });

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(saw_region);
    ASSERT_TRUE(result.screenshot.has_value());
    EXPECT_EQ(result.screenshot->width, 3u);
    EXPECT_EQ(result.screenshot->height, 4u);
    EXPECT_EQ(result.screenshot->format, "rgba");
}

TEST(Tools, LspToolUsesConfiguredLanguageServer) {
    auto root = fs::temp_directory_path() / "cc_repl_runtime_lsp_tool_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "plugins" / "lsp-runtime-fixture");
    const auto plugin_root = root / ".claude" / "plugins" / "lsp-runtime-fixture";
    const auto server_path = plugin_root / "server.js";
    const auto source_path = root / "sample.foo";
    {
        std::ofstream source(source_path);
        source << "function fixtureSymbol() { return fixtureCompletion; }\n";
    }
    {
        std::ofstream server(server_path);
        server << R"JS(
let buffer = Buffer.alloc(0);

function send(message) {
  const body = JSON.stringify(message);
  process.stdout.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}

function position(line, character) {
  return { line, character };
}

function range(line, character) {
  return { start: position(line, character), end: position(line, character + 4) };
}

function handle(message) {
  const uri = message.params?.textDocument?.uri || 'file:///fixture';
  if (message.method === 'initialize') {
    send({ jsonrpc: '2.0', id: message.id, result: { capabilities: { textDocumentSync: 1 } } });
    return;
  }
  if (message.method === 'textDocument/didOpen') {
    send({
      jsonrpc: '2.0',
      method: 'textDocument/publishDiagnostics',
      params: {
        uri,
        diagnostics: [{
          range: range(1, 2),
          severity: 1,
          source: 'fixture',
          message: 'fixture diagnostic',
          code: 'F001'
        }]
      }
    });
    return;
  }
  if (message.method === 'textDocument/definition') {
    send({ jsonrpc: '2.0', id: message.id, result: [{ uri, range: range(7, 3) }] });
    return;
  }
  if (message.method === 'textDocument/references') {
    send({ jsonrpc: '2.0', id: message.id, result: [{ uri, range: range(8, 4) }] });
    return;
  }
  if (message.method === 'textDocument/completion') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { isIncomplete: false, items: [{ label: 'fixtureCompletion', kind: 3, detail: 'callable' }] }
    });
    return;
  }
  if (message.method === 'textDocument/hover') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: { contents: { kind: 'markdown', value: 'fixture hover' }, range: range(2, 1) }
    });
    return;
  }
  if (message.method === 'textDocument/documentSymbol') {
    send({
      jsonrpc: '2.0',
      id: message.id,
      result: [{
        name: 'fixtureSymbol',
        kind: 12,
        range: range(0, 9),
        selectionRange: range(0, 9)
      }]
    });
    return;
  }
  if (message.method === 'exit') process.exit(0);
}

process.stdin.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const headerEnd = buffer.indexOf('\r\n\r\n');
    if (headerEnd === -1) return;
    const header = buffer.subarray(0, headerEnd).toString();
    const match = /Content-Length:\s*(\d+)/i.exec(header);
    if (!match) process.exit(2);
    const length = Number(match[1]);
    const bodyStart = headerEnd + 4;
    if (buffer.length < bodyStart + length) return;
    const body = buffer.subarray(bodyStart, bodyStart + length).toString();
    buffer = buffer.subarray(bodyStart + length);
    handle(JSON.parse(body));
  }
});
process.stdin.resume();
)JS";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "lsp-runtime-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "lspServers": {
    "fixture": {
      "command": "node",
      "args": ["${CLAUDE_PLUGIN_ROOT}/server.js"],
      "extensionToLanguage": {".foo": "foo"}
    }
  }
})JSON";
    }

    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard("CLAUDE_CODE_PLUGIN_CACHE_DIR", (root / ".claude" / "plugins").string());
    CurrentPathGuard cwd(root);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto execute_lsp = [&](std::string_view action) {
        cc::utils::json::JsonMutDoc doc;
        auto input = doc.object();
        input.add("action", doc.string(action));
        input.add("file_path", doc.string(source_path.string()));
        input.add("line", doc.number(static_cast<int64_t>(0)));
        input.add("character", doc.number(static_cast<int64_t>(9)));
        doc.set_root(input);
        return registry.execute("lsp", cc::core::ToolInput::from_json(doc.to_string()));
    };

    auto definition = execute_lsp("definition");
    ASSERT_TRUE(definition.has_value());
    EXPECT_FALSE(definition->is_error) << definition->content.front().text;
    EXPECT_NE(definition->content.front().text.find(":7:3"), std::string::npos);

    auto completion = execute_lsp("completion");
    ASSERT_TRUE(completion.has_value());
    EXPECT_FALSE(completion->is_error) << completion->content.front().text;
    EXPECT_NE(completion->content.front().text.find("fixtureCompletion callable"), std::string::npos);

    auto hover = execute_lsp("hover");
    ASSERT_TRUE(hover.has_value());
    EXPECT_FALSE(hover->is_error) << hover->content.front().text;
    EXPECT_NE(hover->content.front().text.find("fixture hover"), std::string::npos);

    auto symbols = execute_lsp("symbols");
    ASSERT_TRUE(symbols.has_value());
    EXPECT_FALSE(symbols->is_error) << symbols->content.front().text;
    EXPECT_NE(symbols->content.front().text.find("function fixtureSymbol"), std::string::npos);

    auto diagnostics = execute_lsp("diagnostics");
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->is_error) << diagnostics->content.front().text;
    EXPECT_NE(diagnostics->content.front().text.find("fixture:1:2 fixture diagnostic"), std::string::npos);

    fs::remove_all(root);
}

TEST(ToolInput, HasFieldParsesTopLevelJsonKeys) {
    auto input = cc::core::ToolInput::from_json(R"({
      "cwd": null,
      "description": "command mentions timeout and nested_field",
      "nested": {"command": "pwd"}
    })");

    EXPECT_TRUE(input.has_field("cwd"));
    EXPECT_TRUE(input.has_field("description"));
    EXPECT_TRUE(input.has_field("nested"));
    EXPECT_FALSE(input.has_field("timeout"));
    EXPECT_FALSE(input.has_field("command"));
    EXPECT_FALSE(input.has_field("nested_field"));
    EXPECT_FALSE(input.has_field(""));
}

TEST(ToolInput, HasFieldReturnsFalseForInvalidOrNonObjectJson) {
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"("cwd")").has_field("cwd"));
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"(["cwd"])").has_field("cwd"));
    EXPECT_FALSE(cc::core::ToolInput::from_json(R"({"cwd")").has_field("cwd"));
}

TEST(Tools, BashToolCapturesStderrAndNonZeroExitCode) {
    cc::tools::BashTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "printf out; printf err >&2; exit 7"
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("err"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("out"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Exit code: 7"), std::string::npos);
}

TEST(Tools, BashToolUsesCwdWithoutShellInterpolatingIt) {
    auto root = fs::temp_directory_path() / "cc repl bash cwd test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::BashTool tool;
    auto input = std::format(R"({{"command":"pwd","cwd":"{}"}})", root.string());
    auto result = tool.execute(cc::core::ToolInput::from_json(input));

    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find(root.string()), std::string::npos);
}

TEST(Tools, BashToolTimesOutLongRunningCommands) {
    cc::tools::BashTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "command": "sleep 2",
      "timeout": 50
    })"));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Command timed out"), std::string::npos);
}

TEST(Tools, BashToolStartsBackgroundCommands) {
    auto root = fs::temp_directory_path() / "cc_repl_bash_background_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::BashTool tool;
    auto input = std::format(R"({{"command":"sleep 0.1; printf done > background.txt","cwd":"{}","run_in_background":true}})",
        root.string());
    auto result = tool.execute(cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Background task started"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("coming soon"), std::string::npos);

    const auto output_path = root / "background.txt";
    for (int attempt = 0; attempt < 20 && !fs::exists(output_path); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(fs::exists(output_path));
    fs::remove_all(root);
}

TEST(Tools, WebFetchParsesEscapedUrlFromJson) {
    auto parsed = cc::tools::web_fetch::detail::parse_url(R"({"url":"https://example.com/a?x=\"quoted\"&y=1"})");

    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, R"(https://example.com/a?x="quoted"&y=1)");
    EXPECT_FALSE(cc::tools::web_fetch::detail::parse_url(R"({"description":"contains url"})").has_value());
}

TEST(Tools, WebSearchParsesEscapedQueryFromJson) {
    auto parsed = cc::tools::web_search::detail::parse_query(R"({"query":"C++ \"modules\" migration"})");

    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, R"(C++ "modules" migration)");
    EXPECT_FALSE(cc::tools::web_search::detail::parse_query(R"({"description":"contains query"})").has_value());
}

TEST(Tools, WebSearchFormatsDuckDuckGoHtmlResults) {
    const std::string html = R"HTML(
      <div class="result">
        <a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fdocs%3Fx%3D1%26y%3D2&amp;rut=abc">
          Example &amp; Docs
        </a>
        <a class="result__snippet">Docs <b>about</b> migration &amp; testing.</a>
      </div>
      <div class="result">
        <a class="result__a" href="https://second.example/path">Second Result</a>
      </div>
    )HTML";

    auto formatted = cc::tools::web_search::detail::format_results("migration test", html);

    EXPECT_NE(formatted.find("Search results for: migration test"), std::string::npos);
    EXPECT_NE(formatted.find("1. Example & Docs"), std::string::npos);
    EXPECT_NE(formatted.find("https://example.com/docs?x=1&y=2"), std::string::npos);
    EXPECT_NE(formatted.find("Docs about migration & testing."), std::string::npos);
    EXPECT_NE(formatted.find("2. Second Result"), std::string::npos);
    EXPECT_EQ(formatted.find("result__a"), std::string::npos);
}

TEST(Tools, NotebookEditPreservesNotebookJsonStructure) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_roundtrip_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "sample.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {
    "language_info": {"name": "python"},
    "custom": {"keep": true}
  },
  "cells": [
    {
      "cell_type": "code",
      "id": "abc123",
      "metadata": {"tags": ["keep-me"]},
      "source": ["print('old')\n"],
      "execution_count": 12,
      "outputs": [
        {"output_type": "stream", "name": "stdout", "text": ["old\n"]}
      ]
    },
    {
      "cell_type": "markdown",
      "id": "md1",
      "metadata": {"collapsed": false},
      "source": "unchanged markdown"
    }
  ]
})JSON";
    }

    cc::tools::NotebookEditTool tool;
    auto result = tool.execute(cc::tools::NotebookEditRequest{
        .notebook_path = notebook_path,
        .operation = cc::tools::CellOperation::Update,
        .cell_index = 0,
        .target_index = std::nullopt,
        .cell_type = std::nullopt,
        .source = std::string(R"(print("new value"))"),
    });

    ASSERT_TRUE(result.has_value()) << std::string(cc::tools::format_error(result.error()));
    EXPECT_EQ(result->total_cells, 2u);

    auto parsed = cc::utils::json::parse_file(notebook_path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto root_json = parsed->root();
    EXPECT_EQ(root_json.get("metadata").get("language_info").get_string("name"), "python");
    EXPECT_TRUE(root_json.get("metadata").get("custom").get("keep").as_bool());

    auto cells = root_json.get("cells");
    ASSERT_TRUE(cells.is_arr());
    ASSERT_EQ(cells.size(), 2u);
    auto first = cells.at(0);
    EXPECT_EQ(first.get_string("cell_type"), "code");
    EXPECT_EQ(first.get_string("id"), "abc123");
    EXPECT_EQ(first.get("metadata").get("tags").at(0).as_str(), "keep-me");
    EXPECT_EQ(first.get_string("source"), R"(print("new value"))");
    EXPECT_TRUE(first.get("execution_count").is_null());
    EXPECT_EQ(first.get("outputs").size(), 0u);

    auto second = cells.at(1);
    EXPECT_EQ(second.get_string("cell_type"), "markdown");
    EXPECT_EQ(second.get_string("id"), "md1");
    EXPECT_FALSE(second.get("metadata").get("collapsed").as_bool());
    EXPECT_EQ(second.get_string("source"), "unchanged markdown");

    fs::remove_all(root);
}

TEST(Tools, NotebookRuntimeAdapterAcceptsTypeScriptInputShape) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "runtime.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {"language_info": {"name": "python"}},
  "cells": [
    {"cell_type": "markdown", "id": "first", "metadata": {}, "source": "before"}
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"JSONFMT({{
      "notebook_path": "{}",
      "cell_id": "first",
      "edit_mode": "insert",
      "cell_type": "code",
      "new_source": "print(\"inserted\")"
    }})JSONFMT", notebook_path.string());
    auto result = registry.execute("notebook_edit", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Inserted cell at index 1"), std::string::npos);

    auto parsed = cc::utils::json::parse_file(notebook_path);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto cells = parsed->root().get("cells");
    ASSERT_EQ(cells.size(), 2u);
    EXPECT_EQ(cells.at(0).get_string("source"), "before");
    EXPECT_EQ(cells.at(1).get_string("cell_type"), "code");
    EXPECT_TRUE(cells.at(1).has("id"));
    EXPECT_EQ(cells.at(1).get_string("source"), R"(print("inserted"))");
    EXPECT_TRUE(cells.at(1).get("execution_count").is_null());
    EXPECT_EQ(cells.at(1).get("outputs").size(), 0u);

    fs::remove_all(root);
}

TEST(Tools, FileReadFormatsNotebookCellsForToolResult) {
    auto root = fs::temp_directory_path() / "cc_repl_notebook_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto notebook_path = root / "read.ipynb";
    {
        std::ofstream notebook(notebook_path);
        notebook << R"JSON({
  "nbformat": 4,
  "nbformat_minor": 5,
  "metadata": {"language_info": {"name": "r"}},
  "cells": [
    {
      "cell_type": "code",
      "id": "code-cell",
      "metadata": {},
      "source": ["print(1)\n"],
      "execution_count": 1,
      "outputs": [
        {"output_type": "stream", "name": "stdout", "text": ["1\n"]}
      ]
    },
    {
      "cell_type": "markdown",
      "metadata": {},
      "source": "markdown text"
    }
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", notebook_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    const auto& text = result->content.front().text;
    EXPECT_NE(text.find("Notebook:"), std::string::npos);
    EXPECT_NE(text.find(R"CHECK(<cell id="code-cell"><language>r</language>print(1))CHECK"), std::string::npos);
    EXPECT_NE(text.find("\n1\n"), std::string::npos);
    EXPECT_NE(text.find(R"(<cell id="cell-1"><cell_type>markdown</cell_type>markdown text</cell id="cell-1">)"), std::string::npos);
    EXPECT_EQ(text.find(R"("nbformat")"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, FileReadReturnsImageContentBlock) {
    auto root = fs::temp_directory_path() / "cc_repl_image_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto image_path = root / "pixel.png";
    {
        const unsigned char png_header[] = {
            0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
            0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x02,
        };
        std::ofstream image(image_path, std::ios::binary);
        image.write(reinterpret_cast<const char*>(png_header), sizeof(png_header));
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", image_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_EQ(result->content.size(), 2u);
    EXPECT_NE(result->content[0].text.find("Image file read:"), std::string::npos);
    EXPECT_EQ(result->content[1].format, std::optional<std::string>{"image"});
    EXPECT_EQ(result->content[1].media_type, std::optional<std::string>{"image/png"});
    ASSERT_TRUE(result->content[1].data.has_value());
    EXPECT_TRUE(result->content[1].data->starts_with("iVBOR"));

    fs::remove_all(root);
}

TEST(Tools, FileReadReturnsPdfDocumentBlock) {
    auto root = fs::temp_directory_path() / "cc_repl_pdf_read_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto pdf_path = root / "doc.pdf";
    {
        std::ofstream pdf(pdf_path, std::ios::binary);
        pdf << "%PDF-1.4\n%%EOF\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto input = std::format(R"({{"file_path":"{}"}})", pdf_path.string());
    auto result = registry.execute("Read", cc::core::ToolInput::from_json(input));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error);
    ASSERT_EQ(result->content.size(), 2u);
    EXPECT_NE(result->content[0].text.find("PDF file read:"), std::string::npos);
    EXPECT_EQ(result->content[1].format, std::optional<std::string>{"document"});
    EXPECT_EQ(result->content[1].media_type, std::optional<std::string>{"application/pdf"});
    ASSERT_TRUE(result->content[1].data.has_value());
    EXPECT_TRUE(result->content[1].data->starts_with("JVBER"));

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeLoadsMarkdownDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "reviewer.md");
        agent << R"MD(---
name: reviewer
description: Reviews code changes
model: haiku
tools: [Read, Grep, Bash]
disallowedTools: [Bash]
maxTurns: 8
initialPrompt: Inspect only changed files first.
---
You review code changes and report risks.
)MD";
    }

    auto agents = cc::tools::agent_runtime::load_agent_definitions_from_dir(
        root / ".claude" / "agents",
        "projectSettings");

    ASSERT_EQ(agents.size(), 1u);
    EXPECT_EQ(agents.front().agent_type, "reviewer");
    EXPECT_EQ(agents.front().when_to_use, "Reviews code changes");
    EXPECT_EQ(agents.front().model, "haiku");
    ASSERT_EQ(agents.front().tools.size(), 3u);
    EXPECT_EQ(agents.front().tools.front(), "Read");
    ASSERT_EQ(agents.front().disallowed_tools.size(), 1u);
    EXPECT_EQ(agents.front().disallowed_tools.front(), "Bash");
    ASSERT_TRUE(agents.front().max_turns.has_value());
    EXPECT_EQ(*agents.front().max_turns, 8);
    ASSERT_TRUE(agents.front().initial_prompt.has_value());
    EXPECT_EQ(*agents.front().initial_prompt, "Inspect only changed files first.");

    fs::remove_all(root);
}

TEST(Tools, AgentRuntimeResolvesLooseAgentTypeInputs) {
    auto agents = cc::tools::agent_runtime::built_in_agent_definitions();

    auto general = cc::tools::agent_runtime::resolve_requested_agent_type("General Purpose", agents);
    ASSERT_TRUE(general.has_value());
    EXPECT_EQ(*general, "general-purpose");

    auto planner = cc::tools::agent_runtime::resolve_requested_agent_type("planner", agents);
    ASSERT_TRUE(planner.has_value());
    EXPECT_EQ(*planner, "Plan");

    auto explorer = cc::tools::agent_runtime::resolve_requested_agent_type("explorer", agents);
    ASSERT_TRUE(explorer.has_value());
    EXPECT_EQ(*explorer, "Explore");

    EXPECT_FALSE(cc::tools::agent_runtime::resolve_requested_agent_type("missing-agent-type", agents).has_value());
}

TEST(Tools, AgentToolAcceptsTypeScriptInputShape) {
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Inspect plan",
      "prompt": "Inspect the migration plan",
      "subagent_type": "planner",
      "model": "haiku"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("Missing required"), std::string::npos);
}

TEST(Tools, AgentToolRejectsUnknownAgentTypesBeforeExecution) {
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Missing agent",
      "prompt": "Use an unknown agent",
      "subagent_type": "cc-repl-missing-agent-type"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent type 'cc-repl-missing-agent-type' not found"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
}

TEST(Tools, AgentToolLoadsProjectAgentDefinitions) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_tool_project_test";
    auto previous_cwd = fs::current_path();
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "project-reviewer.md");
        agent << R"MD(---
name: project-reviewer
description: Reviews project changes
model: haiku
tools: [Read, Grep]
maxTurns: 2
---
Review the project change and report concrete risks.
)MD";
    }

    fs::current_path(root);
    cc::tools::AgentConfig config;
    config.max_depth = 0;
    cc::tools::AgentTool tool(config);

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Review changes",
      "prompt": "Review this migration change",
      "subagent_type": "project-reviewer"
    })"));

    fs::current_path(previous_cwd);
    fs::remove_all(root);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("not found"), std::string::npos);
}

TEST(Tools, AgentToolAppliesInitialPromptAndToolRestrictionsInExecutionPlan) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_plan_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "restricted.md");
        agent << R"MD(---
name: restricted-reviewer
description: Reviews with restricted tools
model: haiku
tools: [Read, Bash]
disallowedTools: [Bash]
initialPrompt: First inspect the diff.
maxTurns: 2
---
Review with a narrow tool set.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "restricted-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        EXPECT_EQ(plan->agent_type, "restricted-reviewer");
        EXPECT_EQ(plan->model, "claude-3-5-haiku-20241022");
        EXPECT_EQ(plan->max_turns, 2);
        EXPECT_NE(plan->prompt.find("First inspect the diff.\n\nReview this change."), std::string::npos);
        ASSERT_EQ(plan->allowed_tools.size(), 2u);
        EXPECT_EQ(plan->allowed_tools.front(), "Read");
        ASSERT_EQ(plan->disallowed_tools.size(), 1u);
        EXPECT_EQ(plan->disallowed_tools.front(), "Bash");
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolPreloadsSkillsFromDefinition) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_skills_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    fs::create_directories(root / ".claude" / "skills" / "review-skill");
    {
        std::ofstream agent(root / ".claude" / "agents" / "skillful.md");
        agent << R"MD(---
name: skillful-reviewer
description: Reviews with preloaded skills
skills: [review-skill, missing-skill]
---
Review with a preloaded workflow.
)MD";
    }
    {
        std::ofstream skill(root / ".claude" / "skills" / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Review skill
---
Inspect the patch before reporting findings.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "skillful-reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->preloaded_skill_messages.size(), 1u);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("review-skill"), std::string::npos);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("Inspect the patch"), std::string::npos);
        EXPECT_EQ(plan->preloaded_skill_messages.front().find("missing-skill"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsPluginAgentsAndPluginSkills) {
    auto root = fs::temp_directory_path() / "cc_repl_plugin_agent_skills_test";
    fs::remove_all(root);
    const auto plugin_root = root / ".claude" / "plugins" / "plugin-fixture";
    fs::create_directories(plugin_root / "agents");
    fs::create_directories(plugin_root / "skills" / "review-skill");
    {
        std::ofstream entry(plugin_root / "plugin.js");
        entry << "process.exit(0)\n";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << R"JSON({
  "name": "plugin-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "description": "Plugin fixture"
})JSON";
    }
    {
        std::ofstream agent(plugin_root / "agents" / "reviewer.md");
        agent << R"MD(---
name: reviewer
description: Reviews using plugin resources
skills: [review-skill]
hooks: [ignored-for-plugin-agents]
mcpServers: [ignored-server]
---
Review with plugin context.
)MD";
    }
    {
        std::ofstream skill(plugin_root / "skills" / "review-skill" / "SKILL.md");
        skill << R"MD(---
description: Plugin review skill
---
Use the plugin review checklist.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        auto agents = cc::tools::agent_runtime::get_all_agent_definitions();
        auto it = std::ranges::find_if(agents, [](const auto& agent) {
            return agent.agent_type == "plugin-fixture:reviewer";
        });
        ASSERT_NE(it, agents.end());
        EXPECT_EQ(it->source, "plugin");
        EXPECT_FALSE(it->hooks_present);
        EXPECT_TRUE(it->mcp_servers.empty());

        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Review this change.";
        request.subagent_type = "plugin-fixture:reviewer";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->preloaded_skill_messages.size(), 1u);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("plugin-fixture:review-skill"), std::string::npos);
        EXPECT_NE(plan->preloaded_skill_messages.front().find("Use the plugin review checklist"), std::string::npos);

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        auto skill = registry.execute("skill", cc::core::ToolInput::from_json(R"({
          "name": "plugin-fixture:review-skill"
        })"));
        ASSERT_TRUE(skill.has_value());
        ASSERT_FALSE(skill->is_error);
        ASSERT_FALSE(skill->content.empty());
        EXPECT_NE(skill->content.front().text.find("Use the plugin review checklist"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsBackgroundAndIsolationDefinitionFeatures) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_background_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "async.md");
        agent << R"MD(---
name: async-reviewer
description: Reviews in background native modes
background: true
isolation: worktree
skills: [review]
---
Review asynchronously.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Async review",
          "prompt": "Review this change",
          "subagent_type": "async-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("features not yet supported"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("isolation"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolExecutesDefinitionHooksForBackgroundAgents) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_hooks_definition_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    auto marker = root / "hook-marker.txt";
    {
        std::ofstream agent(root / ".claude" / "agents" / "hooked.md");
        agent << R"MD(---
name: hooked-reviewer
description: Reviews with hooks
hooks:
  SubagentStart:
    - command: "printf start-$CLAUDE_HOOK_AGENT_ID > )MD" << marker.string() << R"MD(; echo hook-started"
---
Review with hooks.
)MD";
    }

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentTool tool;

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Hooked review",
          "prompt": "Review this change",
          "subagent_type": "hooked-reviewer",
          "run_in_background": true,
          "name": "hooked-agent"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Queued background agent hooked-agent"), std::string::npos);
        ASSERT_TRUE(fs::exists(marker));
        std::ifstream marker_in(marker);
        std::string marker_text;
        std::getline(marker_in, marker_text);
        EXPECT_EQ(marker_text, "start-hooked-agent");

        auto record = cc::tools::agent_runtime::native_agent_store().get("hooked-agent");
        ASSERT_TRUE(record.has_value());
        ASSERT_GE(record->transcript.size(), 1u);
        EXPECT_NE(record->transcript.back().find("hook-started"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolRejectsMissingRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_missing_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("requires MCP servers matching: linear"), std::string::npos);
        EXPECT_NE(result->content.front().text.find("MCP servers with tools: none"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("recursion depth"), std::string::npos);
    }

    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsAgentSpecificMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_mcp_servers_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'agent-mcp-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup agent context', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "mcp-agent.md");
        agent << R"MD(---
name: mcp-agent
description: Uses an agent-specific MCP server
tools: [Read]
mcpServers: [agent_fixture]
---
Use the agent-specific MCP server.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "agent_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Use MCP context.";
        request.subagent_type = "mcp-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->agent_mcp_servers.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_servers.front(), "agent_fixture");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 1u);
        EXPECT_EQ(plan->agent_mcp_tools.front().server_name, "agent_fixture");
        EXPECT_EQ(plan->agent_mcp_tools.front().tool_name, "lookup");
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("agent_fixture/lookup"), std::string::npos);
        ASSERT_EQ(plan->allowed_tools.size(), 1u);
        EXPECT_EQ(plan->allowed_tools.front(), "Read");
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolLoadsInlineAgentMcpServersWithoutDroppingReferencedServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_inline_mcp_servers_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: process.env.SERVER_NAME || 'agent-inline-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{
          name: process.env.TOOL_NAME || 'lookup',
          description: ['tool', process.env.SERVER_NAME, process.env.INLINE_TOKEN].filter(Boolean).join(':'),
          inputSchema: { type: 'object' }
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "inline-mcp-agent.md");
        agent << std::format(R"MD(---
name: inline-mcp-agent
description: Uses referenced and inline MCP servers
mcpServers:
  - existing_fixture
  - inline_fixture:
      type: stdio
      command: node
      args:
        - "{}"
      env:
        SERVER_NAME: inline_fixture
        TOOL_NAME: inline_lookup
        INLINE_TOKEN: secret-token
---
Use both MCP servers.
)MD", server_path.string());
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "existing_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {{"SERVER_NAME", "existing_fixture"}, {"TOOL_NAME", "existing_lookup"}},
        },
    });
    ASSERT_TRUE(synced.has_value());

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        cc::tools::agent::AgentToolRequest request;
        request.prompt = "Use MCP context.";
        request.subagent_type = "inline-mcp-agent";

        auto plan = cc::tools::agent::build_agent_execution_plan(request, config);

        ASSERT_TRUE(plan.has_value()) << plan.error();
        ASSERT_EQ(plan->agent_mcp_servers.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_servers[0], "existing_fixture");
        EXPECT_EQ(plan->agent_mcp_servers[1], "inline_fixture");
        ASSERT_EQ(plan->agent_mcp_tools.size(), 2u);
        EXPECT_EQ(plan->agent_mcp_tools[0].server_name, "existing_fixture");
        EXPECT_EQ(plan->agent_mcp_tools[0].tool_name, "existing_lookup");
        EXPECT_EQ(plan->agent_mcp_tools[1].server_name, "inline_fixture");
        EXPECT_EQ(plan->agent_mcp_tools[1].tool_name, "inline_lookup");
        EXPECT_NE(plan->agent_mcp_tools[1].description.find("secret-token"), std::string::npos);
        ASSERT_TRUE(plan->agent_mcp_context_message.has_value());
        EXPECT_NE(plan->agent_mcp_context_message->find("existing_fixture/existing_lookup"), std::string::npos);
        EXPECT_NE(plan->agent_mcp_context_message->find("inline_fixture/inline_lookup"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsReadyRequiredMcpServers) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_required_mcp_ready_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude" / "agents");
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'linear-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'lookup', description: 'Lookup issue', inputSchema: { type: 'object' } }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream agent(root / ".claude" / "agents" / "linear.md");
        agent << R"MD(---
name: linear-reviewer
description: Requires Linear MCP tools
requiredMcpServers: [linear]
---
Review Linear context.
)MD";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "linear_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());
    auto restarted = cc::tools::restart_native_mcp_server("linear_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    ASSERT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);

    {
        CurrentPathGuard cwd(root);
        cc::tools::AgentConfig config;
        config.max_depth = 0;
        cc::tools::AgentTool tool(config);

        auto result = tool.execute(cc::core::ToolInput::from_json(R"({
          "description": "Linear review",
          "prompt": "Review with Linear context",
          "subagent_type": "linear-reviewer"
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_NE(result->content.front().text.find("Agent recursion depth limit reached"), std::string::npos);
        EXPECT_EQ(result->content.front().text.find("requires MCP servers"), std::string::npos);
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, AgentToolAcceptsBackgroundNativeParameters) {
    cc::tools::AgentTool tool;

    auto result = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Run in the background",
      "name": "reviewer-one",
      "team_name": "migration-team",
      "mode": "default",
      "run_in_background": true,
      "isolation": "worktree"
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Queued background agent reviewer-one"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("reviewer-one");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->agent_type, "general-purpose");
    ASSERT_TRUE(record->team_name.has_value());
    EXPECT_EQ(*record->team_name, "migration-team");
    ASSERT_TRUE(record->isolation.has_value());
    EXPECT_EQ(*record->isolation, "worktree");
    EXPECT_EQ(record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
}

TEST(Tools, AgentRuntimeTracksLifecycleForkAndResume) {
    auto root = fs::temp_directory_path() / "cc_repl_agent_runtime_lifecycle_test";
    fs::remove_all(root);
    fs::create_directories(root);

    cc::tools::agent_runtime::AgentRuntimeConfig parent_config{
        .agent_id = "runtime-parent",
        .working_dir = root.string(),
        .capabilities = {"Read", "Bash"},
    };
    auto parent = cc::tools::agent_runtime::run_agent(parent_config);
    ASSERT_TRUE(parent.has_value()) << parent.error();
    EXPECT_EQ(parent->agent_id, "runtime-parent");
    EXPECT_EQ(parent->exit_code, 0);
    EXPECT_NE(parent->output.find(root.string()), std::string::npos);
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-parent"),
        cc::tools::agent_runtime::AgentLifecycle::Completed);

    cc::tools::agent_runtime::AgentRuntimeConfig child_config{
        .agent_id = "runtime-child",
        .working_dir = root.string(),
        .capabilities = {"Read"},
        .allow_fork = true,
    };
    auto child = cc::tools::agent_runtime::fork_subagent("runtime-parent", child_config);
    ASSERT_TRUE(child.has_value()) << child.error();
    EXPECT_EQ(*child, "runtime-child");

    auto child_record = cc::tools::agent_runtime::native_agent_store().get("runtime-child");
    ASSERT_TRUE(child_record.has_value());
    ASSERT_TRUE(child_record->parent_agent_id.has_value());
    EXPECT_EQ(*child_record->parent_agent_id, "runtime-parent");
    EXPECT_EQ(child_record->status, cc::tools::agent_runtime::NativeAgentStatus::Queued);
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-child"),
        cc::tools::agent_runtime::AgentLifecycle::Starting);

    auto resumed = cc::tools::agent_runtime::resume_agent("runtime-child");
    ASSERT_TRUE(resumed.has_value()) << resumed.error();
    EXPECT_EQ(resumed->agent_id, "runtime-child");
    EXPECT_NE(resumed->output.find("queued"), std::string::npos);

    ASSERT_FALSE(parent->transcript.empty());
    auto parent_record = cc::tools::agent_runtime::native_agent_store().get("runtime-parent");
    ASSERT_TRUE(parent_record.has_value());
    ASSERT_TRUE(parent_record->progress.has_value());
    EXPECT_DOUBLE_EQ(*parent_record->progress, 1.0);

    cc::tools::agent_runtime::native_agent_store().request_cancel("runtime-child", "test cancel");
    EXPECT_EQ(
        cc::tools::agent_runtime::get_agent_lifecycle("runtime-child"),
        cc::tools::agent_runtime::AgentLifecycle::Cancelled);
    auto cancelled = cc::tools::agent_runtime::resume_agent("runtime-child");
    ASSERT_TRUE(cancelled.has_value()) << cancelled.error();
    EXPECT_EQ(cancelled->exit_code, 130);
    ASSERT_TRUE(cancelled->error.has_value());
    EXPECT_EQ(*cancelled->error, "test cancel");

    child_config.allow_fork = false;
    auto denied = cc::tools::agent_runtime::fork_subagent("runtime-parent", child_config);
    EXPECT_FALSE(denied.has_value());

    auto missing = cc::tools::agent_runtime::resume_agent("missing-agent");
    EXPECT_FALSE(missing.has_value());

    fs::remove_all(root);
}

TEST(Tools, RuntimeSendMessageDeliversToBackgroundAgentQueue) {
    cc::tools::AgentTool tool;
    auto started = tool.execute(cc::core::ToolInput::from_json(R"({
      "description": "Run async",
      "prompt": "Wait for coordination",
      "name": "message-target",
      "run_in_background": true
    })"));
    ASSERT_TRUE(started.has_value());
    ASSERT_FALSE(started->is_error);

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "message-target",
      "content": "Review the migration diff",
      "priority": "high"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    ASSERT_FALSE(delivered->content.empty());
    EXPECT_NE(delivered->content.front().text.find("Delivered message"), std::string::npos);
    EXPECT_NE(delivered->content.front().text.find("message-target"), std::string::npos);
}

TEST(Tools, RuntimeTeamCreateRegistersMembersAndSharedTasks) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto created = registry.execute("team_create", cc::core::ToolInput::from_json(R"({
      "team_id": "runtime-team-members",
      "team_name": "Runtime Team Members",
      "members": [
        {"agent_id": "team-researcher", "role": "worker"},
        {"agent_id": "team-reviewer", "role": "reviewer"}
      ],
      "task_list": [
        {"id": "task-1", "description": "Inspect migration parity", "assigned_to": "team-researcher"}
      ]
    })"));

    ASSERT_TRUE(created.has_value());
    ASSERT_FALSE(created->is_error);
    ASSERT_FALSE(created->content.empty());
    EXPECT_NE(created->content.front().text.find("with 2 members and 1 tasks"), std::string::npos);

    auto record = cc::tools::agent_runtime::native_agent_store().get("team-reviewer");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(record->team_name.has_value());
    EXPECT_EQ(*record->team_name, "Runtime Team Members");

    auto delivered = registry.execute("send_message", cc::core::ToolInput::from_json(R"({
      "target_agent": "team-reviewer",
      "content": "Review team output"
    })"));

    ASSERT_TRUE(delivered.has_value());
    ASSERT_FALSE(delivered->is_error);
    EXPECT_NE(delivered->content.front().text.find("team-reviewer"), std::string::npos);
}

TEST(Tools, RuntimeWorkflowExecutesJsonDefinition) {
    auto root = fs::temp_directory_path() / "cc_repl_runtime_workflow_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto workflow_path = root / "workflow.json";
    {
        std::ofstream workflow(workflow_path);
        workflow << R"JSON({
  "name": "migration-check",
  "variables": {
    "greeting": "hello",
    "run_extra": "false"
  },
  "steps": [
    {"id": "assign", "type": "assign", "action": "target=world"},
    {"id": "log", "type": "log", "action": "${greeting}-${target}"},
    {"id": "condition", "type": "condition", "action": "true"},
    {"id": "command", "type": "command", "command": "printf cmd-${target}"},
    {"id": "repeat", "type": "loop", "command": "printf ${repeat.index}", "maxIterations": 3},
    {"id": "skipped", "type": "log", "action": "should-not-run", "condition": "${run_extra}"}
  ]
})JSON";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    cc::utils::json::JsonMutDoc doc;
    auto input = doc.object();
    input.add("file", doc.string(workflow_path.string()));
    doc.set_root(input);

    auto result = registry.execute("workflow", cc::core::ToolInput::from_json(doc.to_string()));

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_error) << result->content.front().text;
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("Workflow migration-check completed"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Steps executed: 5"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("Steps skipped: 1"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("hello-world"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("cmd-world"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("012"), std::string::npos);

    fs::remove_all(root);
}

TEST(Tools, TodoWriteParsesTypeScriptInputShape) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto result = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Inspect migration gaps","status":"in_progress","activeForm":"Inspecting migration gaps"},
        {"content":"Run native validation","status":"pending","activeForm":"Running native validation"}
      ]
    })"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_NE(result->content.front().text.find("2 total, 2 added"), std::string::npos);
}

TEST(Tools, TodoWriteClearsAllDoneReplacementLists) {
    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);

    auto initial = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"in_progress","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"pending","activeForm":"Verifying parser"}
      ]
    })"));
    ASSERT_TRUE(initial.has_value());
    ASSERT_FALSE(initial->is_error);

    auto completed = registry.execute("todo_write", cc::core::ToolInput::from_json(R"({
      "todos": [
        {"content":"Implement parser","status":"completed","activeForm":"Implementing parser"},
        {"content":"Verify parser","status":"completed","activeForm":"Verifying parser"}
      ]
    })"));

    ASSERT_TRUE(completed.has_value());
    ASSERT_FALSE(completed->is_error);
    ASSERT_FALSE(completed->content.empty());
    EXPECT_NE(completed->content.front().text.find("0 total"), std::string::npos);
}

TEST(Tools, GlobFiltersByPattern) {
    auto root = fs::temp_directory_path() / "cc_repl_glob_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "int main() {}\n";
        std::ofstream(root / "src" / "skip.txt") << "not source\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Glob", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"**/*.cpp","path":"{}"}})", root.string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("skip.txt"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, GrepUsesPathAndRegex) {
    auto root = fs::temp_directory_path() / "cc_repl_grep_test";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    {
        std::ofstream(root / "src" / "match.cpp") << "alpha_123\nbeta\n";
        std::ofstream(root / "src" / "skip.cpp") << "gamma\n";
    }

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("Grep", cc::core::ToolInput::from_json(
        std::format(R"({{"pattern":"alpha_[0-9]+","path":"{}"}})", (root / "src").string())));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    EXPECT_NE(result->content.front().text.find("match.cpp"), std::string::npos);
    EXPECT_NE(result->content.front().text.find("alpha_123"), std::string::npos);
    EXPECT_EQ(result->content.front().text.find("gamma"), std::string::npos);
    fs::remove_all(root);
}

TEST(Tools, McpToolCallsNativeStdioServer) {
    auto root = fs::temp_directory_path() / "cc_repl_mcp_stdio_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto server_path = root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {}, resources: {} },
        serverInfo: { name: 'fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'echo', description: 'Echo value', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{ type: 'text', text: 'echo:' + request.params.arguments.value }]
      }
    });
    return;
  }
  if (request.method === 'resources/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { resources: [{ uri: 'fixture://one', name: 'one', mimeType: 'text/plain' }] }
    });
    return;
  }
  if (request.method === 'resources/read') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: { contents: [{ uri: request.params.uri, mimeType: 'text/plain', text: 'resource-body' }] }
    });
  }
});
)JS";
    }

    auto synced = cc::tools::sync_native_mcp_servers({
        cc::tools::NativeMcpConfiguredServer{
            .name = "echo_fixture",
            .command = "node",
            .args = {server_path.string()},
            .env = {},
        },
    });
    ASSERT_TRUE(synced.has_value());

    auto restarted = cc::tools::restart_native_mcp_server("echo_fixture");
    ASSERT_TRUE(restarted.has_value()) << restarted.error();
    EXPECT_EQ(restarted->status, "ready");
    ASSERT_EQ(restarted->tools.size(), 1u);
    EXPECT_EQ(restarted->tools.front().name, "echo");

    cc::core::ToolRegistry registry;
    cc::tools::register_runtime_tools(registry);
    auto result = registry.execute("mcp", cc::core::ToolInput::from_json(
        R"({"server_name":"echo_fixture","tool_name":"echo","arguments":{"value":"hello"}})"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->is_error);
    ASSERT_FALSE(result->content.empty());
    EXPECT_EQ(result->content.front().text, "echo:hello");

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}

TEST(Tools, McpRuntimeLoadsPluginManifestMcpServers) {
    auto root = fs::weakly_canonical(fs::temp_directory_path()) / "cc_repl_plugin_mcp_test";
    fs::remove_all(root);
    fs::create_directories(root / ".claude");
    EnvironmentGuard home_guard("HOME", root.string());
    EnvironmentGuard plugin_cache_guard(
        "CLAUDE_CODE_PLUGIN_CACHE_DIR",
        (root / ".claude" / "plugins").string()
    );

    const auto plugin_root = root / ".claude" / "plugins" / "mcp-fixture";
    fs::create_directories(plugin_root);
    const auto server_path = plugin_root / "server.js";
    {
        std::ofstream server(server_path);
        server << R"JS(
const readline = require('node:readline');
const rl = readline.createInterface({ input: process.stdin });

function send(message) {
  process.stdout.write(JSON.stringify(message) + '\n');
}

rl.on('line', line => {
  const request = JSON.parse(line);
  if (request.method === 'initialize') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: { tools: {} },
        serverInfo: { name: 'plugin-mcp-fixture', version: '1.0.0' }
      }
    });
    return;
  }
  if (request.method === 'tools/list') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        tools: [{ name: 'plugin_echo', description: 'Echo from plugin MCP', inputSchema: { type: 'object' } }]
      }
    });
    return;
  }
  if (request.method === 'tools/call') {
    send({
      jsonrpc: '2.0',
      id: request.id,
      result: {
        isError: false,
        content: [{
          type: 'text',
          text: [
            'plugin',
            request.params.arguments.value,
            process.env.PLUGIN_MCP_FIXTURE,
            process.env.PLUGIN_MCP_TOKEN,
            process.env.CLAUDE_PLUGIN_ROOT
          ].join(':')
        }]
      }
    });
  }
});
)JS";
    }
    {
        std::ofstream settings(root / ".claude" / "settings.json");
        settings << R"JSON({
  "pluginConfigs": {
    "mcp-fixture": {
      "options": {
        "suffix": "top-level",
        "token": "shared-token"
      },
      "mcpServers": {
        "echo": {
          "suffix": "configured",
          "token": "secret-token"
        }
      }
    }
  }
})JSON";
    }
    {
        std::ofstream defaults(plugin_root / ".mcp.json");
        defaults << R"JSON({
  "mcpServers": {
    "echo": {
      "command": "missing-plugin-mcp-command"
    }
  }
})JSON";
    }
    {
        std::ofstream manifest(plugin_root / "plugin.json");
        manifest << std::format(R"JSON({{
  "name": "mcp-fixture",
  "version": "1.0.0",
  "entry_point": "plugin.js",
  "mcpServers": {{
    "echo": {{
      "type": "stdio",
      "command": "node",
      "args": ["${{CLAUDE_PLUGIN_ROOT}}/server.js"],
      "env": {{
        "PLUGIN_MCP_FIXTURE": "${{user_config.suffix}}",
        "PLUGIN_MCP_TOKEN": "${{user_config.token}}"
      }}
    }}
  }}
}})JSON");
    }
    {
        std::ofstream entry(plugin_root / "plugin.js");
        entry << "process.exit(0)\n";
    }

    {
        CurrentPathGuard cwd(root);
        auto servers = cc::tools::discover_plugin_native_mcp_servers();
        auto it = std::ranges::find_if(servers, [](const auto& server) {
            return server.name == "plugin:mcp-fixture:echo";
        });
        ASSERT_NE(it, servers.end());
        EXPECT_EQ(it->command, "node");
        ASSERT_EQ(it->args.size(), 1u);
        EXPECT_EQ(it->args.front(), server_path.string());
        EXPECT_EQ(it->env.at("PLUGIN_MCP_FIXTURE"), "configured");
        EXPECT_EQ(it->env.at("PLUGIN_MCP_TOKEN"), "secret-token");
        EXPECT_EQ(it->env.at("CLAUDE_PLUGIN_ROOT"), plugin_root.string());
        EXPECT_EQ(
            it->env.at("CLAUDE_PLUGIN_DATA"),
            (root / ".claude" / "plugins" / "data" / "mcp-fixture").string()
        );

        auto synced = cc::tools::sync_native_mcp_servers(std::move(servers));
        ASSERT_TRUE(synced.has_value()) << synced.error();

        auto restarted = cc::tools::restart_native_mcp_server("plugin:mcp-fixture:echo");
        ASSERT_TRUE(restarted.has_value()) << restarted.error();
        EXPECT_EQ(restarted->status, "ready");
        ASSERT_EQ(restarted->tools.size(), 1u);
        EXPECT_EQ(restarted->tools.front().name, "plugin_echo");

        cc::core::ToolRegistry registry;
        cc::tools::register_runtime_tools(registry);
        auto result = registry.execute("mcp", cc::core::ToolInput::from_json(R"({
          "server_name": "plugin:mcp-fixture:echo",
          "tool_name": "plugin_echo",
          "arguments": {"value": "hello"}
        })"));

        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->is_error);
        ASSERT_FALSE(result->content.empty());
        EXPECT_EQ(
            result->content.front().text,
            "plugin:hello:configured:secret-token:" + plugin_root.string()
        );
    }

    ASSERT_TRUE(cc::tools::sync_native_mcp_servers({}).has_value());
    fs::remove_all(root);
}
