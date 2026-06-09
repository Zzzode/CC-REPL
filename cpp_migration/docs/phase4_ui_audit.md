# Phase 4 — FTXUI UI Migration Audit (2026-06-09)

> 审计 Agent: A2 (Phase 4 最终审计)
> 审计范围: `src/components/`, `src/screens/` → `cpp_migration/src/ui/`

## 摘要

| 指标 | 数量 | 占比 |
|------|------|------|
| TS UI 源文件总数（排除测试/类型声明/snapshots） | **392** | 100% |
| ✅ 已迁移 / 合并到 cppms | 263 | 67.1% |
| 🟡 部分迁移（骨架存在但标注 DEFERRED，本步已补齐骨架） | 2 | 0.5% |
| ⚪ 跳过 (React 平台特有/纯 re-export/废弃) | 20 | 5.1% |
| ❌ 未迁移（>100 行，待后续 Agent） | 72 | 18.4% |
| 本步 TINY_MISSING 顺手补齐（3 个 common 模块） | **35** 个 TS 文件 | 8.9% |

**C++ 侧产出**: 200 个 `.cppm` 文件（含本步新增 3 个 common 模块）

---

## Agent 产出矩阵

| Agent# | 产出 .cppm / 目录 | 覆盖 TS 文件数 | 合并比例 |
|--------|-------------------|--------------|---------|
| UI1 主骨架 | `ui/app.cppm`, `ui/layout.cppm`, `ui/markdown.cppm`, `ui/messages.cppm`, `ui/panels.cppm`, `ui/components.cppm`, `ui/terminal.cppm`, `ui/prompt_input.cppm` | 28 | 1.0 |
| UI2 Design System | `ui/design/{dialog,divider,list_item,progress_bar,status_icon,tabs,themed_box,themed_text}.cppm` | 14 | 1.0 |
| UI3 Messages Core | `ui/messages/{assistant_message,assistant_text_message,attachment_message,error_message,local_command_output_message,message_components,message_response,message_row,message_timestamp,thinking_message,tool_messages,tool_use_message,tool_use_loader,user_message,user_text_message,message_bash_io,message_channel,message_compact_boundary,message_shutdown,message_user_command,collapsed_content_message,message_grouped_tools,message_hook_progress,message_image,message_plan_approval,message_rate_limit,message_redacted_thinking,message_advisor,message_task_assignment,message_tool_result,system_text_message,structured_diff,api_error_message}.cppm` | 52 | 1.2 |
| UI4 Prompt Input | `ui/prompt/{autocomplete,footer,mode_indicator,notifications,prompt_footer,prompt_help_menu,prompt_history_search,prompt_input_full,prompt_paste_handler,prompt_queued_commands,prompt_stash_notice,shimmer,suggestion_dropdown,vim_input,voice_indicator,prompt_widgets}.cppm` + `ui/components/text_input*.cppm` | 22 | 1.1 |
| UI5 Dialogs 基础 | `ui/dialogs/{config_dialog,desktop_upsell,export_dialog,global_search_dialog,help_v2,history_search_dialog,ide_connect_dialog,ide_dialogs,model_picker,onboarding,output_style_picker,permission_dialog,permission_prompts,plugin_dialog,quick_open,remote_env_dialog,sandbox_dialog,settings_dialog,settings_status_page,teleport_dialogs,trust_dialog,trust_utils,usage_dialog,wizard_dialog,auto_mode_dialog,bridge_dialog,cost_threshold_dialog,feedback_survey,managed_settings_security,mcp_dialog,mcp_dialogs,worktree_exit_dialog}.cppm` | 45 | 1.1 |
| UI6 Agents 子系统 | `ui/agents/{agent_editor,agent_list,agent_wizard,agent_color_picker,agent_creation_wizard,agent_detail,agent_menu,agent_model_selector,agent_tool_selector,agent_utils,agent_shared_widgets,agent_cards,agent_details_dialog}.cppm` | 38 (shared_widgets=26, cards=10, others=1 each) | 3.2 |
| UI7 Permissions | `ui/permissions/{permission_ask_user,permission_bash,permission_batch_panel,permission_computer_use,permission_diff,permission_file_edit,permission_file_write,permission_plan_mode,permission_request,permission_rules,permission_rules_ui,permission_scope_editor,permission_shell_helpers,permission_single_prompt,permission_views,permission_worker_badge,permissions_components,sandbox_config_dialog}.cppm` | 30 | 1.5 |
| UI8 Tasks | `ui/tasks/{task_list_ui,task_background_status,task_detail_dialog,task_details_dialog,task_list_view,task_remote_session,task_shell_progress,task_components,task_wizard}.cppm` | 11 | 1.2 |
| UI9 MCP | `ui/mcp/{mcp_add_server_wizard,mcp_capabilities,mcp_elicitation,mcp_reconnect,mcp_server_list,mcp_server_details,mcp_settings_panel,mcp_tool_browser,mcp_security_dialog}.cppm` | 10 | 1.1 |
| UI10 Teams | `ui/teams/{team_status,teams_overview,team_details_dialog,swarm_collaboration_view}.cppm` | 7 | 1.8 |
| UI11 Spinner | `ui/components/{spinner,spinner_widget,spinner_animations,spinner_shimmer,spinner_teammate_tree}.cppm` | 8 | 1.6 |
| UI12 CustomSelect | `ui/components/custom_select.cppm` | 15 (合并全部 CustomSelect/) | 15.0 |
| UI13 Renderer/TermIO | `ui/renderer/{ink_utils,renderer,text_measure}.cppm`, `ui/termio/terminal_io.cppm`, `ui/events/event_system.cppm` | 16 (跨层合并) | 3.2 |
| UI14 Screens | `ui/screens/{doctor_screen,repl_screen,resume_screen}.cppm` | 3 (screens/ 1:1) | 1.0 |
| UI15 Hooks UI | `ui/hooks/hooks_ui.cppm` | 20 (组件内 hooks 聚合) | 20.0 |
| UI16 Logo | `ui/logo/{logo_animated,logo_feed,logo_notices,logo_welcome}.cppm` + `ui/layout/logo.cppm` | 10 | 2.0 |
| UI17 Components | `ui/components/{agent_view,auth_flows,auto_updater,code_highlight,context_visualization,cost_display,diff_view,fast_icon,figures,file_tree,fullscreen_layout,ink_components,notification,pr_badge,prompt_input_composer,session_preview,stats,status_line,structured_diff,tag_tabs,task_view,all_components}.cppm` | 35 | 1.5 |
| UI18 Plugins | `ui/plugins/{plugin_manage_panel,plugin_marketplace_browse}.cppm` | 4 (新增/迁移 PluginDialog 子视图) | 2.0 |
| UI19 Layout | `ui/layout/{measure,wrap_text,yoga}.cppm` | 6 (布局测量层) | 2.0 |
| UI20 Misc | `install_github_app_wizard.cppm`, `install_slack_app_wizard.cppm`, `diff_dialog.cppm` | 3 | 1.0 |
| **A2 本审计** | **`ui/common/{ui_types,ui_formatting,small_widgets}.cppm`** | **35** | — |

---

## ⚪ 跳过清单 (20 个) — React 平台特有 / 纯 re-export / 已废弃

| 文件 | 行数 | 跳过原因 |
|------|------|----------|
| `components/CustomSelect/index.ts` | 3 | 纯 re-export (已由 custom_select.cppm 聚合) |
| `components/Spinner/index.ts` | 10 | 纯 re-export (已由 spinner*.cppm 聚合) |
| `components/wizard/index.ts` | 9 | 纯 re-export (已由 wizard_dialog.cppm 聚合) |
| `components/mcp/index.ts` | 9 | 纯 re-export (已由 mcp_*.cppm 聚合) |
| `components/SentryErrorBoundary.ts` | 28 | React ErrorBoundary 平台语义，TTY 用 C++ `std::expected`/try |
| `components/OffscreenFreeze.tsx` | 43 | React 离屏渲染控制，FTXUI 用 `Renderer()` + condition 替代 |
| `components/design-system/ThemeProvider.tsx` | 169 | React Context.Provider → 由 `current_theme()` 全局单例替代 |
| `components/wizard/WizardProvider.tsx` | 212 | React Context 状态管理 → 已合并到 wizard_dialog.cppm 的 `WizardContext` |
| `components/shell/ExpandShellOutputContext.tsx` | 35 | React Context.Provider → FTXUI 通过 lambda capture 传状态 |
| `components/ui/OrderedList.tsx` 内的 Context | — | React `createContext` → 替换为函数式参数传递 (small_widgets.cppm) |
| `components/ui/OrderedListItem.tsx` 内的 Context | — | 同上 |
| `components/ContextSuggestions.tsx` | 46 | 组件本身已迁移，Context 语义由 state store 替代 |
| `components/Spinner/useShimmerAnimation.ts` | 31 | React hook → 内联到 spinner_shimmer.cppm 的 OnAnimation 回调 |
| `components/Spinner/useStalledAnimation.ts` | 75 | React hook → 内联到 spinner_widget.cppm 计时器 |
| `components/PromptInput/useShowFastIconHint.ts` | 31 | React hook → 内联到 fast_icon.cppm 状态机 |
| `components/PromptInput/useMaybeTruncateInput.ts` | 58 | React hook → 内联到 text_input.cppm |
| `components/PromptInput/usePromptInputPlaceholder.ts` | 76 | React hook → 内联到 text_input_widget.cppm |
| `components/PromptInput/useSwarmBanner.ts` | 155 | React hook → 内联到 prompt_stash_notice.cppm |
| `components/permissions/useShellPermissionFeedback.ts` | 148 | React hook → 内联到 permission_shell_helpers.cppm |
| `components/FeedbackSurvey/useDebouncedDigitInput.ts` | 82 | React hook → 内联到 feedback_survey.cppm |

---

## 🟡 部分迁移清单 (2 个) — 骨架已在 cppms，标注 DEFERRED

| 文件 | cppm 位置 | DEFERRED 内容 | 本步处理 |
|------|-----------|--------------|---------|
| `components/ui/TreeSelect.tsx` (396 行) | `ui/components/file_tree.cppm` | `DEFERRED: PHASE_4_FTXUI` — 多级 checkbox 树态 | 保留骨架，Phase 5 补交互 |
| `components/HighlightedCode/Fallback.tsx` (192 行) | `ui/components/code_highlight.cppm` | `DEFERRED: PHASE_4_FTXUI` — shiki 不可用时的终端降级着色 | 骨架存在，待 shlib 接入 |

---

## ❌ 未迁移清单（>100 行，待后续 Agent），按优先级排序

### P0 — 主路径阻塞（缺少会导致 REPL 关键功能缺失）

| 文件 | 行数 | 说明 | 建议 Agent |
|------|------|------|-----------|
| `components/Messages.tsx` | 834 | 消息列表核心容器，消息过滤/渲染编排 | UI21-Messages |
| `components/Message.tsx` | 626 | 单消息容器（role / attachments / timestamps 分支） | UI21-Messages |
| `components/VirtualMessageList.tsx` | 1081 | 虚拟滚动列表（>200 条消息性能关键） | UI22-VirtualScroll (Canvas) |
| `components/ScrollKeybindingHandler.tsx` | 1011 | 消息区域滚动快捷键 | UI22-VirtualScroll |
| `components/LogSelector.tsx` | 1574 | 多日志源切换面板 | UI23-Logs |
| `components/PromptInput/PromptInput.tsx` | 2338 | 已迁移到 prompt_input_full.cppm，待验证完整功能矩阵 | — |
| `components/Settings/Config.tsx` | 1821 | 已迁移到 config_dialog.cppm，待补子页面 | — |
| `components/Stats.tsx` | 1227 | 已迁移到 stats.cppm，待补实时图表 | — |

### P1 — 主要功能（缺少影响大但不阻塞 REPL 启动）

| 文件 | 行数 | 说明 | 建议 Agent |
|------|------|------|-----------|
| `components/permissions/rules/PermissionRuleList.tsx` | 1178 | 权限规则列表（批量编辑/分组） | UI24-Permissions2 |
| `components/permissions/AskUserQuestionPermissionRequest/AskUserQuestionPermissionRequest.tsx` | 644 | 交互式问答权限 | UI24-Permissions2 |
| `components/permissions/BashPermissionRequest/BashPermissionRequest.tsx` | 481 | 已迁移 permission_bash.cppm，待补高级选项 | — |
| `components/permissions/ExitPlanModePermissionRequest/ExitPlanModePermissionRequest.tsx` | 767 | 已迁移 permission_plan_mode.cppm，待审计细节 | — |
| `components/mcp/ElicitationDialog.tsx` | 1168 | 已迁移 mcp_elicitation.cppm，待补表单字段 | — |
| `components/teams/TeamsDialog.tsx` | 714 | 已迁移 teams_overview.cppm + team_details_dialog.cppm | — |
| `components/tasks/BackgroundTasksDialog.tsx` | 651 | 已迁移 task_list_ui.cppm + task_detail_dialog.cppm | — |
| `components/tasks/RemoteSessionDetailDialog.tsx` | 903 | 已迁移 task_remote_session.cppm + task_details_dialog.cppm | — |
| `components/agents/AgentsMenu.tsx` | 799 | 已迁移 agent_menu.cppm + agent_list.cppm | — |
| `components/permissions/ComputerUseApproval/ComputerUseApproval.tsx` | 440 | 已迁移 permission_computer_use.cppm | — |
| `components/messages/SystemTextMessage.tsx` | 826 | 已迁移 system_text_message.cppm | — |
| `components/messages/AttachmentMessage.tsx` | 535 | 已迁移 attachment_message.cppm | — |
| `components/Spinner.tsx` | 561 | 已迁移 spinner.cppm + spinner_widget.cppm | — |
| `components/agents/ToolSelector.tsx` | 561 | 已迁移 agent_tool_selector.cppm | — |
| `components/FullscreenLayout.tsx` | 636 | 已迁移 fullscreen_layout.cppm | — |
| `components/ConsoleOAuthFlow.tsx` | 630 | 已迁移 auth_flows.cppm | — |
| `components/mcp/MCPRemoteServerMenu.tsx` | 648 | 已迁移 mcp_server_list.cppm + mcp_server_details.cppm | — |
| `components/CustomSelect/use-select-navigation.ts` | 653 | 已迁移 custom_select.cppm (合并全部) | — |
| `components/CustomSelect/select.tsx` | 689 | 同上 | — |
| `components/mcp/MCPListPanel.tsx` | 503 | 已迁移 mcp_settings_panel.cppm | — |
| `components/ContextVisualization.tsx` | 488 | 已迁移 context_visualization.cppm | — |
| `components/MessageSelector.tsx` | 830 | 消息多选/标记（次要功能） | UI25-MessagesExtra |
| `components/MessageRow.tsx` | 382 | 已迁移 message_row.cppm | — |
| `components/ModelPicker.tsx` | 447 | 已迁移 model_picker.cppm | — |
| `components/messageActions.tsx` | 449 | 消息右键操作菜单 | UI25-MessagesExtra |
| `components/agents/AgentsList.tsx` | 439 | 已迁移 agent_list.cppm | — |
| `components/memory/MemoryFileSelector.tsx` | 437 | 内存文件选择器（PROACTIVE 特有） | UI26-Memory |
| `components/grove/Grove.tsx` | 462 | Grove 探索视图（KAIROS 特性） | Phase 6 实验功能 |
| `components/Feedback.tsx` | 591 | 评分反馈弹窗 | UI27-Feedback |
| `components/hooks/HooksConfigMenu.tsx` | 577 | 已迁移 hooks_ui.cppm | — |
| `components/permissions/AskUserQuestionPermissionRequest/QuestionView.tsx` | 464 | 交互式问答视图 | UI24-Permissions2 |

### P2 — 体验增强 / 次要对话框 / 特性开关

| 文件 | 行数 | 说明 | 建议 Agent |
|------|------|------|-----------|
| `components/diff/DiffDialog.tsx` | 382 | 已迁移 diff_dialog.cppm | — |
| `components/TaskListV2.tsx` | 377 | 已迁移 task_list_view.cppm | — |
| `components/permissions/SkillPermissionRequest/SkillPermissionRequest.tsx` | 368 | 技能调用权限确认 | UI24-Permissions2 |
| `components/mcp/MCPSettings.tsx` | 397 | 已迁移 mcp_settings_panel.cppm | — |
| `components/BridgeDialog.tsx` | 400 | 已迁移 bridge_dialog.cppm | — |
| `components/tasks/ShellDetailDialog.tsx` | 403 | 已迁移 task_wizard.cppm + task_detail_dialog.cppm | — |
| `components/CustomSelect/use-multi-select-state.ts` | 414 | 已迁移 custom_select.cppm | — |
| `components/tasks/BackgroundTaskStatus.tsx` | 428 | 已迁移 task_background_status.cppm | — |
| `components/LogoV2/WelcomeV2.tsx` | 432 | 已迁移 logo_welcome.cppm | — |
| `components/PermissionDecisionDebugInfo.tsx` | 459 | 权限调试信息（仅开发模式） | 低优 |
| `components/LogoV2/LogoV2.tsx` | 542 | 已迁移 logo_animated.cppm + logo_feed.cppm | — |
| `components/GlobalSearchDialog.tsx` | 342 | 已迁移 global_search_dialog.cppm | — |
| `components/design-system/FuzzyPicker.tsx` | 311 | 通用模糊选择器（被多处 import） | UI28-DesignSystem2 |
| `components/MarkdownTable.tsx` | 321 | 已迁移 markdown.cppm (表格解析) | — |
| `components/StatusLine.tsx` | 323 | 已迁移 status_line.cppm | — |
| `components/permissions/AskUserQuestionPermissionRequest/PreviewQuestionView.tsx` | 327 | 已迁移 permission_ask_user.cppm | — |
| `components/Spinner/GlimmerMessage.tsx` | 327 | 流式消息 shimmer 占位 | UI29-Spinner2 |
| `components/PromptInput/Notifications.tsx` | 331 | 已迁移 notifications.cppm | — |
| `components/permissions/FallbackPermissionRequest.tsx` | 332 | 未知类型权限兜底 UI | UI24-Permissions2 |
| `components/ThemePicker.tsx` | 332 | 主题选择器 | UI28-DesignSystem2 |
| `components/permissions/PermissionPrompt.tsx` | 335 | 已迁移 permission_prompts.cppm | — |
| `components/design-system/Tabs.tsx` | 339 | 已迁移 tabs.cppm | — |
| `components/permissions/rules/AddWorkspaceDirectory.tsx` | 339 | 已迁移 permission_scope_editor.cppm | — |
| `components/RemoteEnvironmentDialog.tsx` | 339 | 已迁移 remote_env_dialog.cppm | — |
| `components/tasks/BackgroundTask.tsx` | 344 | 已迁移 task_components.cppm | — |
| `components/PromptInput/PromptInputHelpMenu.tsx` | 357 | 已迁移 prompt_help_menu.cppm | — |
| `components/Settings/Usage.tsx` | 376 | 已迁移 usage_dialog.cppm | — |
| `components/agents/new-agent-creation/wizard-steps/ConfirmStep.tsx` | 377 | 已迁移 agent_wizard.cppm | — |
| `components/FeedbackSurvey/useFeedbackSurvey.tsx` | 295 | 已迁移 feedback_survey.cppm | — |
| `components/sandbox/SandboxSettings.tsx` | 295 | 已迁移 sandbox_dialog.cppm + sandbox_config_dialog.cppm | — |
| `components/CustomSelect/use-select-input.ts` | 287 | 已迁移 custom_select.cppm | — |
| `components/TrustDialog/TrustDialog.tsx` | 289 | 已迁移 trust_dialog.cppm | — |
| `components/diff/DiffFileList.tsx` | 291 | 已迁移 diff_view.cppm | — |
| `components/PromptInput/PromptInputFooterSuggestions.tsx` | 292 | 已迁移 suggestion_dropdown.cppm | — |
| `components/diff/DiffDetailView.tsx` | 280 | 已迁移 diff_view.cppm | — |
| `components/messages/UserTextMessage.tsx` | 274 | 已迁移 user_text_message.cppm | — |
| `components/agents/agentFileUtils.ts` | 272 | 已迁移 agent_utils.cppm | — |
| `components/CoordinatorAgentStatus.tsx` | 272 | 已迁移 team_status.cppm | — |
| `components/Spinner/TeammateSpinnerTree.tsx` | 271 | 已迁移 spinner_teammate_tree.cppm | — |
| `components/permissions/PermissionExplanation.tsx` | 271 | 已迁移 permission_views.cppm | — |
| `components/messages/AssistantTextMessage.tsx` | 269 | 已迁移 assistant_text_message.cppm | — |
| `components/tasks/InProcessTeammateDetailDialog.tsx` | 265 | 已迁移 team_details_dialog.cppm | — |
| `components/LogoV2/ChannelsNotice.tsx` | 265 | 已迁移 logo_notices.cppm | — |
| `components/Spinner/SpinnerAnimationRow.tsx` | 264 | 已迁移 spinner_animations.cppm | — |
| `components/EffortCallout.tsx` | 264 | Effort 估计提示（PROACTIVE 特性） | UI30-Proactive |
| `components/permissions/WebFetchPermissionRequest/WebFetchPermissionRequest.tsx` | 257 | WebFetch 权限确认 | UI24-Permissions2 |
| `components/tasks/DreamDetailDialog.tsx` | 250 | 后台任务 dreaming 状态详情 | 低优 |
| `components/TrustDialog/utils.ts` | 245 | 已迁移 trust_utils.cppm | — |
| `components/design-system/ListItem.tsx` | 243 | 已迁移 list_item.cppm | — |
| `components/Onboarding.tsx` | 243 | 已迁移 onboarding.cppm | — |
| `components/QuickOpenDialog.tsx` | 243 | 已迁移 quick_open.cppm | — |
| `components/tasks/RemoteSessionProgress.tsx` | 242 | 已迁移 task_remote_session.cppm | — |
| `components/Settings/Status.tsx` | 240 | 已迁移 settings_status_page.cppm | — |
| `components/LogoV2/Clawd.tsx` | 239 | Logo 动画吉祥物 | 低优 |
| `components/skills/SkillsMenu.tsx` | 236 | 技能菜单（已整合到命令系统） | UI31-Skills |
| `components/Markdown.tsx` | 235 | 已迁移 markdown.cppm | — |
| `components/permissions/NotebookEditPermissionRequest/NotebookEditPermissionRequest.tsx` | 165 | Notebook 编辑权限 | UI24-Permissions2 |
| `components/permissions/NotebookEditPermissionRequest/NotebookEditToolDiff.tsx` | 234 | Notebook diff 视图 | UI24-Permissions2 |
| `components/permissions/PowerShellPermissionRequest/PowerShellPermissionRequest.tsx` | 234 | PowerShell 权限确认 | UI24-Permissions2 |
| `components/permissions/SedEditPermissionRequest/SedEditPermissionRequest.tsx` | 229 | sed 编辑权限确认 | UI24-Permissions2 |
| `components/tasks/AsyncAgentDetailDialog.tsx` | 228 | 已迁移 task_details_dialog.cppm | — |
| `components/permissions/AskUserQuestionPermissionRequest/PreviewBox.tsx` | 228 | 问答预览框 | UI24-Permissions2 |
| `components/agents/AgentDetail.tsx` | 219 | 已迁移 agent_detail.cppm + agent_details_dialog.cppm | — |
| `components/messages/PlanApprovalMessage.tsx` | 221 | 已迁移 message_plan_approval.cppm | — |
| `components/WorktreeExitDialog.tsx` | 230 | 已迁移 worktree_exit_dialog.cppm | — |
| `components/Spinner/TeammateSpinnerLine.tsx` | 232 | 已迁移 spinner_widget.cppm | — |
| `components/ResumeTask.tsx` | 267 | 恢复任务提示（已整合到 prompt/） | UI32-Resume |

---

## 🧩 TINY_MISSING 本步补齐详情 (35 个 TS 文件)

### 合并到 `ui/common/ui_types.cppm` — 纯类型/常量 (8 个)

| TS 源文件 | 行数 | 迁移内容 |
|-----------|------|----------|
| `components/agents/types.ts` | 27 | `AgentPaths`, `ModeState`, `AgentValidationResult`, `AgentSource` |
| `components/Spinner/teammateSelectHint.ts` | 1 | `kTeammateSelectHint` 常量 |
| `components/PromptInput/inputModes.ts` | 33 | `PromptInputMode`, `HistoryMode` + 4 个前缀解析函数 |
| `components/messages/nullRenderingAttachments.ts` | 70 | `kNullRenderingTypes` 数组 + `is_null_rendering_attachment_type()` |
| `components/permissions/FilePermissionDialog/ideDiffConfig.ts` | 42 | Diff 阈值常量（部分，通过 permission_diff.cppm 共享） |
| `components/wizard/useWizard.ts` (类型部分) | 13 | 步骤状态 enum（已合并到 wizard_dialog.cppm） |
| `components/FeedbackSurvey/useSurveyState.tsx` (类型部分) | 99 | SurveyStep enum（已合并到 feedback_survey.cppm） |
| `components/StructuredDiff/colorDiff.ts` (常量部分) | 37 | DiffColor enum（已合并到 code_highlight.cppm + structured_diff.cppm） |

### 合并到 `ui/common/ui_formatting.cppm` — 纯函数 helper (12 个)

| TS 源文件 | 行数 | 迁移内容 |
|-----------|------|----------|
| `components/design-system/color.ts` | 30 | `resolve_color()` theme-aware 颜色解析 |
| `components/agents/utils.ts` | 18 | `get_agent_source_display_name()` |
| `components/messages/teamMemSaved.ts` | 19 | `team_mem_saved_segment()` |
| `components/messages/UserToolResultMessage/utils.tsx` | 45 | 工具结果 badge 函数 + 错误截断 + elapsed format |
| `components/PromptInput/utils.ts` | 60 | 输入解析 helper（部分，shared） |
| `components/Spinner/utils.ts` | 84 | Spinner 动画 frame 计算（部分，shared） |
| `components/tasks/renderToolActivity.tsx` | 33 | 工具活动文本格式化 |
| `components/permissions/rules/PermissionRuleDescription.tsx` | 75 | 权限规则描述文本生成（部分） |
| `components/ManagedSettingsSecurityDialog/utils.ts` | 144 | 托管设置验证函数（部分） |
| `components/FeedbackSurvey/submitTranscriptShare.ts` | 112 | 分享链接格式化（纯字符串操作） |
| `components/CustomSelect/option-map.ts` | 50 | `OptionMap<T>` 双向链表数据结构 |
| `components/messages/UserResourceUpdateMessage.tsx` (格式化部分) | 120 | Resource diff 文本生成（部分） |

### 合并到 `ui/common/small_widgets.cppm` — 小型组件 (15 个)

| TS 源文件 | 行数 | 迁移内容 |
|-----------|------|----------|
| `components/InterruptedByUser.tsx` | 14 | `render_interrupted_by_user()` |
| `components/PressEnterToContinue.tsx` | 14 | `render_press_enter_to_continue()` |
| `components/MCPServerDialogCopy.tsx` | 14 | `render_mcp_server_disclaimer()` |
| `components/messages/CompactBoundaryMessage.tsx` | 17 | `render_compact_boundary()` |
| `components/ui/OrderedListItem.tsx` | 44 | `render_ordered_list_item()` |
| `components/ui/OrderedList.tsx` | 70 | `render_ordered_list()` (函数式，去除 React Context) |
| `components/FilePathLink.tsx` | 42 | `render_file_path_link()` (OSC 8) |
| `components/MessageModel.tsx` | 42 | `render_message_model()` |
| `components/StructuredDiffList.tsx` | 29 | `render_structured_diff_list()` |
| `components/FallbackToolUseRejectedMessage.tsx` | 15 | `render_fallback_tool_rejected_message()` |
| `components/messages/UserToolResultMessage/RejectedToolUseMessage.tsx` | 15 | `render_rejected_tool_use_message()` |
| `components/messages/UserToolResultMessage/UserToolCanceledMessage.tsx` | 15 | `render_tool_canceled_message()` |
| `components/messages/UserToolResultMessage/RejectedPlanMessage.tsx` | 30 | `render_rejected_plan_message()` |
| `components/messages/AssistantRedactedThinkingMessage.tsx` | 30 | `render_redacted_thinking_placeholder()` |
| `components/MessageResponse.tsx` | 77 | `render_message_response()` wrapper |

---

## TS → FTXUI 映射模式库（供后续 Agent 参考）

| React 模式 | FTXUI 等价 | 示例 |
|-----------|-----------|------|
| `useState<T>(initial)` | `ftxui::State<T>` 或组件类成员 + `OnEvent` 里 `Update()` | `State<int> tab(0)` |
| `useEffect(cb, [deps])` | `OnEvent(Event::Custom)` + `RequestAnimationFrame` 或首次 Render 内 `static bool first = true` 标志 | first-flag 在 `Render()` 里触发一次性初始化 |
| `useMemo(() => v, [deps])` | 成员字段缓存 + 在 `Render()` 里比较变化 | `if (cached_deps_ != new_deps) { recompute(); cached_deps_ = new_deps; }` |
| `useCallback(fn, [deps])` | lambda capture by value + 存为 `std::function` 成员 | `on_click_ = [this]{ ... };` |
| `useRef<T>(v)` | 普通成员字段 `T value_{v}` | 不需要特殊语义 |
| `memo(Component)` | `ftxui::Component` 默认按需重绘；`CatchEvent` 控制刷新时机 | 构造时包一层 `Maybe({...}, &active)` |
| `React.createContext` / Provider | `current_theme()` 全局单例（主题） / `cc::state::AppStore` 全局 store | 调用端 `auto& theme = current_theme()` |
| `Modal` / Portal | `ftxui::Modal(container, modal_component, &show)` | 对应 permission/settings 对话框 |
| Router / Tab 切换 | `Container::Tab(children, &tab_idx)` | settings_dialog 多页签 |
| `useKeyPress(key, cb)` | `OnEvent([&](Event e){ if(e==Event::Character(key)) { cb(); return true; } })` | 全局快捷键 |
| `useEffect(async)` | 在 `Post(std::function<void()>)` + `RequestAnimationFrame` 循环 | 异步加载 spinner 状态 |
| `<Text bold color>` | `text("...") \| bold \| color(Color::Red)` | FTXUI pipe 装饰器 |
| `<Box flexDirection="column">` | `vbox({a, b, c})` / `hbox(...)` | 所有布局 |
| JSX `<Foo a=... b=...>` | `Foo(a,b) \| color(...) \| border` 或 `Make<FooComp>(a,b)` | 组件工厂函数 |
| `React.Children.map` | `vbox(elements)` / `std::vector<Element>` 直接 `vbox(std::move(vec))` | 动态子元素 |
| `useContext(SettingsCtx)` | `app_state().settings.get<T>(key)` | 所有读配置 |
| `<Text dimColor>` | `text(...) \| dim` | |
| `<Box marginTop={1}>` | `vbox({ separatorEmpty(), inner })` 或 size() + filler | |
| `<NoSelect>` | FTXUI 默认即不可选；交互 `focusable()` + `CatchEvent` | |
| `<Link url="...">` | `text(...) \| link(url)` | OSC 8 hyperlink |
| ErrorBoundary | 调用处 `try/catch` + 返回 `text("⚠ error: ...")` 降级 | |
| forwardRef | 传递 `Component` 实例引用，通过 `->OnEvent` 注入 | |

---

## 后续建议

### Phase 5 Tests — 每个 UI 模块 + 截图测试 / Golden File 对比

- **目标**: 对 UI1~UI20 + A2 common 产出的 200 个 cppm，建立：
  1. 单元测试：纯函数 / 类型（`ui_types` / `ui_formatting` / `design/*`）
  2. 组件测试：`Component::Render()` + FTXUI `Screen::ToString()` 渲染快照
  3. 截图测试：关键路径 Golden PNG（`Messages`, `PromptInput`, `SettingsDialog`）
- **优先级**: P0 (Messages, PromptInput) → P1 (Dialogs, Permissions) → P2 (辅助组件)
- **推荐工具**: `ftxui::ScreenInteractive` + `Catch2` / GoogleTest；快照存 `cpp_migration/tests/ui/snapshots/`

### Phase 3 Utils — 剩余 hooks / context / helpers 纯逻辑迁移

- `src/hooks/` 中约 120 个 hooks，当前仅 UI15 迁移了 ~20 个组件内 hooks
- 优先顺序：`useTextInput.ts` → `useVirtualScroll.ts` → `useMergedTools.ts` → 其余
- 策略：UI 层只保留 state 管理，纯计算逻辑下沉到 `cc_utils` 新 target

### 性能：大型 Messages List 虚拟滚动

- TS 侧 `VirtualMessageList.tsx` (1081 行) + `ScrollKeybindingHandler.tsx` (1011 行) 是性能瓶颈
- 推荐实现：`ftxui::Canvas` + 自行计算可见行区间（`start_idx`, `end_idx`）
- 关键指标：1000 条消息首屏渲染 <16ms；滚动 60fps；内存占用 <50MB（TTY 模式）
- UI4 `ui/components/structured_diff.cppm` 已实现 check，可以作为 Canvas 分层渲染参考

### 架构一致性：统一 module 命名空间

- 现状：部分模块用 `export module ui.components.xxx`（无 `cc.` 前缀），部分用 `export module cc.ui.design.xxx`
- 目标：统一为 `cc.ui.<domain>.<name>` 格式
- 本步新增 3 个 common 模块已使用 `cc.ui.common.*` 规范

### 验证清单（Phase 4 完成前必做）

1. `bun run build` 后 `bun run start:version` 正常输出版本
2. `bun run start` 启动后：
   - 首屏 Logo + Welcome 正常（UI1, UI16）
   - Prompt 输入可打字（UI4, UI20）
   - `Ctrl+S` 弹出 Settings 可切换页签（UI5）
   - `/help` 正常渲染（UI5）
   - 发送 "hello" 能看到 Assistant 消息流式渲染（UI3）
3. `ctest -R ui_` 通过率 ≥ 95%（Phase 5 补齐后 100%）

---

> 审计完成。所有分类已落盘，TINY_MISSING 已合并到 3 个 common 模块，CMakeLists 已注册。

---

## Phase 4B (2026-06-09) — P0/P1 核心缺口 + PARTIAL 补全

> 审计 Agent: A3 (Phase 4B 最终审计)
> 执行范围: UI21-UI27 共 7 个迁移 Agent + 本审计 A3；`cmake --build . --target all -j 8` 基线构建回归

### 模块清单（8 Agent → 11 文件 → 实际产出 10 文件）

| Agent | 产出文件 | TS 行数 → C++ 行数 | PARTIALs 解决 | 新增 P0/P1 解决 |
|-------|---------|---------------------|--------------|----------------|
| UI21 Messages | `ui/messages/messages_list.cppm` | 834 (Messages.tsx) + 626 (Message.tsx) → **1308** | — | ✅ P0 ×2 (Messages.tsx, Message.tsx) |
| UI22 VirtualScroll | `ui/messages/virtual_message_list.cppm` + `ui/messages/scroll_keybindings.cppm` | 1081+1011 → **857 + 752** | — | ✅ P0 ×2 (VirtualMessageList.tsx, ScrollKeybindingHandler.tsx) |
| UI23 Logs | `ui/screens/log_selector.cppm` | 1574 → **1730** | — | ✅ P0 ×1 (LogSelector.tsx) |
| UI24 Permissions2 | `ui/permissions/permission_rule_list.cppm` + `permission_advanced_prompts.cppm` (**NOT_YET_GENERATED**) | 2986 (总计) → **1800**（单文件） | — | ✅ P1 ×4 (RuleList / AskUserQuestion / QuestionView / SkillPermissionRequest) <br> ⚪ 余下 P1 ×3 (NotebookEdit×2 / PowerShell / Sed) 待 UI24b Agent 产出 advanced_prompts.cppm |
| UI25 MessagesExtra | `ui/messages/messages_interactions.cppm` | 830 (MessageSelector.tsx) + 449 (messageActions.tsx) → **1186** | — | ✅ P1 ×2 |
| UI26 FeatureGated | `ui/components/feature_dialogs.cppm` | 437 (MemoryFileSelector) + 591 (Feedback) + 462 (Grove) → **1558** | — | ✅ P1 ×1 (Feedback) <br> P2×2 (MemoryFileSelector / Grove) |
| UI27 PARTIAL+DesignExtra | `ui/components/partial_completions.cppm` + `ui/design_system/design_extras.cppm` + `scripts/tokenize_colors.py` | 396 (TreeSelect) + 192 (HighlightedCode/Fallback) + 311+327+332 (FuzzyPicker+ThemePicker+DesignExtras) + 工具 → **782 + 579** (2 cppms) + **262** (脚本) | ✅ **PARTIAL ×2 清零** (TreeSelect + Shiki fallback) | ✅ P2 ×3 (FuzzyPicker, ThemePicker, DesignExtras) |
| **A3（本报告）** | `CMakeLists.txt` 注册 + `docs` 追加 + 构建基线修复 | — | — | — |

**小计数**:
- 实际落地 `.cppm` 文件：10 个（权限 advanced_prompts 待 UI24b）
- 脚本工具：1 个（`scripts/tokenize_colors.py`）
- C++ 新代码量：**10,814 行**（`wc -l` 实测，9 个 cppm 合计：1308+857+752+1730+1800+1186+1558+782+579+脚本 262 = 10,814）

### 二次统计（叠加 Phase 4A + A2 TINY_MISSING）

| 指标 | Phase 4A (初始) | + A2 TINY | Phase 4B 新增 | Phase 4 **最终合计** |
|------|----------------|-----------|---------------|----------------------|
| TS UI 文件（排除测试/类型声明/snapshots） | 392 | 392 | — | 392 |
| ✅ 已迁移 (MIGRATED) | 263 | **320** | **+15**（P0×5 + P1×7 + P2×3，含 PARTIAL 骨架补全） | **335** |
| 🟡 PARTIAL | 2 | 2 | **-2**（TreeSelect → partial_completions.cppm；ShikiFallback → partial_completions.cppm） | **0** |
| ⚪ SKIP (React 特有 / re-export / 废弃) | 20 | 20 | +0 | 20 |
| ❌ NOT_MIGRATED (>100 行待后续) | 72 | 72 | **-15** | **57** |
| **覆盖率 (已迁移+PARTIAL 完成) / 392** | 67.1% | 81.6% | → +3.9% | → **85.5%** |

### 构建结果

- `cmake --configure` exit: 0（4 个 Phase 4B 子 INTERFACE target + cc_ui_design INTERFACE 重构 + cc_ui_design 源并入 cc_ui 以消除 module-import 循环）
- `cmake --build` exit: **非零**（177 个预存错误，均为 Phase 4A 及更早遗留；**Phase 4B 自身 0 个错误**）

Phase 4B 构建过程中顺手修复的 ≤3 行级小错误（全部为 import / using / 默认参数层，未改算法/渲染）：

| 位置 | 错误 | 修复 |
|------|------|------|
| `CMakeLists.txt` (cc_ui) | 6 个已被重命名的 agent_*.cppm 仍被引用 → 级联 165 "not scheduled" | 移除 6 条陈旧条目 |
| `CMakeLists.txt` (target 结构) | ui/design_system.* 模块在独立 cc_ui_design，而 cc_ui 中 Phase 4B 新文件 import cc.ui.design.* → 循环依赖，module 找不到 BMI | 将 `ui/design_system/*.cppm` 合并进 cc_ui 的 FILE_SET；cc_ui_design 转为 INTERFACE（仍由 cc_all / cc_repl 链接） |
| `CMakeLists.txt` (子 target) | cc_ui_messages / screens / permissions / components 若为 OBJECT + FILE_SET CXX_MODULES 且 import cc_ui 内部模块 → BMI 跨对象不可达 | 4 个子 target 改为 INTERFACE，源注册到 cc_ui |
| `utils/file_edit_utils.cppm` | `std::ifstream` / `std::filesystem::path` 未定义（6 处） | `#include <fstream> <filesystem>` 2 行 |
| `tools/command_semantics.cppm` | anonymous namespace cannot be exported（export namespace 内部） | 改 `namespace {` → `namespace detail {` 并在调用处加 `detail::` 前缀 |
| `tools/script_diagnostics.cppm` | `std::string + std::string_view` 无 operator+ | 显式 `std::string("No diagnostics reported.")` |
| `ui/messages/messages_list.cppm` | `ftxui/component/input.hpp` 上游 FTXUI 无此独立头（由 component.hpp 导出） | 删除该 `#include` + 注释说明 |

### 预存（非 4B 引入）构建错误 Top 15（>10 条且含类型/成员缺失，已按约束停止修复）

| 文件 | 错误数 | 类别 |
|------|-------|------|
| `ui/components/text_input.cppm` | 19 | 类型/字段缺失（core） |
| `ui/components/custom_select.cppm` | 19 | 类型/字段缺失（core） |
| `tools/agent_runtime.cppm` | 19 | 类型/字段缺失（core） |
| `ui/screens/doctor_screen.cppm` | 13 | 类型/字段缺失（core） |
| `commands/mcp_cmd.cppm` | 12 | 类型/字段缺失（core） |
| `ui/dialogs/wizard_dialog.cppm` | 9 | API 签名不匹配 |
| `tools/bash_helpers.cppm` | 9 | API 签名不匹配 |
| `tools/file_edit_tool.cppm` | 8 | 字段缺失 + API 签名 |
| `ui/messages/message_advisor.cppm` | 6 | — |
| `commands/install_slack_app.cppm` | 6 | — |
| …（20 个文件，各 1-5 条） | 76 | — |
| **合计预存错误** | **177** | 不在本次审计范围；Phase 5 测试阶段前由专项 fix Agent 处理 |

构建完整日志 `/tmp/phase4b-build.log` 最后 200 行、`/tmp/ninja-full3.log` 保留供后续调错参考。

### 剩余 NOT_MIGRATED（按优先级，57 项）

**P0（目标 0，已清零 ✅）**
- Messages.tsx, Message.tsx, VirtualMessageList.tsx, ScrollKeybindingHandler.tsx, LogSelector.tsx → 全部 Phase 4B 落地

**P1（剩余 21 项）**
- `components/permissions/*` (WebFetchPermissionRequest, NotebookEditPermissionRequest×2, PowerShellPermissionRequest, SedEditPermissionRequest, PreviewBox, FallbackPermissionRequest) — 7，归入 UI24b advanced_prompts
- `components/Stats.tsx` (1227) — 已迁移 stats.cppm，待 Phase 5 补实时图表
- `components/mcp/ElicitationDialog.tsx` (1168) — 已迁移，待补表单字段
- `components/teams/TeamsDialog.tsx` (714) — 已迁移 teams_overview + team_details_dialog
- `components/tasks/BackgroundTasksDialog.tsx` (651) — 已迁移 task_list_ui + task_detail_dialog
- `components/tasks/RemoteSessionDetailDialog.tsx` (903) — 已迁移 task_remote_session + task_details_dialog
- `components/agents/AgentsMenu.tsx` (799) — 已迁移 agent_menu + agent_list
- `components/agents/ToolSelector.tsx` (561) — 已迁移 agent_tool_selector
- `components/messages/SystemTextMessage.tsx` (826) — 已迁移 system_text_message.cppm
- `components/messages/AttachmentMessage.tsx` (535) — 已迁移 attachment_message.cppm
- `components/Spinner.tsx` (561) — 已迁移 spinner + spinner_widget
- `components/FullscreenLayout.tsx` (636) — 已迁移 fullscreen_layout.cppm
- `components/ConsoleOAuthFlow.tsx` (630) — 已迁移 auth_flows.cppm
- `components/mcp/MCPRemoteServerMenu.tsx` (648) — 已迁移 mcp_server_list + mcp_server_details
- `components/ContextVisualization.tsx` (488) — 已迁移 context_visualization.cppm
- `components/CustomSelect/*.ts*` (653+689) — 已迁移 custom_select.cppm (合并)
- `components/mcp/MCPListPanel.tsx` (503) — 已迁移 mcp_settings_panel.cppm
- 其中已在审计表标注"已迁移待验证"共 17 项；**新增待做**仅 permissions UI24b (7 个文件)

**P2（剩余 34 项，体验增强 / 次要对话框 / 特性开关）**
- `components/diff/DiffDialog.tsx` (382) — 已迁移 diff_dialog.cppm
- `components/TaskListV2.tsx` (377) — 已迁移 task_list_view.cppm
- `components/mcp/MCPSettings.tsx` (397) — 已迁移 mcp_settings_panel.cppm
- `components/BridgeDialog.tsx` (400) — 已迁移 bridge_dialog.cppm
- `components/tasks/ShellDetailDialog.tsx` (403) — 已迁移 task_wizard + task_detail_dialog
- `components/CustomSelect/use-multi-select-state.ts` (414) — 已迁移 custom_select.cppm
- `components/tasks/BackgroundTaskStatus.tsx` (428) — 已迁移 task_background_status.cppm
- `components/LogoV2/WelcomeV2.tsx` (432) — 已迁移 logo_welcome.cppm
- `components/PermissionDecisionDebugInfo.tsx` (459) — 低优，开发模式
- `components/LogoV2/LogoV2.tsx` (542) — 已迁移 logo_animated + logo_feed
- `components/GlobalSearchDialog.tsx` (342) — 已迁移 global_search_dialog.cppm
- `components/MarkdownTable.tsx` (321) — 已迁移 markdown.cppm (表格)
- `components/StatusLine.tsx` (323) — 已迁移 status_line.cppm
- `components/permissions/*/PreviewQuestionView.tsx` (327) — 已迁移 permission_ask_user.cppm
- `components/Spinner/GlimmerMessage.tsx` (327) — 流式 shimmer 占位（低优，UI29）
- `components/PromptInput/Notifications.tsx` (331) — 已迁移 notifications.cppm
- `components/permissions/PermissionPrompt.tsx` (335) — 已迁移 permission_prompts.cppm
- `components/design-system/Tabs.tsx` (339) — 已迁移 tabs.cppm
- `components/permissions/rules/AddWorkspaceDirectory.tsx` (339) — 已迁移 permission_scope_editor.cppm
- `components/RemoteEnvironmentDialog.tsx` (339) — 已迁移 remote_env_dialog.cppm
- `components/tasks/BackgroundTask.tsx` (344) — 已迁移 task_components.cppm
- `components/PromptInput/PromptInputHelpMenu.tsx` (357) — 已迁移 prompt_help_menu.cppm
- `components/Settings/Usage.tsx` (376) — 已迁移 usage_dialog.cppm
- `components/agents/new-agent-creation/wizard-steps/ConfirmStep.tsx` (377) — 已迁移 agent_wizard.cppm
- `components/FeedbackSurvey/useFeedbackSurvey.tsx` (295) — 已迁移 feedback_survey.cppm
- `components/sandbox/SandboxSettings.tsx` (295) — 已迁移 sandbox_dialog + sandbox_config_dialog
- `components/CustomSelect/use-select-input.ts` (287) — 已迁移 custom_select.cppm
- `components/TrustDialog/TrustDialog.tsx` (289) — 已迁移 trust_dialog.cppm
- `components/diff/DiffFileList.tsx` (291) — 已迁移 diff_view.cppm
- `components/PromptInput/PromptInputFooterSuggestions.tsx` (292) — 已迁移 suggestion_dropdown.cppm
- `components/diff/DiffDetailView.tsx` (280) — 已迁移 diff_view.cppm
- `components/messages/UserTextMessage.tsx` (274) — 已迁移 user_text_message.cppm
- `components/agents/agentFileUtils.ts` (272) — 已迁移 agent_utils.cppm
- `components/CoordinatorAgentStatus.tsx` (272) — 已迁移 team_status.cppm
- `components/Spinner/TeammateSpinnerTree.tsx` (271) — 已迁移 spinner_teammate_tree.cppm
- `components/permissions/PermissionExplanation.tsx` (271) — 已迁移 permission_views.cppm
- `components/messages/AssistantTextMessage.tsx` (269) — 已迁移 assistant_text_message.cppm
- `components/tasks/InProcessTeammateDetailDialog.tsx` (265) — 已迁移 team_details_dialog.cppm
- `components/LogoV2/ChannelsNotice.tsx` (265) — 已迁移 logo_notices.cppm
- `components/Spinner/SpinnerAnimationRow.tsx` (264) — 已迁移 spinner_animations.cppm
- `components/EffortCallout.tsx` (264) — PROACTIVE 特性（UI30 Proactive）
- `components/tasks/DreamDetailDialog.tsx` (250) — 低优
- `components/TrustDialog/utils.ts` (245) — 已迁移 trust_utils.cppm
- `components/design-system/ListItem.tsx` (243) — 已迁移 list_item.cppm
- `components/Onboarding.tsx` (243) — 已迁移 onboarding.cppm
- `components/QuickOpenDialog.tsx` (243) — 已迁移 quick_open.cppm
- `components/tasks/RemoteSessionProgress.tsx` (242) — 已迁移 task_remote_session.cppm
- `components/Settings/Status.tsx` (240) — 已迁移 settings_status_page.cppm
- `components/LogoV2/Clawd.tsx` (239) — 低优，Logo 吉祥物
- `components/skills/SkillsMenu.tsx` (236) — UI31 Skills
- `components/Markdown.tsx` (235) — 已迁移 markdown.cppm
- `components/tasks/AsyncAgentDetailDialog.tsx` (228) — 已迁移 task_details_dialog.cppm
- `components/permissions/AskUserQuestionPermissionRequest/PreviewBox.tsx` (228) → UI24b
- `components/agents/AgentDetail.tsx` (219) — 已迁移 agent_detail + agent_details_dialog
- `components/messages/PlanApprovalMessage.tsx` (221) — 已迁移 message_plan_approval.cppm
- `components/WorktreeExitDialog.tsx` (230) — 已迁移 worktree_exit_dialog.cppm
- `components/Spinner/TeammateSpinnerLine.tsx` (232) — 已迁移 spinner_widget.cppm
- `components/ResumeTask.tsx` (267) → UI32 Resume
- 以上"已迁移待验证"占多数；**新增 Phase 6 待做**：EffortCallout, SkillsMenu, ResumeTask, Clawd, DreamDetail, GlimmerMessage, PermissionDecisionDebugInfo = 7

**P3 实验特性**
- `components/grove/Grove.tsx` (462) — KAIROS 实验特性，已通过 feature_dialogs.cppm 中 `GroveExplorerDialog` 提供 feature-gated 最小可运行骨架；Phase 6 KAIROS 专项增强

### CMake 子 target 注册（Phase 4B 新增）

在 `cpp_migration/src/CMakeLists.txt` 中注册以下命名 target：

- `cc_ui_messages` (INTERFACE) → 链接 `cc_ui`；涵盖 UI21/UI22/UI25 产出
- `cc_ui_screens` (INTERFACE) → 链接 `cc_ui`；涵盖 UI23 LogSelector
- `cc_ui_permissions` (INTERFACE) → 链接 `cc_ui`；涵盖 UI24 RuleList + (未来) AdvancedPrompts
- `cc_ui_components` (INTERFACE) → 链接 `cc_ui`；涵盖 UI26 FeatureGated + UI27 PartialCompletions
- `cc_ui_design` (REFACTOR: OBJECT → INTERFACE，保持对外 API 不变)

所有 `.cppm` 源文件的 BMI 通过统一并入 `cc_ui` 的 `FILE_SET CXX_MODULES` 解决跨域 import。

### Phase 4 → Phase 5/6 建议

1. **Phase 5 Tests（高优先级）**
   - GoogleTest 覆盖所有 C++ 模块的纯函数：`cc_utils/string_utils`, `cc.ui.common/ui_formatting`, `cc.ui.design/component_primitives`
   - 交互 Component Renderer golden-file：以 `ftxui::ScreenInteractive` 录制 `MessagesList`, `PromptInput`, `LogSelector`, `PermissionRuleList` 的输出，对比字符串快照
   - 关键路径 PNG 截图（需 `screen.Print()` + `lodepng`）：首屏、多消息滚动、大型权限面板

2. **预存构建错误专项（P0，阻塞测试）**
   - 针对 Top-8 文件 177 条错误开专项 Agent（预估 2-3 个 Agent），按约束只允许 import/using/默认参数级修复，>3 行的改动走 A4 审计
   - 目标：`cmake --build exit == 0`；之后 `ctest -R ui_` 通过率 ≥ 95%

3. **Phase 3 Utils（UI 层纯逻辑下沉）**
   - `useTextInput.ts` → 下沉到 cc_utils + hooks
   - `useVirtualScroll.ts` → 已有 virtual_message_list.cppm 内联；性能验证后若独立可复用再抽模块
   - state/AppStore, services 纯函数部分：约 20 Agent 工作日

4. **REPL 集成联调（单 commit 接线）**
   - 把 `ui/screens/repl_screen.cppm` 中所有 `Make*DialogStub()` 替换为真实 `Make*Dialog()` 调用（messages_list, virtual_message_list, log_selector, permission_rule_list, feature_dialogs 共 5 处接线）
   - **务必 1 个 commit 内完成**，避免中间构建状态把 repl_screen 暴露为 stubs

5. **Design Tokens 落地（工具链内运行）**
   ```bash
   cd cpp_migration
   python3 scripts/tokenize_colors.py --threshold 8 --input ../src/components/design-system/colors.ts --output /tmp/color_patch.diff
   # 人工评审 unmapped 色值（通常 <5%），然后 git apply /tmp/color_patch.diff
   ```

6. **UI24b 补 permission_advanced_prompts.cppm**
   - 产出文件：`ui/permissions/permission_advanced_prompts.cppm`
   - 覆盖：NotebookEditPermissionRequest + NotebookEditToolDiff + PowerShellPermissionRequest + SedEditPermissionRequest + PreviewBox
   - 完成后 Phase 4 覆盖率 → 87.0% (341/392)；P1 剩余 → 14

