/// @file command_registry_init_e.cpp
/// @brief Group E registration: runtime-surface commands (transitively imports 30+ modules).
///
/// This file is kept separate because `cc.commands.runtime_surface_commands`
/// imports 30+ sub-command modules in its interface (ant_trace, autofix_pr,
/// backfill_sessions, break_cache, bridge, bughunter, commit_push_pr,
/// create_moved_to_plugin_command, debug_tool_call, exit, extra_usage,
/// init_verifiers, install_github_app sub-modules, keybindings_cmd,
/// mock_limits, oauth_refresh, onboarding, output_style, perf_issue,
/// pr_comments, privacy_settings, rate_limit_options, release_notes,
/// reload_plugins, remote_env, remote_setup, reset_limits, sandbox_toggle,
/// security_review, statusline, terminal_setup, thinkback, thinkback_play,
/// version).  Loading all of those BMIs in a single translation unit would
/// blow past clang's 31-bit SourceLocation budget; keeping them in their
/// own impl unit bounds the per-TU footprint.
module cc.commands.registry;

import cc.commands.runtime_surface_commands;

namespace cc::commands {

void register_group_e_commands(CommandRegistry& registry) {
    // Commands whose modules are transitively imported via runtime_surface_commands
    registry.register_command<OutputStyleCommand>();
    registry.register_command<PrivacySettingsCommand>();
    registry.register_command<ReleaseNotesCommand>();
    registry.register_command<SandboxToggleCommand>();
    registry.register_command<ThinkbackCommand>();
    registry.register_command<AntTraceCommand>();
    registry.register_command<AutofixPrCommand>();
    registry.register_command<BackfillSessionsCommand>();
    registry.register_command<BreakCacheCommand>();
    registry.register_command<BridgeCommand>();
    registry.register_command<BughunterCommand>();
    registry.register_command<CommitPushPrCommand>();
    registry.register_command<CreateMovedToPluginCommand>();
    registry.register_command<DebugToolCallCommand>();
    registry.register_command<ExitCommand>();
    registry.register_command<ExtraUsageCommand>();
    registry.register_command<MockLimitsCommand>();
    registry.register_command<OauthRefreshCommand>();
    registry.register_command<OnboardingCommand>();
    registry.register_command<PerfIssueCommand>();
    registry.register_command<PrCommentsCommand>();
    registry.register_command<RateLimitOptionsCommand>();
    registry.register_command<ReloadPluginsCommand>();
    registry.register_command<RemoteEnvCommand>();
    registry.register_command<RemoteSetupCommand>();
    registry.register_command<ResetLimitsCommand>();
    registry.register_command<StatuslineCommand>();
    registry.register_command<TerminalSetupCommand>();
    registry.register_command<ThinkbackPlayCommand>();
    registry.register_command<VersionCommand>();
}

} // namespace cc::commands