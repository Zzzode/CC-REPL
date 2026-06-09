/// @file command_registry_init.cpp
/// @brief Module implementation unit for `cc.commands.registry`.
///
/// This is the *implementation* translation unit for the
/// `cc.commands.registry` named module. It imports every concrete command
/// module and provides the body of `register_default_commands`.
///
/// Keeping the per-command fan-out here (rather than inside the primary
/// module interface unit `command_registry.cppm`) dramatically reduces the
/// SourceLocation footprint of the `cc.commands.registry` BMI for
/// downstream consumers (main.cpp / tests), which would otherwise blow
/// past clang's 31-bit SLOC budget.

module cc.commands.registry;

import cc.commands.commit;
import cc.commands.review;
import cc.commands.config;
import cc.commands.context;
import cc.commands.diff;
import cc.commands.mcp_cmd;
import cc.commands.compact;
import cc.commands.help;
import cc.commands.doctor;
import cc.commands.clear;
import cc.commands.add_dir;
import cc.commands.agents;
import cc.commands.btw;
import cc.commands.advisor;
import cc.commands.bridge_kick;
import cc.commands.brief;
import cc.commands.color;
import cc.commands.ctx_viz;
import cc.commands.effort;
import cc.commands.env;
import cc.commands.fast;
import cc.commands.feedback;
import cc.commands.files;
import cc.commands.heapdump;
import cc.commands.hooks;
import cc.commands.ide;
import cc.commands.issue;
import cc.commands.memory;
import cc.commands.passes;
import cc.commands.rename;
import cc.commands.rewind;
import cc.commands.share;
import cc.commands.stats;
import cc.commands.status;
import cc.commands.summary;
import cc.commands.tag;
import cc.commands.teleport;
import cc.commands.upgrade;
import cc.commands.ultraplan;
import cc.commands.review.ultrareview;
import cc.commands.review.review_remote;
import cc.commands.security_review;
import cc.commands.init_verifiers;
import cc.commands.install;
import cc.commands.install_github_app;
import cc.commands.insights;
import cc.commands.init;
import cc.commands.session;
import cc.commands.resume;
import cc.commands.model;
import cc.commands.cost;
import cc.commands.plan;
import cc.commands.theme;
import cc.commands.vim;
import cc.commands.login;
import cc.commands.logout;
import cc.commands.permissions_cmd;
import cc.commands.plugin_cmd;
import cc.commands.usage;
import cc.commands.branch;
import cc.commands.chrome;
import cc.commands.copy_cmd;
import cc.commands.desktop;
import cc.commands.export_cmd;
import cc.commands.good_claude;
import cc.commands.install_slack_app;
import cc.commands.mobile;
import cc.commands.stickers;
import cc.commands.tasks_cmd;
import cc.commands.skills_cmd;
import cc.commands.voice;
import cc.commands.runtime_surface_commands;

namespace cc::commands {

void register_default_commands(CommandRegistry& registry) {
    registry.register_command<CommitCommand>();
    registry.register_command<ReviewCommand>();
    registry.register_command<ReviewRemoteCommand>();
    registry.register_command<UltraReviewCommand>();
    registry.register_command<ConfigCommand>();
    registry.register_command<McpCommand>();
    registry.register_command<CompactCommand>();
    registry.register_command<HelpCommand>();
    registry.register_command<DoctorCommand>();
    registry.register_command<ClearCommand>();
    registry.register_command<AddDirCommand>();
    registry.register_command<AgentsCommand>();
    registry.register_command<BtwCommand>();
    registry.register_command<AdvisorCommand>();
    registry.register_command<BridgeKickCommand>();
    registry.register_command<BriefCommand>();
    registry.register_command<ColorCommand>();
    registry.register_command<CtxVizCommand>();
    registry.register_command<EffortCommand>();
    registry.register_command<EnvCommand>();
    registry.register_command<FastCommand>();
    registry.register_command<FeedbackCommand>();
    registry.register_command<FilesCommand>();
    registry.register_command<HeapdumpCommand>();
    registry.register_command<HooksCommand>();
    registry.register_command<IdeCommand>();
    registry.register_command<IssueCommand>();
    registry.register_command<MemoryCommand>();
    registry.register_command<PassesCommand>();
    registry.register_command<RenameCommand>();
    registry.register_command<RewindCommand>();
    registry.register_command<ShareCommand>();
    registry.register_command<StatsCommand>();
    registry.register_command<StatusCommand>();
    registry.register_command<SummaryCommand>();
    registry.register_command<TagCommand>();
    registry.register_command<TeleportCommand>();
    registry.register_command<UpgradeCommand>();
    registry.register_command<UltraplanCommand>();
    registry.register_command<InstallCommand>();
    registry.register_command<InsightsCommand>();
    registry.register_command<InitCommand>();
    registry.register_command<ContextCommand>();
    registry.register_command<DiffCommand>();
    registry.register_command<SessionCommand>();
    registry.register_command<ResumeCommand>();
    registry.register_command<ModelCommand>();
    registry.register_command<CostCommand>();
    registry.register_command<PlanCommand>();
    registry.register_command<ThemeCommand>();
    registry.register_command<VimCommand>();
    registry.register_command<LoginCommand>();
    registry.register_command<LogoutCommand>();
    registry.register_command<PermissionsCommand>();
    registry.register_command<PluginCommand>();
    registry.register_command<UsageCommand>();
    registry.register_command<BranchCommand>();
    registry.register_command<ChromeCommand>();
    registry.register_command<CommitPushPrCommand>();
    registry.register_command<CopyCommand>();
    registry.register_command<DesktopCommand>();
    registry.register_command<ExportCommand>();
    registry.register_command<GoodClaudeCommand>();
    registry.register_command<InstallSlackAppCommand>();
    registry.register_command<KeybindingsCommand>();
    registry.register_command<MobileCommand>();
    registry.register_command<OutputStyleCommand>();
    registry.register_command<PrivacySettingsCommand>();
    registry.register_command<ReleaseNotesCommand>();
    registry.register_command<SandboxToggleCommand>();
    registry.register_command<SecurityReviewCommand>();
    registry.register_command<StickersCommand>();
    registry.register_command<TasksCommand>();
    registry.register_command<SkillsCommand>();
    registry.register_command<ThinkbackCommand>();
    registry.register_command<VoiceCommand>();
    registry.register_command<AntTraceCommand>();
    registry.register_command<AutofixPrCommand>();
    registry.register_command<BackfillSessionsCommand>();
    registry.register_command<BreakCacheCommand>();
    registry.register_command<BridgeCommand>();
    registry.register_command<BughunterCommand>();
    registry.register_command<CreateMovedToPluginCommand>();
    registry.register_command<DebugToolCallCommand>();
    registry.register_command<ExitCommand>();
    registry.register_command<ExtraUsageCommand>();
    registry.register_command<InitVerifiersCommand>();
    registry.register_command<InstallGithubAppCommand>();
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
