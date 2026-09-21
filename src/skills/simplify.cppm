/// @file simplify.cppm
/// @brief Simplify skill - code simplification and cleanup workflow.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.simplify;

import cc.skills.skill;

export namespace cc::skills::simplify {

/// Simplify skill definition
[[nodiscard]] inline SkillDefinition make_simplify_skill() {
    return SkillDefinition{
        .name = "simplify",
        .description = "Review and simplify code for reuse, quality, and efficiency",
        .trigger_patterns = {
            R"(simplif(?:y|ied|ication))",
            R"(clean\s*up)",
            R"(polish(?:ing)?)",
            R"(reduce\s+complexity)",
            R"(make.*(?:simpler|cleaner|shorter))",
        },
        .content = R"(## Simplify Skill

### Process
1. **Identify**: Find overly complex code in recent changes
2. **Categorize**: Group issues by type (duplication, over-abstraction, dead code)
3. **Prioritize**: Rank by impact and risk of simplification
4. **Simplify**: Apply minimal transformations to reduce complexity
5. **Verify**: Ensure behavior is preserved after each change

### Common Simplifications
- **Extract**: Pull repeated logic into shared helpers
- **Inline**: Remove unnecessary indirection layers
- **Flatten**: Reduce nesting depth (early returns, guard clauses)
- **Eliminate**: Remove dead code, unused imports, redundant checks
- **Rename**: Improve clarity of identifiers

### Quality Checks
- Cyclomatic complexity reduced or unchanged
- No new dependencies introduced
- Test coverage maintained
- Public API unchanged (unless explicitly requested)

### Anti-patterns to Fix
- Premature abstraction (YAGNI violations)
- Copy-paste with minor variations
- Boolean blindness (use enums instead)
- Stringly-typed interfaces
- God functions (> 50 lines)

### Safety Rules
- Never simplify without passing tests
- Preserve git blame by using atomic commits
- Don't combine simplification with feature changes
- Keep each simplification independently revertible
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::simplify
