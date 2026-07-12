/// @file command_registry_init_c.cpp
/// @brief Group C registration: session/model/plan commands (model, cost, plan, insights, etc.)
module cc.commands.registry;

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

namespace cc::commands {

void register_group_c_commands(CommandRegistry& registry) {
    registry.register_command<TeleportCommand>();
    registry.register_command<UpgradeCommand>();
    registry.register_command<UltraplanCommand>();
    registry.register_command<UltraReviewCommand>();
    registry.register_command<ReviewRemoteCommand>();
    registry.register_command<SecurityReviewCommand>();
    registry.register_command<InitVerifiersCommand>();
    registry.register_command<InstallCommand>();
    registry.register_command<InstallGithubAppCommand>();
    registry.register_command<InsightsCommand>();
    registry.register_command<InitCommand>();
    registry.register_command<SessionCommand>();
    registry.register_command<ResumeCommand>();
    registry.register_command<ModelCommand>();
    registry.register_command<CostCommand>();
    registry.register_command<PlanCommand>();
    registry.register_command<ThemeCommand>();
    registry.register_command<VimCommand>();
}

} // namespace cc::commands