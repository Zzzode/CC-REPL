// ============================================================================
// Bundled skills registry audit (Phase 2, Agent S2):
//   Source of truth: src/skills/bundled/index.ts (17 skills + 4 conditional)
//   Audit baseline  : 2026-06-09, S2 commit
//
//   Legend: OK migrated & registered   IN imported from root module
//           RT partial (runtime impl only)  DF DEFER / TODO
//
//   batch.cppm                    OK migrated (Phase 0)  make_batch_skill()
//   claude_api.cppm               RT bundled/ has impl + IN root SkillDefinition
//   claude_api_content.cppm       IN S1 migrated (root: make_claude_api_content_skill)
//   claude_in_chrome.cppm         OK (S2, this commit)  make_claude_in_chrome_skill()
//   debug.cppm                    OK migrated (Phase 0)  make_debug_skill()
//   index                         -> THIS FILE (aggregator)
//   keybindings (bundled)         OK (S2, this commit)  make_keybindings_help_skill()
//                                 NOTE: root keybindings.cppm = simple shortcut sheet
//                                       bundled keybindings.cppm   = full customization guide
//   loop.cppm                     OK migrated (Phase 0)  make_loop_skill()
//   lorem_ipsum.cppm              IN root: make_lorem_ipsum_skill()
//   remember.cppm                 RT bundled/ has impl + IN root SkillDefinition
//   schedule_remote_agents.cppm   IN root: make_schedule_remote_agents_skill()
//   simplify.cppm                 RT bundled/ has impl + IN root SkillDefinition
//   skillify.cppm                 IN root: make_skillify_skill()
//   stuck.cppm                    OK migrated (Phase 0)  runtime impl in bundled/stuck
//                                 + self-unstuck definition here + /stuck adapter
//   update_config.cppm            IN root: make_update_config_skill()
//   verify.cppm                   OK migrated (Phase 0)  make_verify_skill()
//   verify_content.cppm           IN S1 migrated (root: make_verify_content_skill)
//
// Conditional (feature-flag gated in TS, unregistered here pending flags):
//   dream.ts                      DF DEFER: requires KAIROS feature flag + prompt design
//   hunter.ts                     DF DEFER: requires REVIEW_ARTIFACT + review design
//   runSkillGenerator.ts          DF DEFER: requires RUN_SKILL_GENERATOR feature
//
// Missing total: 3 (all feature-flag conditional)
// ============================================================================
/// @file bundled.cppm
/// @brief Built-in skill definitions with structured workflow steps.
/// Provides predefined skills: update-config, keybindings, keybindings-help,
/// lorem-ipsum, remember, verify, verify-content, debug, simplify, skillify,
/// self-unstuck, stuck, loop, batch, schedule-remote-agents, claude-api,
/// claude-api-content, claude-in-chrome.
///
/// REGISTRATION ORDER (dependency-first):
///   1. Config/infra first (update-config)
///   2. Reference/helpers (keybindings, lorem-ipsum, remember)
///   3. Validation (verify, verify-content)
///   4. Workflow (debug, simplify, skillify, self-unstuck, stuck, loop, batch)
///   5. Agent orchestration (schedule-remote-agents)
///   6. Integration (claude-api, claude-api-content, claude-in-chrome)
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

// ---------------------------------------------------------------------------
// Root-level skill modules (imported + re-registered in BundledSkills)
// ---------------------------------------------------------------------------
import cc.skills.claude_api;
import cc.skills.claude_api_content;
import cc.skills.lorem_ipsum;
import cc.skills.remember;
import cc.skills.schedule_remote_agents;
import cc.skills.simplify;
import cc.skills.skillify;
import cc.skills.update_config;
import cc.skills.verify_content;
import cc.skills.keybindings; // NOTE: simple shortcut sheet, separate from keybindings-help

// ---------------------------------------------------------------------------
// Bundled sub-modules (runtime impls from cpp_migration/src/skills/bundled/)
// ---------------------------------------------------------------------------
import cc.skills.bundled.stuck;      // runtime: detect_stuck_pattern, get_stuck_skill_manifest
import cc.skills.bundled.claude_in_chrome;
import cc.skills.bundled.skill_keybindings;

export namespace cc::skills {

// ============================================================
// Individual Bundled Skill Factories
// ============================================================

// --- Already migrated (Phase 0) --------------------------------------------

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

/// Self-unstuck strategies - "I, the agent, am stuck on this task".
///
/// NOTE: This used to be registered as "stuck" (v1.0).  The /stuck slash
/// command in TS is about DIAGNOSING OTHER CLAUDE CODE SESSIONS on the
/// same machine (see cc::skills::bundled::make_stuck_skill imported from
/// cc.skills.bundled.stuck).  We keep the generic self-unstuck advice
/// under a separate discoverable name so neither behaviour is lost.
[[nodiscard]] inline SkillDefinition make_self_unstuck_skill() {
    return SkillDefinition{
        .name = "self-unstuck",
        .description =
            "Strategies for getting unstuck when progress stalls on a task",
        .trigger_patterns = {
            R"(how\s+to\s+proceed)",
            R"(stuck\s+on\s+this\s+task)",
            R"(approach\s+not\s+working)",
            R"(what\s+to\s+try\s+next)",
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
        .version = "1.1.0",
    };
}

/// /stuck skill - diagnose other sessions + get alternative approaches.
/// Wraps runtime functions from cc.skills.bundled.stuck into a SkillDefinition.
[[nodiscard]] inline SkillDefinition make_stuck_skill() {
    auto manifest = cc::skills::bundled::get_stuck_skill_manifest();
    std::vector<std::string> triggers;
    triggers.reserve(manifest.triggers.size());
    for (const auto& t : manifest.triggers) triggers.push_back(t);
    return SkillDefinition{
        .name = std::move(manifest.name),
        .description = std::move(manifest.description),
        .trigger_patterns = std::move(triggers),
        .content = R"(## Stuck Diagnosis (/stuck)

### Detect Stuck Patterns
Check recent outputs for:
1. Identical consecutive outputs (exact repetition 3x)
2. Multiple consecutive error messages suggesting a loop
3. Oscillation between two states across 4+ outputs

### Alternative Approaches
When stuck, evaluate whether:
- Permission issues exist -> try elevated access or check ownership
- Resource not found -> verify path, list directory, search broadly
- Timeout -> check network, increase timeout, try different endpoint
- Syntax/parse errors -> validate format, check delimiters, simplify input
- Import/module issues -> verify installed, check paths, clear cache
- Generic -> break into smaller steps, verify assumptions, search codebase

For full runtime detection call cc::skills::bundled::detect_stuck_pattern()
and cc::skills::bundled::suggest_unstuck_action(context).
)",
        .is_builtin = true,
        .author = manifest.version,
        .version = manifest.version.value_or("1.0.0"),
    };
}

// --- Phase 2, S2 additions --------------------------------------------------

/// Claude in Chrome skill - browser automation via Chrome extension MCP
/// Mirrors src/skills/bundled/claudeInChrome.ts.
[[nodiscard]] inline SkillDefinition make_claude_in_chrome_skill() {
    return SkillDefinition{
        .name = "claude-in-chrome",
        .description =
            "Automates your Chrome browser to interact with web pages - clicking elements, "
            "filling forms, capturing screenshots, reading console logs, and navigating sites. "
            "Opens pages in new tabs within your existing Chrome session. Requires site-level "
            "permissions before executing (configured in the extension).",
        .trigger_patterns = {
            // English keywords
            R"(chrome\s+extension)",
            R"(chrome\s+plugin)",
            R"(chrome\s+tool)",
            R"(browser\s+automation)",
            R"(web\s+page\s+(?:interact|scrape|automate))",
            R"(web\s+browser\s+tool)",
            R"((?:sidebar|side\s+bar)\s+(?:chrome|browser))",
            R"(claude\s+in\s+chrome\s+not\s+respond)",
            R"(cfc\s+(?:not|broken|issue))",
            R"(chrome\s+(?:tab|screenshot|console))",
            // Chinese keywords (utf-8 escapes)
            u8R"(\u6d4f\u89c8\u5668.*\u64cd\u4f5c)",
            u8R"(\u7f51\u9875.*\u64cd\u4f5c)",
            u8R"(chrome.*\u63d2\u4ef6)",
            u8R"(\u4fa7\u8fb9\u680f)",
            u8R"(\u6269\u5c55.*\u6ca1\u53cd\u5e94)",
        },
        .content = cc::skills::bundled::build_claude_in_chrome_prompt()
            + "\n\n## Troubleshooting\n\n"
            + cc::skills::bundled::build_troubleshooting_checklist(),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

/// Bundled keybindings-help skill - full customization workflow.
/// NOTE: This is the COMPREHENSIVE guide (name = "keybindings-help"), distinct
/// from the root-level simple shortcut sheet (name = "keybindings") in
/// cc.skills.keybindings.  The two skills target different user needs:
///   - keybindings       -> quick lookup of default shortcuts
///   - keybindings-help  -> file format, rebinding recipes, /doctor validation
[[nodiscard]] inline SkillDefinition make_keybindings_help_skill() {
    return SkillDefinition{
        .name = "keybindings-help",
        .description =
            "Use when the user wants to customize keyboard shortcuts, rebind keys, add "
            "chord bindings, or modify ~/.claude/keybindings.json. Examples: \"rebind "
            "ctrl+s\", \"add a chord shortcut\", \"change the submit key\", "
            "\"customize keybindings\".",
        .trigger_patterns = {
            R"(keybind(?:ing)?s?\s+(?:customi[sz]e|change|modify|edit))",
            R"(rebind\s+(?:key|shortcut))",
            R"(customi[sz]e\s+(?:keyboard|shortcut|keybinding))",
            R"(keybindings\.json)",
            R"(chord\s+(?:key|binding|shortcut))",
            R"(unbind\s+key)",
            R"(change\s+submit\s+key)",
            R"(unbind\s+default)",
            // Chinese triggers (utf-8 escapes)
            u8R"(\u5feb\u6377\u952e.*\u81ea\u5b9a\u4e49)",
            u8R"(\u952e\u4f4d)",
            u8R"(\u4fee\u6539.*\u5feb\u6377\u952e)",
            u8R"(\u91cd\u65b0\u7ed1\u5b9a)",
        },
        .content = cc::skills::bundled::build_keybindings_help_prompt(),
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
        // --- Registration order: dependency-first per audit header ---

        // 1. Config / infrastructure
        skills_.push_back(cc::skills::update_config::make_update_config_skill());

        // 2. Reference / helpers
        // root keybindings (simple cheat sheet, separate from help)
        skills_.push_back(cc::skills::keybindings::make_keybindings_skill());
        // bundled keybindings-help (full customization guide)
        skills_.push_back(make_keybindings_help_skill());
        skills_.push_back(cc::skills::lorem_ipsum::make_lorem_ipsum_skill());
        skills_.push_back(cc::skills::remember::make_remember_skill());

        // 3. Validation
        skills_.push_back(make_verify_skill());
        skills_.push_back(cc::skills::verify_content::make_verify_content_skill());

        // 4. Workflow skills
        skills_.push_back(make_debug_skill());
        skills_.push_back(cc::skills::simplify::make_simplify_skill());
        skills_.push_back(cc::skills::skillify::make_skillify_skill());
        skills_.push_back(make_self_unstuck_skill());
        skills_.push_back(make_stuck_skill());
        skills_.push_back(make_loop_skill());
        skills_.push_back(make_batch_skill());

        // 5. Agent orchestration
        skills_.push_back(cc::skills::schedule_remote_agents::make_schedule_remote_agents_skill());

        // 6. Integration (API + browser)
        skills_.push_back(cc::skills::claude_api::make_claude_api_skill());
        skills_.push_back(cc::skills::claude_api_content::make_claude_api_content_skill());
        skills_.push_back(make_claude_in_chrome_skill());

        // TODO(feature-flag): Register the following only when feature flags become
        // available in C++ runtime (mirrors TS index.ts feature() gating):
        //   - dream          (KAIROS || KAIROS_DREAM)
        //   - hunter         (REVIEW_ARTIFACT)
        //   - runSkillGenerator (RUN_SKILL_GENERATOR)
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

// ============================================================
// Free function entry point (required per Phase 2, Agent S2 spec)
// ============================================================

/// Register all bundled skills into the given SkillExecutor.
///
/// This is the preferred entry point for consumers of the bundled registry,
/// as it works with the global SkillExecutor without requiring the caller
/// to instantiate the BundledSkills class.
///
/// @returns Number of skills successfully registered.
inline std::size_t register_all_bundled_skills(SkillExecutor& executor) {
    static const BundledSkills registry;
    registry.register_all(executor);
    return registry.size();
}

} // namespace cc::skills
