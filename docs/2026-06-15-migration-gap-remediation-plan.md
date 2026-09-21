# TS → C++ 迁移差距综合修复计划

> **编制日期**：2026-06-15
> **基准**：clang-debug 构建通过 · 660/660 ctest 全绿 · 1,200 个 C++ 文件（319K 行）
> **对照**：TypeScript 源码 1,923 文件 / 517K 行
> **总体完备度**：~82-87%（功能可演示/可测试 Alpha 阶段）
> **测试策略**：每个项目交付真实功能测试，仅新增、不修改现有测试资产
> **目标里程碑**：P0 → 生产可用；P0+P1+P2 → 功能完备

---

## 0. 核心架构替换说明（预期偏差，不属缺陷）

以下差距来自 React+Ink → FTXUI 范式转换，**不列入修复目标**：

| TS 侧 | C++ 侧 | 处置 |
|-------|--------|------|
| Ink 渲染引擎（97 文件） | FTXUI + ui/renderer + ui/termio（~25 文件） | 接受 |
| React reconciler / VDOM | FTXUI 原生 Diff | 接受 |
| AppState.tsx Context Provider | FTXUI Signal + state/ftxui_integration | 接受 |
| SentryErrorBoundary（React 专属） | C++ `std::expected` + 异常策略 | 接受 |
| 所有 `.d.ts` 类型声明文件 | C++ 编译期类型系统 | 接受 |
| TS `.test.ts`/`.spec.ts`（至少 14 个） | C++ `tests/test_*.cpp`（660 用例） | 接受 |
| BiDi 文本（`bidi.ts`） | 暂无 · 标记 P3 | 视需求启用 |

BYOC 与自托管 Runner（`environment-runner/`、`self-hosted-runner/`）在 TS 侧也受 `feature()` flag 构建期消除，C++ 侧未迁移**非缺陷**，按 P3 处理。

---

## 1. P0 — 生产阻塞（3 项，预估 5–7 天）

### P0-01：实现 11 个配置迁移函数的真实写回逻辑
- **文件**：`src/migrations/concrete_migrations.cppm` + `src/migrations/migration_runner.cppm`
- **问题**：`run_migration_1` ~ `run_migration_11` 全部为 detector-only，`new_model`、`value` 等变量以 `(void)` 丢弃，从未写入 config。
- **修复要点**：
  - 为每个迁移补齐 `Result<bool> apply(yyjson_mut_doc*, AppState&, ConfigPath)` 真实写回；
  - `migration_runner.cppm` 编排层：调用检测 → 触发 apply → 原子 rename 写入目标文件；
  - 迁移 4 (`migrateFennecToOpus`) 与 9 (`migrateSonnet45ToSonnet46`)：实际输出模型名并设置 `fast_mode` 标志；
  - 迁移 7：真正写入 `remote_control_at_startup` 键值；
  - 所有迁移失败回滚（临时文件→rename 成功才应用）。
- **测试**：`tests/test_state.cpp` 新增 `Migrations.All11RoundTripWithRealConfig`，fixture 涵盖 v0→current 全量升级序列、幂等重入、损坏文件回滚。

### P0-02：修复 SkillTool 存根 —— 接入 `cc_skills` 真实执行
- **文件**：`src/tools/missing_tools.cppm`（SkillTool 存根段）→ 新建 `src/tools/skill_tool.cppm`
- **问题**：当前 SkillTool 仅读取 skill 的 frontmatter 并返回 markdown，未触发 `cc::skills::*` 执行路径，也没有 skill workflow 回调。
- **修复要点**：
  - 依赖 `cc_skills_core + cc_skills`，按 `Skill.skill_path` 解析 yaml frontmatter 构建 `SkillExecutionContext`；
  - 调用 `execute_skill(ctx, budget, hooks)`，在 tool-call 循环里回传中间产物（thinking、side-query、hook triggers）；
  - 处理 skill 的 return schema：`structured_output` 约束、`should_use_sandbox` 判定；
  - `missing_tools.cppm` 中移除 SkillTool 的存根条目；`tool_registry.cppm` 指向新实现。
- **测试**：`tests/test_tools.cpp` 新增 `SkillTool.RunsBundledLoopSkill` + `SkillTool.RespectsBudget` + `SkillTool.StructuredOutputValidation`。

### P0-03：权限 hooks 执行引擎补齐 3 种触发器
- **文件**：`src/utils/hooks_execution.cppm` + `src/utils/hooks_registry.cppm` + `src/hooks/permission_resolver.cppm`
- **问题**：TS `utils/hooks/` 17 文件实现了 `execAgentHook`、`execHttpHook`、`execPromptHook` 三类触发路径，C++ 侧合并为 4 文件后仅保留 `apiQueryHookHelper` 的骨架。
- **修复要点**：
  - `HookTrigger = kAgent | kHttp | kPrompt | kFileChange | kPostSampling` 枚举；
  - `execHook(trigger, ctx, payload) -> Result<HookOutput>`：
    - `kAgent`：子代理隔离调用（限制 budget，禁止嵌套 agent hooks）；
    - `kHttp`：httplib GET/POST，SSRF 防护（解析目标 → deny RFC1918 + link-local，见 `ssrf_guard.cppm`）；
    - `kPrompt`：注入 system_prompt 节，遵循 section 顺序常量；
  - `registerFrontmatterHooks` / `registerSkillHooks` 从 markdown frontmatter 的 `hooks:` 列表构建注册表；
  - 将引擎接入 `cc_query` 的 `pre_tool_call` / `post_tool_call` / `pre_sample` 中间件。
- **测试**：`tests/test_hooks.cpp` + `tests/test_tools.cpp`，HTTP 使用 `httplib::Server` 本地监听，SSRF 用 `127.0.0.1` 拒绝用例。

---

## 2. P1 — 功能完整化（6 项，预估 9–11 天）

### P1-01：REPLTool 持久会话（替换临时脚本存根）
- **文件**：新建 `src/tools/repl_tool.cppm`，修改 `src/tools/missing_tools.cppm`（删除 REPLTool 存根）
- **问题**：当前存根为 `tmpfile → exec → delete` 的单次脚本执行，非持久 REPL 会话。
- **修复要点**：
  - `ReplSession { id, process, stdin_pipe, stdout_pipe, stderr_pipe, prompt_regex, history }`；
  - 按语言启动：Python → `python3 -i`，Node → `node`，Bun → `bun repl`，Ruby → `irb`；
  - 基于 prompt 正则切分响应（防止 read 阻塞）；
  - `REPL.create / REPL.eval / REPL.close / REPL.history` 四个子动作；
  - 会话跨 turn 存活，纳入 `store` 的 runtime resources，shutdown 时统一 SIGTERM → 2s → SIGKILL。
- **测试**：`tests/test_tools.cpp` `ReplTool.PythonEval` / `CrossTurnPersistence` / `CleanupOnDestruct`。

### P1-02：RemoteTriggerTool 真实 Bridge 集成（替换 curl 骨架）
- **文件**：`src/tools/missing_tools.cppm` → `src/tools/remote_trigger_tool.cppm`；接入 `cc_bridge`
- **修复要点**：
  - 通过 `bridge::session_api::lookup(session_id)` 获取远端句柄；
  - 构造 `bridge::messages::ToolTrigger{ agent_id, tool_name, input_json }` 经 `bridge_messaging::send()` 投递；
  - 响应走 `RemoteAgentTask` 的现有结果通道（复用 `tasks/types`）；
  - curl 路径降级为无 Bridge 可用时的 fallback（显式日志，默认 fail-closed）。
- **测试**：`tests/test_services.cpp` + in-memory bridge 桩。

### P1-03：8 个 notification hooks 存根 → 真实实现
- **文件**：`src/hooks/notifs/remaining_notifs.cppm`（当前集中返回空值）
- **涉及 hooks**：
  | Hook | 数据来源 | 预期输出 |
  |------|----------|----------|
  | NpmDeprecation | `services/analytics/growthbook.cppm` 实验值 | `std::optional<DeprecationNotice>` |
  | ModelMigration | `utils/model/deprecation.cppm` + 当前模型 | `{from,to,reason,deadline}` |
  | PluginAutoupdate | `utils/plugin_lifecycle.cppm` update registry | `vector<PluginUpdate>` |
  | PluginInstallationStatus | `services/plugins/installation_manager.cppm` 队列 | `{queued,installing,failed}` |
  | McpConnectivityStatus | `services/mcp/connection_manager.cppm` | per-server `state + last_error` |
  | SettingsErrors | `utils/settings_validation.cppm` | `vector<SettingViolation>` |
  | TeammateShutdown | `coordinator/swarm.cppm` 成员表 | `vector<AgentId> recently_down` |
  | SubscriptionSwitch | `services/rate_limit/claude_ai_limits_hook.cppm` | `{can,offer_tier,monthly_cost}` |
- **测试**：`tests/test_hooks.cpp` 用例一 hook 一条，fixture 注入可控 source。

### P1-04：AgentTool 拆分 —— 恢复 `runAgent / resumeAgent / forkSubagent`
- **文件**：`src/tools/agent_tool.cppm`（当前聚合 ~2.4K 行）；新建
  - `src/tools/agent_run.cppm`
  - `src/tools/agent_resume.cppm`
  - `src/tools/agent_fork.cppm`
  - `src/tools/agent_utils.cppm`
- **修复要点**：
  - `runAgent`：新 agent 生命周期（预算申请 → 权限上下文 → tool-call loop → 结算）；
  - `resumeAgent`：基于 `task_graph` 中已有节点恢复，读取 session history + 未完成工具调用；
  - `forkSubagent`：父 → 子 context 裁剪（敏感字段剥离），结果合并回父上下文；
  - 公共 helper 抽到 `agent_utils.cppm`（budget 申请、cost 结算、hook 回调适配）；
  - `agent_tool.cppm` 保留注册面 + dispatch。
- **测试**：`tests/test_tools.cpp` 补充 ResumeFromCrash、ForkContextIsStripped 用例。

### P1-05：补全 3 个缺失的根级模块
- **文件**：
  1. `src/history.cppm`（从 TS `history.ts` → `session/history.cppm` 升级）
  2. `src/task_types.cppm`（从 TS `Task.ts` → 独立 `Task` 模型 + ser/de）
  3. `src/state/teammate_view_helpers.cppm`（`TeammateView{collapsed,pinned,filter}` + 变换函数）
- **说明**：3 个文件逻辑目前散落在 `session/`、`tasks/`、`ui/teams/` 中，缺少公共访问点，导致跨模块调用时出现重复 helper。
- **测试**：`tests/test_state.cpp` + `tests/test_ui.cpp`。

### P1-06：interactiveHelpers 57KB 枢纽集中化
- **文件**：新建 `src/bootstrap/interactive_helpers.cppm`；从 `ui/prompt/*`、`commands/*`、`state/on_change_app_state.cppm` 抽取公共入口
- **修复要点**：
  - `InteractiveContext { prompt_state, queued_commands, clipboard, last_paste_ts, at_mention_index }`；
  - 统一处理：`processBashCommand / processSlashCommand / processTextPrompt / at-mention resolve / paste sanitization`；
  - 挂到 `entrypoints/cli_entrypoints.cppm` 的 FTXUI 组件初始化段。
- **测试**：新增 `tests/test_bootstrap.cpp` `Interactive.DispatchTable`，覆盖所有 slash 命令 + bash 前缀识别。

---

## 3. P2 — 质量提升（7 项，预估 11–14 天）

### P2-01：tree-sitter 集成（bash AST 精确分析 + 其他语言查询）
- **文件**：新建 `src/utils/tree_sitter/`；CMake `FetchContent` `tree-sitter/tree-sitter` + `tree-sitter/tree-sitter-bash`
- **修复要点**：
  - `tree_sitter.cppm`：`TSLanguageHandle` RAII，`Parser::parse(text) -> Tree`，`Query{pattern}.matches(tree)`；
  - `bash/ast.cppm`：基于 bash 语法的 `classify_dangerous(path, script) -> BashClassifierResult`（替换 bash_security.cppm 里正则近似版）；
  - 接入点：`BashTool` 预执行校验（现有 dangerousPatterns 检测升级）、`BriefTool` 符号级摘要。
- **风险**：新增外部依赖；需在 `CMakeLists.txt` 顶层与 yyjson/ftxui 同等处理，注意 iOS/Android 交叉编译时关闭（feature flag）。
- **测试**：`tests/test_utils.cpp` TreeSitter.* 覆盖 15+ bash AST 模式（pipe 危险链、heredoc、子 shell、环境变量展开注入）。

### P2-02：Migrations 编排层完整 fixture 测试
- **文件**：`tests/test_migrations.cpp`（新建）；补充 `src/migrations/migration_runner.cppm`
- **目标**：为 P0-01 提供真实 schema fixture；覆盖
  - v0（旧用户）→ current 完整链；
  - 中途断网/磁盘满/损坏 JSON 的恢复；
  - 迁移版本跳变（如 v1→v8 直接升级）；
  - 重复运行幂等；
  - 并发迁移（多进程启动）锁行为。

### P2-03：缺失 UI 组件补全（4 个）
- **文件**：
  - `src/ui/components/passes.cppm`（Passes 展示组件，接 `query_engine` 剩余 passes 数据）
  - `src/ui/components/grove.cppm`（Grove 查询结果可视化树形）
  - `src/ui/components/lsp_recommendation_menu.cppm`（LSP 插件推荐）
  - `src/ui/components/plugin_hint_menu.cppm`（Claude Code Hint 推荐）
- **4 组件共性**：接入 `cc_hooks` + `cc_state` 数据源；交互风格遵循 `ui/design_system/component_primitives.cppm`。
- **测试**：`tests/test_ui.cpp`，用 FXTUI Screen 抓帧断言（遵循现有 UI 测试模式）。

### P2-04：Permissions rules UI 细节补全
- **文件**：`src/ui/permissions/permission_rule_list.cppm` + `src/ui/permissions/permission_rules_ui.cppm`
- **缺失面板（TS permissions/rules/ 共 8 文件）**：
  1. `RecentDenialsTab` —— 从 `utils/permissions_engine` 拉取最近 N=50 拒绝；
  2. `WorkspaceTab` + `AddWorkspaceDirectory`/`RemoveWorkspaceDirectory` —— 工作区目录的 allow/deny 编辑；
  3. `PermissionRuleInput` + `PermissionRuleDescription` —— 规则编辑表单；
- **接入**：修改 `permission_rules_ui.cppm` 为多 Tab 容器，注册 Tab 路由。
- **测试**：`tests/test_ui.cpp` Permissions.TabSwitching + RuleCRUD。

### P2-05：plugin 工具组 20+ 缺失函数
- **文件**：按缺失项逐文件补全（均位于 `src/utils/plugin_*.cppm`）：
  | 缺失函数 | 目标文件 | 行为 |
  |----------|----------|------|
  | hintRecommendation | `plugin_recommendation.cppm` | 基于 usage histogram 推荐 |
  | installCounts | `plugin_marketplace_rules.cppm` | 拉取 marketplace API 计数 |
  | loadPluginAgents / loadPluginCommands / loadPluginHooks / loadPluginOutputStyles | 新建 `plugin_loader_extensions.cppm` | 扫描插件 `agents/`、`commands/`、`hooks/`、`output-styles/` 并注入注册表 |
  | lspPluginIntegration / mcpPluginIntegration | `plugin_integrations.cppm` | 插件声明的 LSP/MCP server 自动注册 |
  | pluginBlocklist / pluginFlagging / pluginPolicy | `plugin_policy.cppm` | blocklist URL + 本地 SHA256 deny-list + 策略判定 |
  | pluginAutoupdate / orphanedPluginFilter | `plugin_lifecycle.cppm` | 后台更新 + 孤儿插件清理 |
  | pluginOptionsStorage / reconciler / zipCache / zipCacheAdapters | 新建 `plugin_storage.cppm` | 选项 JSON 持久化 + 冲突合并 + zip 包缓存 |
  | officialMarketplaceGcs | `plugin_marketplace.cppm` | GCS 签名 URL 下载 |
- **测试**：`tests/test_tools.cpp` `Plugin.LoadsCommandsFromZip` / `Policy.BlocksDeniedHash`。

### P2-06：query/deps 依赖注入系统（或文档化不实现）
- **文件**：新建 `src/query/deps.cppm` 或在 `docs/` 新增 `why-no-di.md`
- **两种方向（需产品确认）**：
  - **A — 实现**：`QueryDeps { api_client_factory, hooks_engine, tools_ctx, state_handle, memdir, session_store, ... }`；在 `QueryEngine::build` 时注入，测试时提供 mock。
  - **B — 不实现**：文档说明 C++ 模块导出的构造函数已天然解耦（通过 `CMake target_link_libraries` 组合依赖边界），测试使用 googlemock/fakeit 直接替换依赖即可。
- **建议**：选 **B**（C++ 侧无运行时 DI 的必要），在 migration-audit-report 加一条明确说明。

### P2-07：daemon/workerRegistry + 标记 server/types
- **文件**：
  - 新建 `src/daemon/worker_registry.cppm`（Worker 池：`register/unregister/heartbeat/lookup/expire`，cap 1024，ttl 30s）；
  - 新建 `src/server/types.cppm`（`ServerSession{id,token,role}`、`DirectConnectRequest` 等）。
- **原因**：两个文件的内容当前都内联在 `daemon_server.cppm` / `server_routes.cppm`，类型无法被其他模块复用。
- **测试**：`tests/test_services.cpp` `WorkerRegistry.ExpiresStale`。

---

## 4. P3 — 可选增强（5 项，预估 8.5–15.5 天，启用前需确认需求）

### P3-01：BiDi 文本支持
- **文件**：`src/ui/layout/wrap_text.cppm` + 依赖 `fribidi`（或内置简易 L2/R2R 切换）
- **范围**：RTL 语言渲染；无内部需求可暂缓。

### P3-02：Terminal OSC Notification
- **文件**：`src/ui/hooks/hooks_ui.cppm`（新增 `useTerminalNotification`）+ `ui/termio/terminal_io.cppm`
- **实现**：OSC 777（通知）、OSC 0/2（标题）、OSC 9 / OSC 1337 通用通知；失败 fallback 静默。

### P3-03：BYOC / Self-hosted runner 实现
- **文件**：新建 `src/environment-runner/` + `src/self-hosted-runner/` 两套入口 + runner_protocol
- **说明**：需要 `feature("BYOC_ENVIRONMENT_RUNNER")` / `feature("SELF_HOSTED_RUNNER")` 开关；涉及容器沙箱（Docker API / containerd）+ runner 心跳 / 双向认证，预计单独里程碑。

### P3-04：Ink termio 9 种解析器拆分还原
- **文件**：`src/ui/termio/` 拆分出 `ansi.cppm / csi.cppm / dec.cppm / esc.cppm / osc.cppm / sgr.cppm / parser.cppm / tokenize.cppm / types.cppm`
- **理由**：当前 `terminal_io.cppm` 超 3K 行，可读性/测试覆盖率不佳；非功能性工作，按代码健康度预算排。

### P3-05：Chrome 集成 2 个缺失 hooks
- **文件**：
  - `src/hooks/chrome_extension_notification.cppm`
  - `src/hooks/prompts_from_chrome.cppm`
- **依赖**：`utils/chrome_messaging.cppm` 已存在 chromeNativeHost 基础；补齐 Native Messaging 协议握手 + prompt 跨进程桥接。
- **前提**：有 Chrome 扩展侧用户，否则无 ROI。

---

## 5. 执行顺序与里程碑

```
Week 1 — P0 (3项)         ───▶ 生产就绪
  P0-01 migrations writeback
  P0-02 skilltool real execute
  P0-03 hooks engine (agent/http/prompt)

Week 2–3 — P1 (6项)       ───▶ 功能完备 Alpha
  P1-01 repl persistent session
  P1-02 remote trigger over bridge
  P1-03 8 notif hooks real impl
  P1-04 split agent tool
  P1-05 3 missing root modules
  P1-06 interactiveHelpers centralize

Week 4–5 — P2 (7项)       ───▶ 功能完备 Beta
  P2-01 tree-sitter
  P2-02 migrations fixture tests
  P2-03 4 missing UI components
  P2-04 permissions rules UI tabs
  P2-05 plugin 20+ missing funcs
  P2-06 deps / document decision
  P2-07 daemon workerRegistry + server types

Week 6+ — P3 (可选)
  按需求优先级挑选
```

---

## 6. 验收与回归

| 里程碑 | 最低验收标准 |
|--------|-------------|
| P0 完成 | `ctest -j` 仍 660+ 全绿；新增 `P0-*` 测试用例数 ≥ 25；配置迁移在真实 v0 配置上成功 |
| P0+P1 完成 | 660 + 新增 ≥ 70 全绿；手动回归：`/skills` `/repl` `/doctor` 全流程；远程会话触发工具调用 |
| P2 完成 | 全绿 780+ 用例；TS vs C++ 模块级 grep 差异清单清零（除 §0 接受项） |
| 全量完成 | 820+ 用例；无 `missing_*.cppm` 存根残留；迁移审计报告 §13 全标 Resolved |

---

## 7. 附：已关闭的 P0/P1 项（参考 2026-06-15 审计 §13）

为避免重复工作，以下已在审计 §13 中标记 **Resolved**，不再纳入：

- 审计 §13-01：`runtime_registry.cppm` 3437→2100 行拆分 ✅
- 审计 §13-02：泛型 `Store<State, Action>` 与 AppState 解耦 ✅
- 审计 §13-03：JSON 规范统一到 `cc.utils.json`（99+ 导入方）✅
- 审计 §13-04：AppState 持久化读写对称 ✅
- 审计 §13-05：状态 schema 版本 + undo/redo + 不变量 ✅
- 审计 §13-06：LSP duplex 传输 + 7 解析器 ✅
- 审计 §13-07：OAuth macOS Keychain 后端 ✅
- 审计 §13-08a：LSP semantic-token 高亮叠加 ✅
- 审计 §13-08b：布局响应式 + dead stub 清理 ✅
- 审计 §13-09：`ide_at_mentioned` 真实解析 + Doctor 真实探针 ✅
- 审计 §13-10：660/660 测试、安全 3 项阻塞 ✅
- 审计 §6 安全：RuntimeFunctionTool fail-closed / Bash 混淆检测 / 路径符号链感知 ✅
- 审计 §10 P3：77 处 `popen` → `posix_spawn` ✅
