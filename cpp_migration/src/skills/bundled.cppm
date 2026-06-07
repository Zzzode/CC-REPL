/// @file bundled.cppm
/// @brief Built-in skill definitions with structured workflow steps.
/// Provides predefined skills: debug, verify, loop, batch, stuck.
module;

#include <algorithm>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <optional>
#include <format>
#include <ranges>

export module cc.skills.bundled;

import cc.skills.skill;

export namespace cc::skills {

// ============================================================
// Individual Bundled Skill Factories
// ============================================================

/// Systematic debugging workflow skill
[[nodiscard]] inline SkillDefinition make_debug_skill() {
    return SkillDefinition{
        .name = "debug",
        .description = "Systematic debugging workflow: reproduce, minimize, hypothesize, instrument, fix, verify",
        .trigger_patterns = {
            R"(debug\s+this)",
            R"(diagnose\s+this)",
            R"(bug.*(?:fix|found|report))",
            R"((?:broken|failing|throwing|crashing))",
            R"(performance\s+regression)",
        },
        .content = R"(## Systematic Debugging Workflow

### Step 1: Reproduce
- Confirm the bug exists with a minimal reproduction case
- Note exact error messages, stack traces, or unexpected behavior
- Record environment details (OS, runtime version, config)

### Step 2: Minimize
- Strip away unrelated code until you have the smallest failing case
- Identify whether the bug is deterministic or intermittent
- Check if it's environment-specific

### Step 3: Hypothesize
- Form 2-3 hypotheses about the root cause
- Rank by likelihood and ease of verification
- Consider recent changes that may have introduced the issue

### Step 4: Instrument
- Add targeted logging/tracing to verify hypotheses
- Use debugger breakpoints at suspect locations
- Verify assumptions about data flow and state

### Step 5: Fix
- Implement the minimal change that addresses root cause
- Ensure fix doesn't introduce new issues
- Consider edge cases the fix might affect

### Step 6: Verify
- Confirm the original reproduction case now passes
- Run the full test suite to catch regressions
- Remove any temporary debugging instrumentation
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

/// Verification before completion skill
[[nodiscard]] inline SkillDefinition make_verify_skill() {
    return SkillDefinition{
        .name = "verify",
        .description = "Verification checklist before claiming work is complete",
        .trigger_patterns = {
            R"(verif(?:y|ication))",
            R"(before\s+(?:commit|merge|push|complet))",
            R"(check(?:list)?.*(?:done|complete|ready))",
            R"(make\s+sure)",
        },
        .content = R"(## Verification Before Completion

### Checklist
1. **Build passes**: Run full build with no warnings/errors
2. **Tests pass**: Execute relevant test suites, verify green
3. **Linting**: No new lint warnings introduced
4. **Type-check**: Static analysis reports clean
5. **Manual test**: Verify the happy path works end-to-end
6. **Edge cases**: Test boundary conditions, empty inputs, error paths
7. **No regressions**: Existing functionality unchanged
8. **Documentation**: Update any affected docs/comments

### Evidence Requirements
- Show actual command output, not assumptions
- Include test run results with pass/fail counts
- Provide before/after comparison if fixing a bug
- Never claim "it works" without running verification commands
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

/// Iterative refinement loop skill
[[nodiscard]] inline SkillDefinition make_loop_skill() {
    return SkillDefinition{
        .name = "loop",
        .description = "Iterative refinement loop for incremental improvement",
        .trigger_patterns = {
            R"(iterati(?:ve|on))",
            R"(refine.*(?:loop|again|more))",
            R"(keep\s+(?:going|trying|improving))",
            R"(not\s+(?:good|right|done)\s+yet)",
        },
        .content = R"(## Iterative Refinement Loop

### Process
1. **Assess**: Evaluate current state against desired outcome
2. **Identify gap**: Pinpoint the specific shortcoming
3. **Plan increment**: Design the smallest change that moves toward goal
4. **Execute**: Make the change
5. **Measure**: Verify improvement (quantitatively if possible)
6. **Decide**: Continue iterating or accept current state

### Exit Conditions
- All acceptance criteria met
- Diminishing returns (last N iterations < threshold improvement)
- User signals satisfaction
- Resource/time budget exhausted

### Anti-patterns to Avoid
- Gold-plating beyond requirements
- Oscillating between two states
- Refactoring without measurable improvement
- Ignoring the cost of each iteration
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

/// Batch operations skill
[[nodiscard]] inline SkillDefinition make_batch_skill() {
    return SkillDefinition{
        .name = "batch",
        .description = "Batch operations pattern for processing multiple items efficiently",
        .trigger_patterns = {
            R"(batch\s+(?:process|operation|update|change))",
            R"((?:all|every|each)\s+file)",
            R"(bulk\s+(?:edit|update|rename|change))",
            R"(across\s+(?:all|multiple|many))",
        },
        .content = R"(## Batch Operations Pattern

### Planning Phase
1. **Enumerate targets**: List all items to be processed
2. **Categorize**: Group items by similarity or required action
3. **Dry run**: Show what would change without making changes
4. **User confirmation**: Present plan for approval before executing

### Execution Phase
1. **Process sequentially**: Handle one item at a time for debuggability
2. **Track progress**: Report N/M completed
3. **Error handling**: Log failures, continue with remaining items
4. **Rollback plan**: Know how to undo if batch goes wrong

### Verification Phase
1. **Summary report**: Items processed, succeeded, failed, skipped
2. **Spot-check**: Verify a few results manually
3. **Regression check**: Ensure batch didn't break unrelated functionality

### Safety Rules
- Never proceed without user confirmation on destructive batches
- Always provide a preview of changes
- Limit batch size to avoid overwhelming systems
- Include an abort mechanism
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

/// Unstuck strategies skill
[[nodiscard]] inline SkillDefinition make_stuck_skill() {
    return SkillDefinition{
        .name = "stuck",
        .description = "Strategies for getting unstuck when progress stalls",
        .trigger_patterns = {
            R"((?:i'?m\s+)?stuck)",
            R"((?:can'?t|cannot)\s+(?:figure|solve|fix|understand))",
            R"(no\s+(?:idea|progress|clue))",
            R"(tried\s+everything)",
            R"(help\s+me\s+(?:think|understand|figure))",
        },
        .content = R"(## Getting Unstuck

### Immediate Actions
1. **Re-read the error**: Often the answer is in the message you're ignoring
2. **Check assumptions**: List what you think is true, verify each one
3. **Simplify**: Remove complexity until the problem disappears, then add back
4. **Fresh search**: Try different search terms, look in different places

### Strategic Approaches
1. **Rubber duck**: Explain the problem step-by-step out loud
2. **Invert the problem**: Instead of "why doesn't X work", ask "what would make X work"
3. **Find working example**: Locate code that does something similar successfully
4. **Binary search**: Bisect the problem space to isolate the faulty component
5. **Time-box**: Set a 15-minute timer; if no progress, change approach entirely

### Escalation Paths
1. **Read the source**: Don't trust docs alone; read the actual implementation
2. **Check issues/forums**: Someone likely hit this exact problem before
3. **Minimal reproduction**: Create the simplest possible failing case
4. **Ask for help**: Present what you tried, what you expected, what happened

### Red Flags (Change Approach)
- Same error after 3+ different attempts
- Making random changes hoping something works
- Fixing one thing breaks another in a loop
- The "fix" is more complex than the original feature
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

// ============================================================
// BundledSkills - registry of all built-in skills
// ============================================================

/// Manages all predefined built-in skills and supports composition
class BundledSkills {
    std::vector<SkillDefinition> skills_;

public:
    BundledSkills() {
        // Register all built-in skills
        skills_.push_back(make_debug_skill());
        skills_.push_back(make_verify_skill());
        skills_.push_back(make_loop_skill());
        skills_.push_back(make_batch_skill());
        skills_.push_back(make_stuck_skill());
    }

    /// Get all bundled skill definitions
    [[nodiscard]] const std::vector<SkillDefinition>& all() const noexcept {
        return skills_;
    }

    /// Find a built-in skill by name
    [[nodiscard]] const SkillDefinition* find(std::string_view name) const {
        auto it = std::ranges::find_if(skills_,
            [name](const SkillDefinition& s) { return s.name == name; });
        return (it != skills_.end()) ? &(*it) : nullptr;
    }

    /// Compose multiple skills into a single combined skill
    [[nodiscard]] SkillDefinition compose(
        std::string_view composite_name,
        const std::vector<std::string_view>& skill_names) const {

        SkillDefinition composite;
        composite.name = std::string(composite_name);
        composite.description = std::format("Composite skill: {}", composite_name);
        composite.is_builtin = true;

        // Merge trigger patterns and content from all referenced skills
        for (auto sn : skill_names) {
            if (auto* skill = find(sn)) {
                for (const auto& pattern : skill->trigger_patterns) {
                    composite.trigger_patterns.push_back(pattern);
                }
                composite.content += std::format(
                    "\n---\n# {} - {}\n{}\n",
                    skill->name, skill->description, skill->content);
            }
        }
        return composite;
    }

    /// Register all bundled skills into a SkillExecutor
    void register_all(SkillExecutor& executor) const {
        for (const auto& skill : skills_) {
            executor.register_skill(skill);
        }
    }

    /// Number of bundled skills
    [[nodiscard]] std::size_t size() const noexcept { return skills_.size(); }
};

} // namespace cc::skills
