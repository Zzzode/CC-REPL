/// @file feature_flags.cppm
/// @brief Compile-time feature flags mirroring Bun's feature() gates in TS tools.ts.
///
/// These constexpr booleans control which tools are registered in the runtime
/// tool registry.  They mirror the Bun `feature('FLAG')` / `process.env` checks
/// in `src/tools.ts` lines 16-158 and 195-256 so that the C++ migration can
/// produce equivalent tool sets at compile time without a bundler.
///
/// To enable a feature for a custom build, flip the corresponding `constexpr bool`
/// to `true` (or pass a `-D` define via CMake if we add that plumbing later).
module;

export module cc.tools.feature_flags;

// TS REF: src/tools.ts:16-24  (USER_TYPE === 'ant' gates)
// Ant-internal build flag.  When true, REPLTool + SuggestBackgroundPRTool +
// ConfigTool + TungstenTool are registered (matching TS ant-only paths).
constexpr bool USER_TYPE_ANT = false;

// TS REF: src/tools.ts:25-28  (SleepTool gate)
// Enables the SleepTool.  TS gates this on PROACTIVE || KAIROS.
constexpr bool FEATURE_PROACTIVE = false;
constexpr bool FEATURE_KAIROS = false;

// TS REF: src/tools.ts:29-34  (Cron tools gate)
// CronCreateTool / CronDeleteTool / CronListTool registration.
constexpr bool FEATURE_AGENT_TRIGGERS = false;

// TS REF: src/tools.ts:36-38  (RemoteTriggerTool gate)
constexpr bool FEATURE_AGENT_TRIGGERS_REMOTE = false;

// TS REF: src/tools.ts:39-41  (MonitorTool gate)
constexpr bool FEATURE_MONITOR_TOOL = false;

// TS REF: src/tools.ts:42-44  (SendUserFileTool gate — KAIROS)
constexpr bool FEATURE_SEND_USER_FILE_TOOL = FEATURE_KAIROS;

// TS REF: src/tools.ts:45-49  (PushNotificationTool gate — KAIROS || KAIROS_PUSH_NOTIFICATION)
constexpr bool FEATURE_KAIROS_PUSH_NOTIFICATION = false;
constexpr bool FEATURE_PUSH_NOTIFICATION_TOOL =
    FEATURE_KAIROS || FEATURE_KAIROS_PUSH_NOTIFICATION;

// TS REF: src/tools.ts:50-52  (SubscribePRTool gate)
constexpr bool FEATURE_KAIROS_GITHUB_WEBHOOKS = false;

// TS REF: src/tools.ts:91-96  (VerifyPlanExecutionTool gate)
constexpr bool FEATURE_VERIFY_PLAN_EXECUTION = false;

// TS REF: src/tools.ts:107-109  (OverflowTestTool gate)
constexpr bool FEATURE_OVERFLOW_TEST_TOOL = false;

// TS REF: src/tools.ts:110-112  (CtxInspectTool gate)
constexpr bool FEATURE_CONTEXT_COLLAPSE = false;

// TS REF: src/tools.ts:113-116  (TerminalCaptureTool gate)
constexpr bool FEATURE_TERMINAL_PANEL = false;

// TS REF: src/tools.ts:117-119  (WebBrowserTool gate)
// Enabled in CPP builds — the browser tool has a working implementation.
constexpr bool FEATURE_WEB_BROWSER_TOOL = true;

// TS REF: src/tools.ts:123-125  (SnipTool gate)
constexpr bool FEATURE_HISTORY_SNIP = false;

// TS REF: src/tools.ts:126-128  (ListPeersTool gate)
constexpr bool FEATURE_UDS_INBOX = false;

// TS REF: src/tools.ts:129-134  (WorkflowTool gate — also calls initBundledWorkflows)
// Enabled in CPP — workflow tool has a working implementation.
constexpr bool FEATURE_WORKFLOW_SCRIPTS = true;

// TS REF: src/tools.ts:152-157  (PowerShellTool — runtime check isPowerShellToolEnabled())
// In CPP we register it unconditionally; execution is a no-op on non-Windows.
constexpr bool FEATURE_POWERSHELL_TOOL = true;

// TS REF: src/tools.ts:199  (isBashToolDisabled — runtime check)
// Bash is always enabled in CPP builds.
constexpr bool FEATURE_BASH_TOOL_ENABLED = true;

// TS REF: src/tools.ts:203  (hasEmbeddedSearchTools — ant-native bfs/ugrep)
// When true, Glob/Grep tools are suppressed (ant-native shell aliases handle it).
constexpr bool FEATURE_EMBEDDED_SEARCH_TOOLS = false;

// TS REF: src/tools.ts:220-222  (isTodoV2Enabled — TaskCreate/Get/Update/List tools)
// Enabled in CPP — task tools are fully implemented.
constexpr bool FEATURE_TODO_V2 = true;

// TS REF: src/tools.ts:226  (ENABLE_LSP_TOOL env var)
// Enabled in CPP — LSP tool has a working implementation.
constexpr bool FEATURE_ENABLE_LSP_TOOL = true;

// TS REF: src/tools.ts:227  (isWorktreeModeEnabled — Enter/ExitWorktree tools)
// Enabled in CPP — worktree tools are implemented.
constexpr bool FEATURE_WORKTREE_MODE = true;

// TS REF: src/tools.ts:230-232  (isAgentSwarmsEnabled — TeamCreate/Delete tools)
// Enabled in CPP — team tools are fully implemented.
constexpr bool FEATURE_AGENT_SWARMS_ENABLED = true;

// TS REF: src/tools.ts:246  (NODE_ENV === 'test' — TestingPermissionTool)
// In CPP we register the testing tool for all builds.
constexpr bool FEATURE_TESTING_PERMISSION_TOOL = true;

// TS REF: src/tools.ts:251  (isToolSearchEnabledOptimistic — ToolSearchTool)
// Enabled in CPP.
constexpr bool FEATURE_TOOL_SEARCH = true;

// TS REF: src/tools.ts:252-254  (isScriptToolEnabled — ScriptTool)
// Enabled in CPP — script tool has a sandboxed implementation.
constexpr bool FEATURE_SCRIPT_TOOL_ENABLED = true;

export namespace cc::tools::features {

// Re-export the flags under a readable namespace for call sites.
// TS REF: src/tools.ts:16-158, 195-256

/// Ant-only tools (REPLTool, SuggestBackgroundPRTool, ConfigTool, TungstenTool).
/// TS REF: src/tools.ts:16-24, 216-218, 234
inline constexpr bool kUserTypeAnt = USER_TYPE_ANT;

/// SleepTool registration.  TS REF: src/tools.ts:25-28, 236
inline constexpr bool kEnableSleepTool = FEATURE_PROACTIVE || FEATURE_KAIROS;

/// Cron tools (CronCreate/Delete/List).  TS REF: src/tools.ts:29-34, 237
inline constexpr bool kAgentTriggers = FEATURE_AGENT_TRIGGERS;

/// RemoteTriggerTool.  TS REF: src/tools.ts:36-38, 238
inline constexpr bool kAgentTriggersRemote = FEATURE_AGENT_TRIGGERS_REMOTE;

/// MonitorTool.  TS REF: src/tools.ts:39-41, 239
inline constexpr bool kMonitorTool = FEATURE_MONITOR_TOOL;

/// SendUserFileTool.  TS REF: src/tools.ts:42-44, 241
inline constexpr bool kSendUserFileTool = FEATURE_SEND_USER_FILE_TOOL;

/// PushNotificationTool.  TS REF: src/tools.ts:45-49, 242
inline constexpr bool kPushNotificationTool = FEATURE_PUSH_NOTIFICATION_TOOL;

/// SubscribePRTool.  TS REF: src/tools.ts:50-52, 243
inline constexpr bool kSubscribePRTool = FEATURE_KAIROS_GITHUB_WEBHOOKS;

/// VerifyPlanExecutionTool.  TS REF: src/tools.ts:91-96, 233
inline constexpr bool kVerifyPlanExecution = FEATURE_VERIFY_PLAN_EXECUTION;

/// OverflowTestTool.  TS REF: src/tools.ts:107-109, 223
inline constexpr bool kOverflowTestTool = FEATURE_OVERFLOW_TEST_TOOL;

/// CtxInspectTool.  TS REF: src/tools.ts:110-112, 224
inline constexpr bool kContextCollapse = FEATURE_CONTEXT_COLLAPSE;

/// TerminalCaptureTool.  TS REF: src/tools.ts:113-116, 225
inline constexpr bool kTerminalPanel = FEATURE_TERMINAL_PANEL;

/// WebBrowserTool.  TS REF: src/tools.ts:117-119, 219
inline constexpr bool kWebBrowserTool = FEATURE_WEB_BROWSER_TOOL;

/// SnipTool.  TS REF: src/tools.ts:123-125, 245
inline constexpr bool kHistorySnip = FEATURE_HISTORY_SNIP;

/// ListPeersTool.  TS REF: src/tools.ts:126-128, 229
inline constexpr bool kUdsInbox = FEATURE_UDS_INBOX;

/// WorkflowTool (also calls initBundledWorkflows in TS).  TS REF: src/tools.ts:129-134, 235
inline constexpr bool kWorkflowScripts = FEATURE_WORKFLOW_SCRIPTS;

/// PowerShellTool.  TS REF: src/tools.ts:152-157, 244
inline constexpr bool kPowerShellTool = FEATURE_POWERSHELL_TOOL;

/// BashTool enabled.  TS REF: src/tools.ts:199
inline constexpr bool kBashToolEnabled = FEATURE_BASH_TOOL_ENABLED;

/// Embedded search tools (ant-native bfs/ugrep).  TS REF: src/tools.ts:203
inline constexpr bool kEmbeddedSearchTools = FEATURE_EMBEDDED_SEARCH_TOOLS;

/// Todo V2 (TaskCreate/Get/Update/List).  TS REF: src/tools.ts:220-222
inline constexpr bool kTodoV2 = FEATURE_TODO_V2;

/// LSP tool.  TS REF: src/tools.ts:226
inline constexpr bool kEnableLspTool = FEATURE_ENABLE_LSP_TOOL;

/// Worktree mode (Enter/ExitWorktree).  TS REF: src/tools.ts:227
inline constexpr bool kWorktreeMode = FEATURE_WORKTREE_MODE;

/// Agent swarms (TeamCreate/Delete).  TS REF: src/tools.ts:230-232
inline constexpr bool kAgentSwarmsEnabled = FEATURE_AGENT_SWARMS_ENABLED;

/// TestingPermissionTool.  TS REF: src/tools.ts:246
inline constexpr bool kTestingPermissionTool = FEATURE_TESTING_PERMISSION_TOOL;

/// ToolSearchTool.  TS REF: src/tools.ts:251
inline constexpr bool kToolSearch = FEATURE_TOOL_SEARCH;

/// ScriptTool.  TS REF: src/tools.ts:252-254
inline constexpr bool kScriptToolEnabled = FEATURE_SCRIPT_TOOL_ENABLED;

} // namespace cc::tools::features
