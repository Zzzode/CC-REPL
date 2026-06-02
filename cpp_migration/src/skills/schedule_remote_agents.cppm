/// @file schedule_remote_agents.cppm
/// @brief Schedule Remote Agents skill - orchestrating distributed agent execution.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.schedule_remote_agents;

import cc.skills.skill;

export namespace cc::skills::schedule_remote_agents {

/// Schedule remote agents skill definition
[[nodiscard]] inline SkillDefinition make_schedule_remote_agents_skill() {
    return SkillDefinition{
        .name = "schedule-remote-agents",
        .description = "Schedule and orchestrate remote agent execution for parallel tasks",
        .trigger_patterns = {
            R"(schedule.*(?:agent|remote))",
            R"(remote\s+agent)",
            R"(parallel.*agent)",
            R"(dispatch.*(?:task|agent))",
            R"(swarm\s+(?:schedule|dispatch))",
        },
        .content = R"(## Schedule Remote Agents

### Purpose
Dispatch tasks to remote agents for parallel execution across sessions.

### Workflow
1. **Define tasks**: Break work into independent, parallelizable units
2. **Select agents**: Choose appropriate agent profiles for each task
3. **Schedule**: Dispatch tasks with priority and dependencies
4. **Monitor**: Track progress across all remote agents
5. **Collect**: Gather results and merge into unified outcome

### Task Definition
- Each task must be self-contained with clear inputs/outputs
- Specify timeout and retry policy per task
- Define success criteria for automated verification
- Include rollback instructions for failure cases

### Agent Selection
- Match agent capabilities to task requirements
- Consider resource constraints (context window, tools)
- Balance load across available agents
- Prefer locality for file-heavy operations

### Monitoring
- Poll agent status at configurable intervals
- Alert on stalled or failed agents
- Provide real-time progress dashboard
- Log all agent interactions for debugging

### Error Handling
- Retry transient failures automatically
- Escalate persistent failures to orchestrator
- Support partial result collection
- Graceful degradation when agents unavailable
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::schedule_remote_agents
