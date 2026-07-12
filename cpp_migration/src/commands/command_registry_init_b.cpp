/// @file command_registry_init_b.cpp
/// @brief Group B registration: utility commands (effort, fast, files, stats, etc.)
module cc.commands.registry;

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

namespace cc::commands {

void register_group_b_commands(CommandRegistry& registry) {
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
}

} // namespace cc::commands