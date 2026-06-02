/// @file remember.cppm
/// @brief Remember skill - persistent memory and context recall.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.remember;

import cc.skills.skill;

export namespace cc::skills::remember {

/// Remember skill for persistent context recall
[[nodiscard]] inline SkillDefinition make_remember_skill() {
    return SkillDefinition{
        .name = "remember",
        .description = "Store and recall facts, preferences, and context across sessions",
        .trigger_patterns = {
            R"(remember\s+(?:this|that))",
            R"(don'?t\s+forget)",
            R"(save.*(?:preference|context|fact))",
            R"(recall.*(?:earlier|before|previous))",
            R"(what\s+did\s+(?:i|we)\s+(?:say|decide))",
        },
        .content = R"(## Remember Skill

### Purpose
Persist important facts, decisions, and preferences across sessions.

### Storage Categories
- **Facts**: Project-specific knowledge (e.g., "API uses OAuth2")
- **Preferences**: User choices (e.g., "prefer tabs over spaces")
- **Decisions**: Architectural choices (e.g., "using PostgreSQL for persistence")
- **Context**: Background info (e.g., "this is a monorepo with 3 services")

### Operations
1. **Store**: Save a new memory with category and tags
2. **Recall**: Retrieve relevant memories by context
3. **Update**: Modify existing memories when facts change
4. **Forget**: Remove outdated or incorrect memories

### Best Practices
- Store decisions with rationale, not just the choice
- Tag memories with project/topic for easier retrieval
- Periodically review and prune stale memories
- Don't store sensitive data (API keys, passwords)

### Memory File
Memories are stored in ~/.cc-repl/memories.json and CLAUDE.md.
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::remember
