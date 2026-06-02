/// @file verify_content.cppm
/// @brief Verify Content skill - content validation and correctness checks.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.verify_content;

import cc.skills.skill;

export namespace cc::skills::verify_content {

/// Verify content skill definition
[[nodiscard]] inline SkillDefinition make_verify_content_skill() {
    return SkillDefinition{
        .name = "verify-content",
        .description = "Validate generated content for accuracy, completeness, and correctness",
        .trigger_patterns = {
            R"(verify.*content)",
            R"(check.*(?:accuracy|correctness))",
            R"(validate.*(?:output|response|result))",
            R"(is\s+this\s+(?:correct|right|accurate))",
            R"(double.?check)",
        },
        .content = R"(## Verify Content Skill

### Purpose
Ensure generated content is accurate, complete, and fit for purpose.

### Verification Dimensions
1. **Factual accuracy**: Are claims verifiable and correct?
2. **Completeness**: Are all requested elements present?
3. **Consistency**: Does content align with prior context?
4. **Format compliance**: Does output match expected structure?
5. **Code correctness**: Does generated code compile and run?

### Process
1. **Re-read requirements**: Compare output against original request
2. **Check facts**: Verify any factual claims against sources
3. **Test code**: Run any generated code to confirm it works
4. **Cross-reference**: Check consistency with project context
5. **Edge cases**: Consider boundary conditions and error paths
6. **Report**: List verified items and any issues found

### Evidence Standards
- Show actual output, not assumed results
- Include command runs with timestamps
- Provide before/after comparisons for changes
- Link to source for factual claims
- Flag uncertainty explicitly

### Common Failures
- Hallucinated function names or APIs
- Incorrect parameter types or orders
- Missing error handling in generated code
- Outdated information from training data
- Inconsistent naming with existing codebase
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::verify_content
