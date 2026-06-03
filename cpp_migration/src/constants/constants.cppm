// C++23 Module: Global constants

module;
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

export module cc.constants.constants;

export namespace cc::core::constants {

// ============================================================

// ============================================================
inline constexpr std::string_view kVersion = "1.0.0-cpp";
inline constexpr std::string_view kBuildDate = __DATE__;
inline constexpr std::string_view kAppName = "cc-repl";
inline constexpr std::string_view kUserAgent = "cc-repl/1.0.0-cpp";

// ============================================================

// ============================================================
namespace api_limits {
    inline constexpr size_t kMaxTokensClaude3Opus = 200000;
    inline constexpr size_t kMaxTokensClaude3Sonnet = 200000;
    inline constexpr size_t kMaxTokensClaude35Sonnet = 200000;
    inline constexpr size_t kMaxTokensClaude4Sonnet = 200000;
    inline constexpr size_t kMaxTokensDefault = 128000;
    inline constexpr size_t kMaxOutputTokens = 8192;
    inline constexpr size_t kMaxOutputTokensExtended = 64000;


    inline constexpr uint32_t kRateLimitRequestsPerMinute = 60;
    inline constexpr uint32_t kRateLimitTokensPerMinute = 100000;
    inline constexpr uint32_t kRateLimitRetryAfterMs = 1000;
    inline constexpr uint32_t kRateLimitMaxRetries = 3;
}

// ============================================================

// ============================================================
enum class ErrorCode : uint16_t {

    Ok = 0,
    Unknown = 100,
    InvalidArgument = 101,
    NotFound = 102,
    PermissionDenied = 103,
    AlreadyExists = 104,
    ResourceExhausted = 105,
    Cancelled = 106,
    Timeout = 107,


    NetworkError = 200,
    ConnectionRefused = 201,
    ConnectionTimeout = 202,
    DnsResolutionFailed = 203,
    TlsError = 204,
    HttpError = 205,


    ApiError = 300,
    RateLimited = 301,
    ContextTooLong = 302,
    InvalidApiKey = 303,
    ModelNotFound = 304,
    ContentFiltered = 305,
    ServerOverloaded = 306,


    ToolError = 400,
    ToolNotFound = 401,
    ToolTimeout = 402,
    ToolOutputTooLarge = 403,
    ToolPermissionDenied = 404,
    ToolExecutionFailed = 405,


    FileError = 500,
    FileNotFound = 501,
    FileReadError = 502,
    FileWriteError = 503,
    FileTooLarge = 504,
    DirectoryNotFound = 505,


    SessionError = 600,
    SessionExpired = 601,
    SessionNotFound = 602,
    SessionCorrupted = 603,


    ConfigError = 700,
    ConfigNotFound = 701,
    ConfigParseError = 702,
    ConfigValidationError = 703,
};


[[nodiscard]] inline std::string_view error_description(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Success";
        case ErrorCode::Unknown: return "Unknown error";
        case ErrorCode::InvalidArgument: return "Invalid argument";
        case ErrorCode::NotFound: return "Resource not found";
        case ErrorCode::PermissionDenied: return "Permission denied";
        case ErrorCode::Timeout: return "Operation timed out";
        case ErrorCode::RateLimited: return "Rate limit exceeded";
        case ErrorCode::ContextTooLong: return "Context window exceeded";
        case ErrorCode::InvalidApiKey: return "Invalid API key";
        case ErrorCode::ToolOutputTooLarge: return "Tool output exceeds size limit";
        case ErrorCode::FileNotFound: return "File not found";
        case ErrorCode::FileTooLarge: return "File exceeds size limit";
        case ErrorCode::SessionExpired: return "Session has expired";
        case ErrorCode::ConfigParseError: return "Configuration parse error";
        default: return "Unspecified error";
    }
}

// ============================================================

// ============================================================
namespace tool_limits {
    inline constexpr size_t kMaxOutputSize = 1024 * 1024;        // 1MB
    inline constexpr size_t kMaxFileSize = 10 * 1024 * 1024;     // 10MB
    inline constexpr size_t kMaxFileReadSize = 512 * 1024;       // 512KB per read
    inline constexpr size_t kMaxSearchResults = 100;
    inline constexpr size_t kMaxGlobResults = 1000;
    inline constexpr uint32_t kMaxCommandDurationMs = 120000;    // 2 minutes
    inline constexpr size_t kMaxConcurrentTools = 8;
}

// ============================================================

// ============================================================
namespace oauth {
    inline constexpr std::string_view kClientId = "cc-repl-cli";
    inline constexpr std::string_view kRedirectUri = "http://localhost:9876/callback";
    inline constexpr std::string_view kAuthEndpoint = "https://api.anthropic.com/oauth/authorize";
    inline constexpr std::string_view kTokenEndpoint = "https://api.anthropic.com/oauth/token";
    inline constexpr std::string_view kScopes = "read write tools";
    inline constexpr uint16_t kCallbackPort = 9876;
}

// ============================================================

// ============================================================
namespace paths {
    inline constexpr std::string_view kConfigDir = ".claude";
    inline constexpr std::string_view kConfigFile = "config.json";
    inline constexpr std::string_view kSessionsDir = "sessions";
    inline constexpr std::string_view kPluginsDir = "plugins";
    inline constexpr std::string_view kCacheDir = "cache";
    inline constexpr std::string_view kLogsDir = "logs";
    inline constexpr std::string_view kCredentialsFile = "credentials.json";
    inline constexpr std::string_view kPermissionsFile = "permissions.json";
    inline constexpr std::string_view kHistoryFile = "history";


    [[nodiscard]] inline std::filesystem::path config_home() {
        if (auto* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / kConfigDir;
        }
        return std::filesystem::path(kConfigDir);
    }
}

// ============================================================

// ============================================================
namespace timeouts {
    inline constexpr uint32_t kApiRequestMs = 60000;
    inline constexpr uint32_t kApiStreamFirstByteMs = 30000;
    inline constexpr uint32_t kToolExecutionMs = 120000;
    inline constexpr uint32_t kSessionIdleMs = 3600000;
    inline constexpr uint32_t kReconnectMs = 5000;
    inline constexpr uint32_t kHealthCheckMs = 30000;
    inline constexpr uint32_t kShutdownGraceMs = 5000;
    inline constexpr uint32_t kFileWatchDebounceMs = 100;
}

// ============================================================

// ============================================================
namespace models {
    inline constexpr std::string_view kClaude3Opus = "claude-3-opus-20240229";
    inline constexpr std::string_view kClaude3Sonnet = "claude-3-sonnet-20240229";
    inline constexpr std::string_view kClaude35Sonnet = "claude-3-5-sonnet-20241022";
    inline constexpr std::string_view kClaude4Sonnet = "claude-sonnet-4-20250514";
    inline constexpr std::string_view kClaude45Haiku = "claude-haiku-4-5-20251001";
    inline constexpr std::string_view kClaude45Opus = "claude-opus-4-5";
    inline constexpr std::string_view kClaude46Opus = "claude-opus-4-6";
    inline constexpr std::string_view kClaude46Sonnet = "claude-sonnet-4-6";
    inline constexpr std::string_view kDefault = "claude-sonnet-4-20250514";
    inline constexpr std::string_view kFrontierModelName = "Claude Opus 4.6";
}

// ============================================================

// ============================================================
namespace figures {
    inline constexpr std::string_view kBlackCircle = "●";
    inline constexpr std::string_view kBulletOperator = "∙";
    inline constexpr std::string_view kTeardropAsterisk = "✻";
    inline constexpr std::string_view kUpArrow = "↑";
    inline constexpr std::string_view kDownArrow = "↓";
    inline constexpr std::string_view kLightningBolt = "↯";
    inline constexpr std::string_view kEffortLow = "○";
    inline constexpr std::string_view kEffortMedium = "◐";
    inline constexpr std::string_view kEffortHigh = "●";
    inline constexpr std::string_view kEffortMax = "◉";
    inline constexpr std::string_view kPlayIcon = "▶";
    inline constexpr std::string_view kPauseIcon = "⏸";
    inline constexpr std::string_view kRefreshArrow = "↻";
    inline constexpr std::string_view kChannelArrow = "←";
    inline constexpr std::string_view kInjectedArrow = "→";
    inline constexpr std::string_view kForkGlyph = "⑂";
    inline constexpr std::string_view kDiamondOpen = "◇";
    inline constexpr std::string_view kDiamondFilled = "◆";
    inline constexpr std::string_view kReferenceMark = "※";
    inline constexpr std::string_view kFlagIcon = "⚑";
    inline constexpr std::string_view kBlockquoteBar = "▎";
    inline constexpr std::string_view kHeavyHorizontal = "━";
    inline constexpr std::string_view kBridgeReadyIndicator = "·✓·";
    inline constexpr std::string_view kBridgeFailedIndicator = "×";
}

// ============================================================

// ============================================================
namespace binary_extensions {
    inline constexpr std::array<std::string_view, 100> kExtensions = {

        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".tiff", ".tif",

        ".mp4", ".mov", ".avi", ".mkv", ".webm", ".wmv", ".flv", ".m4v", ".mpeg", ".mpg",

        ".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a", ".wma", ".aiff", ".opus",

        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar", ".xz", ".z", ".tgz", ".iso",

        ".exe", ".dll", ".so", ".dylib", ".bin", ".o", ".a", ".obj", ".lib", ".app", ".msi", ".deb", ".rpm",

        ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",

        ".ttf", ".otf", ".woff", ".woff2", ".eot",

        ".pyc", ".pyo", ".class", ".jar", ".war", ".ear", ".node", ".wasm", ".rlib",

        ".sqlite", ".sqlite3", ".db", ".mdb", ".idx",

        ".psd", ".ai", ".eps", ".sketch", ".fig", ".xd", ".blend", ".3ds", ".max",
        // Flash
        ".swf", ".fla",

        ".lockb", ".dat", ".data"
    };

    [[nodiscard]] inline bool has_binary_extension(std::string_view path) {
        auto dot_pos = path.rfind('.');
        if (dot_pos == std::string_view::npos) return false;
        auto ext = path.substr(dot_pos);

        std::string lower_ext;
        lower_ext.reserve(ext.size());
        for (char c : ext) {
            lower_ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (auto e : kExtensions) {
            if (lower_ext == e) return true;
        }
        return false;
    }
}

// ============================================================

// ============================================================
namespace messages {
    inline constexpr std::string_view kNoContent = "(no content)";
}

// ============================================================

// ============================================================
namespace github_app {
    inline constexpr std::string_view kPrTitle = "Add Claude Code GitHub Workflow";
    inline constexpr std::string_view kGitHubActionSetupDocsUrl = 
        "https://github.com/anthropics/claude-code-action/blob/main/docs/setup.md";
    
    inline constexpr std::string_view kWorkflowContent = R"(name: Claude Code

on:
  issue_comment:
    types: [created]
  pull_request_review_comment:
    types: [created]
  issues:
    types: [opened, assigned]
  pull_request_review:
    types: [submitted]

jobs:
  claude:
    if: |
      (github.event_name == 'issue_comment' && contains(github.event.comment.body, '@claude')) ||
      (github.event_name == 'pull_request_review_comment' && contains(github.event.comment.body, '@claude')) ||
      (github.event_name == 'pull_request_review' && contains(github.event.review.body, '@claude')) ||
      (github.event_name == 'issues' && (contains(github.event.issue.body, '@claude') || contains(github.event.issue.title, '@claude')))
    runs-on: ubuntu-latest
    permissions:
      contents: read
      pull-requests: read
      issues: read
      id-token: write
      actions: read
    steps:
      - name: Checkout repository
        uses: actions/checkout@v4
        with:
          fetch-depth: 1

      - name: Run Claude Code
        id: claude
        uses: anthropics/claude-code-action@v1
        with:
          anthropic_api_key: ${{ secrets.ANTHROPIC_API_KEY }}
)";

    inline constexpr std::string_view kPrBody = R"(## 🤖 Installing Claude Code GitHub App

This PR adds a GitHub Actions workflow that enables Claude Code integration in our repository.

### What is Claude Code?

[Claude Code](https://claude.com/claude-code) is an AI coding agent that can help with:
- Bug fixes and improvements
- Documentation updates
- Implementing new features
- Code reviews and suggestions
- Writing tests
- And more!

### How it works

Once this PR is merged, we'll be able to interact with Claude by mentioning @claude in a pull request or issue comment.
Once the workflow is triggered, Claude will analyze the comment and surrounding context, and execute on the request in a GitHub action.

### Important Notes

- **This workflow won't take effect until this PR is merged**
- **@claude mentions won't work until after the merge is complete**
- The workflow runs automatically whenever Claude is mentioned in PR or issue comments
- Claude gets access to the entire PR or issue context including files, diffs, and previous comments
)";
}

// ============================================================

// ============================================================
namespace product {
    inline constexpr std::string_view kProductUrl = "https://claude.com/claude-code";
    inline constexpr std::string_view kClaudeAiBaseUrl = "https://claude.ai";
    inline constexpr std::string_view kClaudeAiStagingBaseUrl = "https://claude-ai.staging.ant.dev";
    inline constexpr std::string_view kClaudeAiLocalBaseUrl = "http://localhost:4000";
}

// ============================================================

// ============================================================
namespace spinner_verbs {
    inline constexpr std::array<std::string_view, 204> kVerbs = {
        "Accomplishing", "Actioning", "Actualizing", "Architecting", "Baking", "Beaming",
        "Beboppin'", "Befuddling", "Billowing", "Blanching", "Bloviating", "Boogieing",
        "Boondoggling", "Booping", "Bootstrapping", "Brewing", "Bunning", "Burrowing",
        "Calculating", "Canoodling", "Caramelizing", "Cascading", "Catapulting", "Cerebrating",
        "Channeling", "Channelling", "Choreographing", "Churning", "Clauding", "Coalescing",
        "Cogitating", "Combobulating", "Composing", "Computing", "Concocting", "Considering",
        "Contemplating", "Cooking", "Crafting", "Creating", "Crunching", "Crystallizing",
        "Cultivating", "Deciphering", "Deliberating", "Determining", "Dilly-dallying",
        "Discombobulating", "Doing", "Doodling", "Drizzling", "Ebbing", "Effecting",
        "Elucidating", "Embellishing", "Enchanting", "Envisioning", "Evaporating",
        "Fermenting", "Fiddle-faddling", "Finagling", "Flambéing", "Flibbertigibbeting",
        "Flowing", "Flummoxing", "Fluttering", "Forging", "Forming", "Frolicking",
        "Frosting", "Gallivanting", "Galloping", "Garnishing", "Generating", "Gesticulating",
        "Germinating", "Gitifying", "Grooving", "Gusting", "Harmonizing", "Hashing",
        "Hatching", "Herding", "Honking", "Hullaballooing", "Hyperspacing", "Ideating",
        "Imagining", "Improvising", "Incubating", "Inferring", "Infusing", "Ionizing",
        "Jitterbugging", "Julienning", "Kneading", "Leavening", "Levitating", "Lollygagging",
        "Manifesting", "Marinating", "Meandering", "Metamorphosing", "Misting", "Moonwalking",
        "Moseying", "Mulling", "Mustering", "Musing", "Nebulizing", "Nesting", "Newspapering",
        "Noodling", "Nucleating", "Orbiting", "Orchestrating", "Osmosing", "Perambulating",
        "Percolating", "Perusing", "Philosophising", "Photosynthesizing", "Pollinating",
        "Pondering", "Pontificating", "Pouncing", "Precipitating", "Prestidigitating",
        "Processing", "Proofing", "Propagating", "Puttering", "Puzzling", "Quantumizing",
        "Razzle-dazzling", "Razzmatazzing", "Recombobulating", "Reticulating", "Roosting",
        "Ruminating", "Sautéing", "Scampering", "Schlepping", "Scurrying", "Seasoning",
        "Shenaniganing", "Shimmying", "Simmering", "Skedaddling", "Sketching", "Slithering",
        "Smooshing", "Sock-hopping", "Spelunking", "Spinning", "Sprouting", "Stewing",
        "Sublimating", "Swirling", "Swooping", "Symbioting", "Synthesizing", "Tempering",
        "Thinking", "Thundering", "Tinkering", "Tomfoolering", "Topsy-turvying",
        "Transfiguring", "Transmuting", "Twisting", "Undulating", "Unfurling",
        "Unravelling", "Vibing", "Waddling", "Wandering", "Warping", "Whatchamacalliting",
        "Whirlpooling", "Whirring", "Whisking", "Wibbling", "Working", "Wrangling",
        "Zesting", "Zigzagging"
    };
}

// ============================================================

// ============================================================
namespace tool_names {
    inline constexpr std::string_view kAgentTool = "agent_tool";
    inline constexpr std::string_view kAskUserQuestionTool = "ask_user_question";
    inline constexpr std::string_view kBashTool = "bash";
    inline constexpr std::string_view kBriefTool = "brief";
    inline constexpr std::string_view kConfigTool = "config";
    inline constexpr std::string_view kCronCreateTool = "cron_create";
    inline constexpr std::string_view kCronDeleteTool = "cron_delete";
    inline constexpr std::string_view kCronListTool = "cron_list";
    inline constexpr std::string_view kEnterPlanModeTool = "enter_plan_mode";
    inline constexpr std::string_view kEnterWorktreeTool = "enter_worktree";
    inline constexpr std::string_view kExitPlanModeV2Tool = "exit_plan_mode_v2";
    inline constexpr std::string_view kExitWorktreeTool = "exit_worktree";
    inline constexpr std::string_view kFileEditTool = "file_edit";
    inline constexpr std::string_view kFileReadTool = "file_read";
    inline constexpr std::string_view kFileWriteTool = "file_write";
    inline constexpr std::string_view kGlobTool = "glob";
    inline constexpr std::string_view kGrepTool = "grep";
    inline constexpr std::string_view kListMcpResourcesTool = "list_mcp_resources";
    inline constexpr std::string_view kLspTool = "lsp";
    inline constexpr std::string_view kMcpTool = "mcp";
    inline constexpr std::string_view kNotebookEditTool = "notebook_edit";
    inline constexpr std::string_view kReadMcpResourceTool = "read_mcp_resource";
    inline constexpr std::string_view kSendMessageTool = "send_message";
    inline constexpr std::string_view kShellTool = "shell";
    inline constexpr std::string_view kSkillTool = "skill";
    inline constexpr std::string_view kSleepTool = "sleep";
    inline constexpr std::string_view kSyntheticOutputTool = "synthetic_output";
    inline constexpr std::string_view kTaskCreateTool = "task_create";
    inline constexpr std::string_view kTaskGetTool = "task_get";
    inline constexpr std::string_view kTaskListTool = "task_list";
    inline constexpr std::string_view kTaskOutputTool = "task_output";
    inline constexpr std::string_view kTaskStopTool = "task_stop";
    inline constexpr std::string_view kTaskUpdateTool = "task_update";
    inline constexpr std::string_view kTodoWriteTool = "todo_write";
    inline constexpr std::string_view kToolSearchTool = "tool_search";
    inline constexpr std::string_view kWebFetchTool = "web_fetch";
    inline constexpr std::string_view kWebSearchTool = "web_search";
    inline constexpr std::string_view kWorkflowTool = "workflow";
}

// ============================================================

// ============================================================
namespace prompts {
    inline constexpr std::string_view kSystemPromptDynamicBoundary = 
        "__SYSTEM_PROMPT_DYNAMIC_BOUNDARY__";
    inline constexpr std::string_view kClaudeCodeDocsMapUrl = 
        "https://code.claude.com/docs/en/claude_code_docs_map.md";
    inline constexpr std::string_view kDefaultAgentPrompt = R"(You are an agent for Claude Code, Anthropic's official CLI for Claude. Given the user's message, you should use the tools available to complete the task. Complete the task fully—don't gold-plate, but don't leave it half-done. When you complete the task, respond with a concise report covering what was done and any key findings — the caller will relay this to the user, so it only needs the essentials.)";
}

// ============================================================

// ============================================================
namespace xml_tags {
    inline constexpr std::string_view kTickTag = "<tick>";
}

// ============================================================

// ============================================================
namespace knowledge_cutoff {
    inline constexpr std::string_view kClaude46Sonnet = "August 2025";
    inline constexpr std::string_view kClaude46Opus = "May 2025";
    inline constexpr std::string_view kClaude45Opus = "May 2025";
    inline constexpr std::string_view kClaude4Haiku = "February 2025";
    inline constexpr std::string_view kClaude4 = "January 2025";

    [[nodiscard]] inline std::optional<std::string_view> get_for_model(std::string_view model_id) {
        if (model_id.find("claude-sonnet-4-6") != std::string_view::npos) return kClaude46Sonnet;
        if (model_id.find("claude-opus-4-6") != std::string_view::npos) return kClaude46Opus;
        if (model_id.find("claude-opus-4-5") != std::string_view::npos) return kClaude45Opus;
        if (model_id.find("claude-haiku-4") != std::string_view::npos) return kClaude4Haiku;
        if (model_id.find("claude-opus-4") != std::string_view::npos || 
            model_id.find("claude-sonnet-4") != std::string_view::npos) return kClaude4;
        return std::nullopt;
    }
}

// ============================================================

// ============================================================
namespace error_ids {
    inline constexpr uint32_t kToolUseSummaryGenerationFailed = 344;
}

// ============================================================

// ============================================================
namespace oauth {
    inline constexpr std::string_view kClaudeAiInferenceScope = "user:inference";
    inline constexpr std::string_view kClaudeAiProfileScope = "user:profile";
    inline constexpr std::string_view kConsoleScope = "org:create_api_key";
    inline constexpr std::string_view kOAuthBetaHeader = "oauth-2025-04-20";
    
    inline constexpr std::array<std::string_view, 2> kConsoleOAuthScopes = {
        kConsoleScope,
        kClaudeAiProfileScope
    };
    
    inline constexpr std::array<std::string_view, 5> kClaudeAiOAuthScopes = {
        kClaudeAiProfileScope,
        kClaudeAiInferenceScope,
        "user:sessions:claude_code",
        "user:mcp_servers",
        "user:file_upload"
    };
}

// ============================================================

// ============================================================
namespace cli_sysprompt {
    inline constexpr std::string_view kDefaultPrefix = 
        "You are Claude Code, Anthropic's official CLI for Claude.";
    inline constexpr std::string_view kAgentSdkClaudeCodePresetPrefix = 
        "You are Claude Code, Anthropic's official CLI for Claude, running within the Claude Agent SDK.";
    inline constexpr std::string_view kAgentSdkPrefix = 
        "You are a Claude agent, built on Anthropic's Claude Agent SDK.";
}

} // namespace cc::core::constants
