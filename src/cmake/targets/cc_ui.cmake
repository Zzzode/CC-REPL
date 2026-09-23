# ─── cc_ui: Terminal UI ───────────────────────────────────────────────────────
add_library(cc_ui)
target_sources(cc_ui
    PUBLIC FILE_SET CXX_MODULES FILES
        ui/app/app.cppm
        ui/app/app_impl.cppm
        ui/prompt/autocomplete_sources.cppm
        ui/app/app_dialog_registration.cppm
        ui/widgets/components.cppm
        ui/features/agents/agent_cards.cppm
        ui/features/agents/agent_details_dialog.cppm
        ui/features/agents/agent_editor.cppm
        ui/features/agents/agent_list.cppm
        ui/features/agents/agent_shared_widgets.cppm
        ui/features/agents/agent_wizard.cppm
        ui/widgets/all_components.cppm
        ui/features/agents/agent_view.cppm
        ui/visual/code_highlight.cppm
        ui/widgets/custom_select.cppm
        ui/widgets/dev_bar.cppm
        ui/visual/diff_view.cppm
        ui/widgets/fast_icon.cppm
        ui/dialogs/feature_dialogs.cppm
        ui/foundation/components_figures.cppm
        ui/features/grove.cppm
        ui/features/plugins/lsp_recommendation_menu.cppm
        ui/widgets/partial_completions.cppm   # UI27 — TreeSelect + ShikiFallback (PARTIAL -> DONE)
        ui/widgets/passes.cppm
        ui/features/plugins/plugin_hint_menu.cppm
        ui/widgets/pr_badge.cppm
        ui/widgets/spinner.cppm
        ui/widgets/spinner_widget.cppm
        ui/widgets/stats.cppm
        ui/visual/structured_diff.cppm
        ui/visual/file_edit_tool_diff.cppm
        ui/widgets/tag_tabs.cppm
        ui/features/tasks/task_view.cppm
        ui/widgets/text_input.cppm
        ui/widgets/text_input_widget.cppm
        ui/messages/user_bash_input_message.cppm
        ui/messages/shell_progress_message.cppm
        ui/messages/shell_time_display.cppm
        ui/widgets/spinner_animations.cppm
        ui/foundation/ui_types.cppm
        ui/foundation/ui_formatting.cppm
        ui/foundation/declared_cursor.cppm
        ui/foundation/dialog.cppm
        ui/foundation/divider.cppm
        ui/foundation/list_item.cppm
        ui/foundation/progress_bar.cppm
        ui/foundation/status_icon.cppm
        ui/foundation/tabs.cppm
        ui/foundation/themed_box.cppm
        ui/foundation/themed_text.cppm
        ui/dialogs/dialog_system.cppm
        ui/dialogs/dialog_frame.cppm
        ui/dialogs/dialog_launchers.cppm
        ui/dialogs/dialog_default_renderers.cppm
        ui/dialogs/hooks_dialog_renderer.cppm
        ui/dialogs/diff_dialog.cppm
        ui/dialogs/elicitation_dialog.cppm
        ui/dialogs/feedback_survey.cppm
        ui/dialogs/help_view.cppm
        ui/dialogs/settings_view.cppm
        ui/dialogs/about_dialog.cppm
        ui/dialogs/confirmation_dialog.cppm
        ui/dialogs/ide_dialogs.cppm
        ui/dialogs/managed_settings_security.cppm
        ui/dialogs/mcp_dialog.cppm
        ui/dialogs/mcp_dialogs.cppm
        ui/dialogs/output_style_picker.cppm
        ui/dialogs/permission_dialog.cppm
        ui/dialogs/permission_prompts.cppm
        ui/dialogs/plugin_dialog.cppm
        ui/dialogs/plugin_dialog_renderer.cppm
        ui/dialogs/prompt_dialog.cppm
        ui/dialogs/quick_open.cppm
        ui/dialogs/sandbox_dialog.cppm
        ui/dialogs/sandbox_settings.cppm        # new: SandboxSettings faithful port (tabs + doctor)
        ui/dialogs/settings_dialog.cppm
        ui/dialogs/settings_status_page.cppm
        ui/dialogs/usage_dialog.cppm
        ui/dialogs/trust_dialog.cppm
        ui/dialogs/trust_utils.cppm
        ui/dialogs/wizard_dialog.cppm
        ui/dialogs/bridge_dialog.cppm
        ui/dialogs/cost_threshold_dialog.cppm
        ui/dialogs/global_search_dialog.cppm
        ui/dialogs/idle_return_dialog.cppm
        ui/dialogs/all_renderers.cppm            # new: DialogRendererRegistry + all renderers
        ui/dialogs/bottom_renderers.cppm         # new: Bottom-band dialog renderers
        ui/dialogs/modal_renderers.cppm          # new: Overlay/Modal dialog renderers
        ui/dialogs/sandbox_permission.cppm       # new: SandboxPermission renderer
        ui/dialogs/triggers.cppm                 # new: Dialog trigger / invocation routing
        # M8 note: ManagedSettingsSecurity / FeedbackSurvey / GlobalSearch /
        # HistorySearch / PluginDialog / DiffDialog — FTXUI chrome not yet ported.
        # Do NOT add placeholder sources into FILE_SET CXX_MODULES: CMake 4.x
        # cascades "not scheduled for compilation" diagnostics across the
        # entire target if any listed source is missing.  Only add module
        # sources that exist AND contain real implementations.  Registration
        # lives in register_default_renderers() in default_renderers.cppm.
        ui/chrome/layout.cppm
        ui/chrome/fullscreen_layout.cppm
        ui/foundation/logo.cppm
        ui/foundation/logo_v2.cppm          # P0-4: LogoV2 3-mode dispatch + WelcomeV2 + notice stack
        ui/chrome/yoga.cppm
        ui/visual/markdown.cppm
        ui/messages/messages.cppm
        ui/messages/assistant_message.cppm
        ui/messages/error_message.cppm
        ui/messages/message_components.cppm
        ui/messages/message_response.cppm
        ui/messages/message_row.cppm
        ui/messages/message_timestamp.cppm
        ui/messages/message_advisor.cppm
        ui/messages/message_bash_io.cppm
        ui/messages/message_channel.cppm
        ui/messages/message_hook_progress.cppm
        ui/messages/message_plan_approval.cppm
        ui/messages/message_rate_limit.cppm
        ui/messages/message_task_assignment.cppm
        ui/messages/message_tool_result.cppm
        ui/messages/thinking_message.cppm
        ui/messages/tool_messages.cppm
        ui/messages/tool_use_message.cppm
        ui/messages/user_message.cppm
        ui/messages/message_image.cppm
        ui/messages/message_compact_boundary.cppm
        ui/messages/message_shutdown.cppm
        ui/messages/message_user_command.cppm
        ui/messages/user_text_message.cppm
        ui/messages/assistant_text_message.cppm
        ui/messages/system_text_message.cppm
        ui/messages/attachment_message.cppm
        ui/messages/api_error_message.cppm
        ui/messages/collapsed_content_message.cppm
        ui/messages/local_command_output_message.cppm
        ui/messages/messages_list.cppm            # UI21 — Messages.tsx + Message.tsx (834+626 → 1308 loc)
        ui/messages/messages_interactions.cppm    # UI25 — MessageSelector + messageActions
        ui/messages/message_pipeline.cppm         # P0-2 — 7-stage message pipeline (dedup/tag-filter/augment/hide/index)
        ui/messages/collapse_background_bash.cppm  # P0-2 — collapseBackgroundBashNotifications pass (Messages.tsx:520)
        ui/messages/scroll_keybindings.cppm       # UI22 — ScrollKeybindingHandler (1011 → 752 loc)
        ui/messages/virtual_message_list.cppm     # UI22 — VirtualScroll (1081 → 857 loc)
        ui/chrome/panels.cppm
        ui/permissions/permission_request.cppm
        ui/permissions/permission_rules.cppm
        ui/permissions/permission_views.cppm
        ui/permissions/permission_bash.cppm
        ui/permissions/permission_computer_use.cppm
        ui/permissions/permission_file_edit.cppm
        ui/permissions/permission_file_write.cppm
        ui/permissions/permission_rules_ui.cppm
        ui/permissions/permission_shell_helpers.cppm
        ui/permissions/permission_worker_badge.cppm
        ui/permissions/permissions_components.cppm
        ui/permissions/permission_scope_editor.cppm
        ui/permissions/permission_rule_list.cppm        # UI24 — PermissionRuleList
        ui/permissions/permission_advanced_prompts.cppm # UI24b — AskUserQuestion+Skill+Fallback advanced prompts
        ui/permissions/permission_single_prompt.cppm
        ui/permissions/permission_batch_panel.cppm
        ui/permissions/sandbox_config_dialog.cppm
        ui/prompt/at_attachments.cppm
        ui/prompt/file_index.cppm
        ui/prompt/fuzzy_rank_nucleo.cppm
        ui/prompt/mode_indicator.cppm  # P0: 3-way prefix glyph (❯/!/agent-tint) TS PromptInputModeIndicator
        ui/prompt/notifications.cppm
        ui/prompt/prompt_input_footer.cppm  # M5 — faithful TS PromptInputFooter port
        ui/prompt/prompt_input_full.cppm
        ui/prompt/prompt_paste_handler.cppm
        ui/prompt/prompt_queued_commands.cppm
        ui/prompt/prompt_stash_notice.cppm
        ui/prompt/combined_highlights.cppm  # P1: 8-tier combined highlights builder
        ui/prompt/placeholder_cascade.cppm  # P1: 4-tier memoized placeholder cascade
        ui/prompt/vim_input.cppm
        ui/prompt/voice_indicator.cppm
        ui/prompt/prompt_input.cppm
        ui/features/plugins/plugin_install_flow.cppm
        ui/features/plugins/plugin_manage_panel.cppm
        ui/features/plugins/plugin_marketplace_browse.cppm
        ui/features/plugins/plugin_settings_dialog.cppm
        ui/features/mcp/mcp_add_server_wizard.cppm
        ui/features/mcp/mcp_security_dialog.cppm
        ui/features/mcp/mcp_server_details.cppm
        ui/features/mcp/mcp_server_list.cppm
        ui/features/mcp/mcp_elicitation.cppm
        ui/features/mcp/mcp_settings_panel.cppm
        ui/chrome/ink_utils.cppm
        ui/chrome/renderer.cppm
        ui/chrome/text_measure.cppm
        ui/features/tasks/task_components.cppm
        ui/features/tasks/task_details_dialog.cppm
        ui/features/tasks/task_list_view.cppm
        ui/features/tasks/task_wizard.cppm
        ui/features/tasks/task_list_ui.cppm
        ui/tools/tool_ui_registry.cppm
        ui/tools/tool_ui_init.cppm
        ui/tools/tool_ui_generic.cppm
        ui/tools/tool_ui_bash.cppm
        ui/tools/tool_ui_file_edit.cppm
        ui/tools/tool_ui_file_write.cppm
        ui/tools/tool_ui_file_read.cppm
        ui/tools/tool_ui_grep.cppm
        ui/tools/tool_ui_glob.cppm
        ui/tools/tool_ui_web_fetch.cppm
        ui/tools/tool_ui_web_search.cppm
        ui/tools/tool_ui_skill.cppm
        ui/tools/tool_ui_agent.cppm
        ui/tools/tool_ui_task.cppm
        ui/tools/tool_ui_mcp.cppm
        ui/tools/tool_ui_lsp.cppm
        ui/tools/tool_ui_longtail.cppm
        ui/features/teams/teams_overview.cppm
        ui/features/teams/team_details_dialog.cppm
        ui/features/teams/swarm_collaboration_view.cppm
        ui/features/teams/team_status.cppm
        ui/features/teams/live_teammates.cppm
        ui/chrome/terminal_io.cppm
        ui/features/hooks_ui.cppm
        ui/screens/doctor_screen.cppm
        ui/screens/repl_screen.cppm
        ui/screens/resume_screen.cppm
        ui/screens/log_selector.cppm              # UI23 — LogSelector (1574 → 1730 loc)
        ui/chrome/terminal.cppm
        ui/foundation/design_tokens.cppm       # UI20 (merged from cc_ui_design to resolve module-import cycle)
        ui/foundation/design_figures.cppm             # Shared glyph + prompt-mode constants (TS utils/figures.ts + inputModes.ts)
        ui/foundation/theme_provider.cppm
        ui/foundation/design_logo.cppm
        ui/foundation/component_primitives.cppm
        ui/foundation/design_extras.cppm       # UI27 — fuzzy picker + theme picker + design extras
)
# Module implementation units for cc.ui.app_dialog_registration — one per
# dialog-renderer aggregator, so no single TU imports more than one aggregator's
# closure (importing all four at once crashes Clang codegen). This keeps the
# ~44 dialog implementations out of app.cppm's BMI (source-location budget).
# See app_dialog_registration.cppm / *_impl.cpp for the rationale.
target_sources(cc_ui PRIVATE
    ui/app/app_autocomplete.cpp
    ui/app/app_extra_methods.cpp
    ui/app/app_constructor.cpp
    ui/app/app_handle_submit.cpp
    ui/app/app_agent_menu.cpp
    ui/prompt/autocomplete_sources_impl.cpp
    ui/app/app_prompt_suggestion_wiring.cpp
    ui/prompt/at_attachments_impl.cpp
    ui/app/app_dialog_registration_default.cpp
    ui/app/app_dialog_registration_modal.cpp
    ui/app/app_dialog_registration_bottom.cpp
    ui/app/app_dialog_registration_all.cpp
    ui/app/app_dialog_registration_hooks.cpp
    ui/app/app_dialog_registration_teams.cpp
    ui/app/app_team_projection.cpp
    ui/app/app_message_projection.cpp
    ui/app/app_store_bridge.cpp
    ui/dialogs/hooks_dialog_renderer_impl.cpp
    ui/dialogs/plugin_dialog_renderer_impl.cpp
)
target_link_libraries(cc_ui
    PUBLIC
        cc_utils
        cc_types
        cc_query
        cc_commands
        cc_vim
        cc_hooks
        cc_plugins
        cc_session
        cc_history
        cc_skills            # SkillRegistry::on_skills_changed for dynamic skill refresh
        # U1: cc.ui.prompt.suggestion_provider imports cc.services.prompt_suggestion
        # directly. cc_ui already consumed cc.services.* transitively via cc_hooks;
        # making the dependency explicit is correct (no cycle: cc_services never
        # imports cc.ui.*).
        cc_services
        ftxui::screen
        ftxui::dom
        ftxui::component
)
