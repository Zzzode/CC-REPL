/// @file command_registry_init_d.cpp
/// @brief Group D registration: system commands (login, logout, permissions, plugin, voice, etc.)
module cc.commands.registry;

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
import cc.commands.keybindings_cmd;
import cc.commands.voice;

namespace cc::commands {

void register_group_d_commands(CommandRegistry& registry) {
    registry.register_command<LoginCommand>();
    registry.register_command<LogoutCommand>();
    registry.register_command<PermissionsCommand>();
    registry.register_command<PluginCommand>();
    registry.register_command<UsageCommand>();
    registry.register_command<BranchCommand>();
    registry.register_command<ChromeCommand>();
    registry.register_command<CopyCommand>();
    registry.register_command<DesktopCommand>();
    registry.register_command<ExportCommand>();
    registry.register_command<GoodClaudeCommand>();
    registry.register_command<InstallSlackAppCommand>();
    registry.register_command<KeybindingsCommand>();
    registry.register_command<MobileCommand>();
    registry.register_command<StickersCommand>();
    registry.register_command<TasksCommand>();
    registry.register_command<SkillsCommand>();
    registry.register_command<VoiceCommand>();
}

} // namespace cc::commands