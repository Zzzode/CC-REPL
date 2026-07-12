/// @file command_registry_init_a.cpp
/// @brief Group A registration: core commands (commit, review, config, help, etc.)
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

namespace cc::commands {

void register_group_a_commands(CommandRegistry& registry) {
    registry.register_command<CommitCommand>();
    registry.register_command<ReviewCommand>();
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
    registry.register_command<ContextCommand>();
    registry.register_command<DiffCommand>();
}

} // namespace cc::commands