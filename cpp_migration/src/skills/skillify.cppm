/// @file skillify.cppm
/// @brief Skillify skill - convert workflows into reusable skill definitions.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.skillify;

import cc.skills.skill;

export namespace cc::skills::skillify {

/// Skillify skill definition - meta-skill for creating new skills
[[nodiscard]] inline SkillDefinition make_skillify_skill() {
    return SkillDefinition{
        .name = "skillify",
        .description = "Convert a workflow or conversation pattern into a reusable skill",
        .trigger_patterns = {
            R"(skillif(?:y|ied))",
            R"(create\s+(?:a\s+)?skill)",
            R"(make.*(?:into|as)\s+(?:a\s+)?skill)",
            R"(save.*(?:as|into)\s+(?:a\s+)?skill)",
            R"(new\s+skill\s+from)",
        },
        .content = R"(## Skillify - Create a Reusable Skill

### Process
1. **Identify the pattern**: What repeatable workflow should become a skill?
2. **Extract the steps**: List the ordered steps with decision points
3. **Define triggers**: What phrases/contexts should activate this skill?
4. **Write the template**: Create the skill markdown with clear instructions
5. **Test matching**: Verify trigger patterns match intended inputs
6. **Deploy**: Save to ~/.cc-repl/skills/ directory

### Skill File Structure
```markdown
---
description: Brief description of what the skill does
trigger: regex pattern 1
trigger: regex pattern 2
author: your-name
version: 1.0.0
---

## Skill Name

### Step 1: ...
### Step 2: ...
```

### Best Practices
- Keep skills focused on one workflow
- Use clear, numbered steps
- Include decision points and exit conditions
- Provide examples for complex steps
- Test with varied trigger phrasings
- Version skills for iterative improvement

### Trigger Pattern Tips
- Use regex with case-insensitive matching
- Cover common phrasings and abbreviations
- Avoid overly broad patterns that cause false matches
- Test against a corpus of sample inputs
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::skillify
