/// @file claude_in_chrome.cppm
/// @brief Claude in Chrome skill - Chrome extension integration and browser automation.
/// Detects Chrome-extension related user queries and injects install/debug workflow prompts.
/// Mirrors src/skills/bundled/claudeInChrome.ts + src/utils/claudeInChrome/*.
module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <sstream>
#include <format>
#include <filesystem>

export module cc.skills.bundled.claude_in_chrome;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Chrome Web Store installation URL for the Claude in Chrome extension
constexpr const char* CHROME_WEB_STORE_URL =
    "https://chromewebstore.google.com/detail/claude-code/fcoeoabgfenejglbffodgkkbkcdhcgfn";

/// Reconnect URL opened after native-host manifest installation
constexpr const char* CHROME_EXTENSION_RECONNECT_URL =
    "https://clau.de/chrome/reconnect";

/// Native host identifier used in Chrome manifest files
constexpr const char* NATIVE_HOST_IDENTIFIER =
    "com.anthropic.claude_code_browser_extension";

/// Base Chrome system prompt (kept in sync with src/utils/claudeInChrome/prompt.ts)
/// The full prompt is injected when the skill is triggered.
inline const char* BASE_CHROME_PROMPT = R"(# Claude in Chrome browser automation

You have access to browser automation tools (mcp__claude-in-chrome__*) for interacting
with web pages in Chrome. Follow these guidelines for effective browser automation.

## GIF recording

When performing multi-step browser interactions that the user may want to review or
share, use mcp__claude-in-chrome__gif_creator to record them.

You must ALWAYS:
* Capture extra frames before and after taking actions to ensure smooth playback
* Name the file meaningfully to help the user identify it later (e.g., "login_process.gif")

## Console log debugging

You can use mcp__claude-in-chrome__read_console_messages to read console output. Console
output may be verbose. If you are looking for specific log entries, use the 'pattern'
parameter with a regex-compatible pattern. This filters results efficiently and avoids
overwhelming output. For example, use pattern: "[MyApp]" to filter for application-specific
logs rather than reading all console output.

## Alerts and dialogs

IMPORTANT: Do not trigger JavaScript alerts, confirms, prompts, or browser modal dialogs
through your actions. These browser dialogs block all further browser events and will
prevent the extension from receiving any subsequent commands. Instead, when possible, use
console.log for debugging and then use the mcp__claude-in-chrome__read_console_messages
tool to read those log messages. If a page has dialog-triggering elements:
1. Avoid clicking buttons or links that may trigger alerts (e.g., "Delete" buttons with
   confirmation dialogs)
2. If you must interact with such elements, warn the user first that this may interrupt
   the session
3. Use mcp__claude-in-chrome__javascript_tool to check for and dismiss any existing
   dialogs before proceeding

If you accidentally trigger a dialog and lose responsiveness, inform the user they need
to manually dismiss it in the browser.

## Avoid rabbit holes and loops

When using browser automation tools, stay focused on the specific task. If you encounter
any of the following, stop and ask the user for guidance:
- Unexpected complexity or tangential browser exploration
- Browser tool calls failing or returning errors after 2-3 attempts
- No response from the browser extension
- Page elements not responding to clicks or input
- Pages not loading or timing out
- Unable to complete the browser task despite multiple approaches

Explain what you attempted, what went wrong, and ask how the user would like to proceed.
Do not keep retrying the same failing browser action or explore unrelated pages without
checking in first.

## Tab context and session startup

IMPORTANT: At the start of each browser automation session, call
mcp__claude-in-chrome__tabs_context_mcp first to get information about the user's current
browser tabs. Use this context to understand what the user might want to work with before
creating new tabs.

Never reuse tab IDs from a previous/other session. Follow these guidelines:
1. Only reuse an existing tab if the user explicitly asks to work with it
2. Otherwise, create a new tab with mcp__claude-in-chrome__tabs_create_mcp
3. If a tool returns an error indicating the tab doesn't exist or is invalid, call
   tabs_context_mcp to get fresh tab IDs
4. When a tab is closed by the user or a navigation error occurs, call tabs_context_mcp
   to see what tabs are available)";

/// Activation message appended when the skill is invoked
inline const char* SKILL_ACTIVATION_MESSAGE = R"(
Now that this skill is invoked, you have access to Chrome browser automation tools. You
can now use the mcp__claude-in-chrome__* tools to interact with web pages.

IMPORTANT: Start by calling mcp__claude-in-chrome__tabs_context_mcp to get information
about the user's current browser tabs.)";

// ---------------------------------------------------------------------------
// Platform detection helpers (stub implementations)
// ---------------------------------------------------------------------------

namespace detail {

/// Execute a command and capture stdout (shared with other bundled skills)
inline std::expected<std::string, std::string> exec_command(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return std::unexpected("Failed to execute: " + cmd);

    std::string output;
    std::array<char, 4096> buffer{};
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        output.append(buffer.data(), bytes);
    }

    int status = ::pclose(pipe);
    if (status != 0 && output.empty()) {
        return std::unexpected("Command failed with exit code " +
            std::to_string(status) + ": " + cmd);
    }
    return output;
}

/// Check if environment variable is truthy (non-empty, not "0"/"false"/"no")
inline bool is_env_truthy(const char* var) {
    const char* val = std::getenv(var);
    if (!val) return false;
    std::string s(val);
    if (s.empty()) return false;
    if (s == "0" || s == "false" || s == "no" || s == "FALSE" || s == "NO") return false;
    return true;
}

/// Check if environment variable is explicitly defined and falsy
inline bool is_env_defined_falsy(const char* var) {
    const char* val = std::getenv(var);
    if (!val) return false;
    std::string s(val);
    return (s == "0" || s == "false" || s == "no" || s == "FALSE" || s == "NO");
}

/// Detect current platform
inline std::string detect_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

} // namespace detail

// ---------------------------------------------------------------------------
// Auto-enable detection (mirrors setup.ts:shouldAutoEnableClaudeInChrome)
// ---------------------------------------------------------------------------

/// Determine whether the Claude in Chrome skill should be auto-enabled.
/// Mirrors TS: getIsInteractive() && isChromeExtensionInstalled() && (ant user || feature flag)
/// C++ implementation is conservative: only enables when explicit env/config signals are
/// present; the full cached-extension-installation scan is deferred to runtime.
inline bool should_auto_enable_claude_in_chrome() {
    // Explicit environment override (highest priority)
    if (detail::is_env_truthy("CLAUDE_CODE_ENABLE_CFC")) return true;
    if (detail::is_env_defined_falsy("CLAUDE_CODE_ENABLE_CFC")) return false;

    // Cached positive detection stored in ~/.claude.json — read via lightweight file check
    // TODO(perf): Integrate with global config reader when available.
    // For now, treat the existence of the Chrome native-host manifest as a proxy.
    namespace fs = std::filesystem;
    if (const char* home = std::getenv("HOME")) {
        fs::path manifest_path = fs::path(home) /
            "Library/Application Support/Google/Chrome/NativeMessagingHosts" /
            std::string(NATIVE_HOST_IDENTIFIER) + ".json";
        if (fs::exists(manifest_path)) return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Browser launching helpers
// ---------------------------------------------------------------------------

/// Open a URL in the user's default Chrome browser (platform-aware).
/// TODO(ui): On macOS use `open -a "Google Chrome"`, on Linux `xdg-open`,
/// on Windows `start chrome`. Fallback to system-default handler.
inline void open_in_chrome(std::string_view url) {
    std::string url_str(url);
    std::string cmd;
    const auto platform = detail::detect_platform();
    if (platform == "macos") {
        // Prefer the "open" command on macOS; user's Chrome becomes default if installed.
        cmd = std::format("open '{}' > /dev/null 2>&1 &", url_str);
    } else if (platform == "windows") {
        cmd = std::format("start chrome \"{}\"", url_str);
    } else {
        cmd = std::format("xdg-open '{}' > /dev/null 2>&1 &", url_str);
    }
    // Fire-and-forget; discard any errors.
    std::system(cmd.c_str());
}

/// Open the Chrome Web Store page for the Claude in Chrome extension.
/// Used when the user indicates the extension is missing or not installed.
inline void open_chrome_web_store_install() {
    open_in_chrome(CHROME_WEB_STORE_URL);
}

/// Open the reconnect page after native-host manifest (re)installation.
inline void open_chrome_reconnect() {
    open_in_chrome(CHROME_EXTENSION_RECONNECT_URL);
}

// ---------------------------------------------------------------------------
// Diagnostic / install checklist
// ---------------------------------------------------------------------------

/// Build the human-readable troubleshooting checklist for when the extension
/// is installed but not responding. Mirrors the debugging flow described in
/// the TS skill description.
inline std::string build_troubleshooting_checklist() {
    std::ostringstream oss;
    oss << "## Claude in Chrome — Troubleshooting Checklist\n\n"
        << "If browser tools are not responding, work through these steps:\n\n"
        << "### 1. Verify extension is installed\n"
        << "- Open `chrome://extensions` in Chrome\n"
        << "- Look for **Claude Code** extension (id: fcoeoabgfenejglbffodgkkbkcdhcgfn)\n"
        << "- Ensure the toggle is ON\n"
        << "- If missing: install from " << CHROME_WEB_STORE_URL << "\n\n"
        << "### 2. Grant site permissions\n"
        << "- Click the extension icon > Manage Extension > Site access\n"
        << "- Select **On all sites** or add specific domains you want to automate\n"
        << "- Without site permissions, the extension cannot read/interact with pages\n\n"
        << "### 3. Verify native host connectivity\n"
        << "- Reload the extension on `chrome://extensions` (click the round arrow)\n"
        << "- Then run: `" << CHROME_EXTENSION_RECONNECT_URL << "`\n"
        << "- Inspect the extension's service worker console for errors\n\n"
        << "### 4. Restart the browser\n"
        << "- Completely quit Chrome (Cmd+Q on macOS) and reopen\n"
        << "- The native messaging pipe resets only on full restart\n\n"
        << "### 5. Check terminal environment\n"
        << "- Confirm `CLAUDE_CODE_ENABLE_CFC` is not set to `0`\n"
        << "- Confirm you are running in an **interactive** session (not CI/SDK)\n\n";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Prompt construction
// ---------------------------------------------------------------------------

/// Build the complete prompt text injected when the claude-in-chrome skill fires.
/// If `task_args` is provided, appends a "Task" section with the user's request.
inline std::string build_claude_in_chrome_prompt(
    std::optional<std::string_view> task_args = std::nullopt) {

    std::ostringstream oss;
    oss << BASE_CHROME_PROMPT << "\n" << SKILL_ACTIVATION_MESSAGE;
    if (task_args && !task_args->empty()) {
        oss << "\n\n## Task\n\n" << *task_args;
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Manifest (for SkillLoader discovery)
// ---------------------------------------------------------------------------

/// Get the skill manifest for the Claude in Chrome skill.
/// The `triggers` list contains keyword patterns (English + Chinese) that should
/// cause the skill to be injected.  Additional regex triggers are defined in
/// bundled.cppm:make_claude_in_chrome_skill().
cc::skills::SkillManifest get_claude_in_chrome_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "claude-in-chrome",
        .description =
            "Automates your Chrome browser to interact with web pages - clicking elements, "
            "filling forms, capturing screenshots, reading console logs, and navigating sites. "
            "Opens pages in new tabs within your existing Chrome session. Requires site-level "
            "permissions before executing (configured in the extension).",
        .version = "1.0.0",
        .triggers = {
            "chrome extension",
            "chrome 插件",
            "chrome 扩展",
            "侧边栏",
            "Claude in Chrome",
            "CFC",
            "browser automation",
            "browser tool",
            "chrome tab",
            "网页操作",
            "浏览器操作",
        },
        .directory = {}
    };
}

} // namespace cc::skills::bundled
