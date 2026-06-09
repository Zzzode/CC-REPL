/// @file skillify.cppm
/// @brief Bundled Skillify skill - full meta-skill creator with structured interview flow.
/// Converts a session's repeatable workflow into a reusable SKILL.md file.
/// Heavier than the root-level skillify.cppm which is a simplified version.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <format>

export module cc.skills.bundled.skillify;

import cc.skills.skill;
import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

// ============================================================
// Skillify Prompt - mirrors TS bundled/skillify.ts
// ============================================================

constexpr std::string_view SKILLIFY_PROMPT = R"(## Skillify - Capture a Repeatable Workflow as a Skill

### Purpose
You are converting this session's repeatable process into a reusable skill that
can be invoked automatically in future sessions. The skill lives as a markdown
file (SKILL.md) either in the project-local `.claude/skills/` directory or the
user's global skills directory.

### Phase 1: Session Analysis (DO BEFORE asking questions)

Extract the following from the session context:
- **Repeatable process**: What was the core workflow performed?
- **Inputs/Parameters**: What did the user have to provide each time?
- **Ordered steps**: List the distinct steps in execution order
- **Success criteria per step**: Not just "did something" but specific
  artifacts or passing conditions (e.g. "CI passing", "PR opened", "test green")
- **User corrections**: Where did the user steer you away from a wrong path?
- **Tools & permissions needed**: Which tools were used, with what patterns
  (e.g. `Bash(git:*)`, `Bash(npm:*)`, `Edit(.claude)`)
- **Agents used**: Did the workflow involve sub-agents or teams?
- **Goals & success artifacts**: What tangible outputs proved completion?

### Phase 2: Structured User Interview (use AskUserQuestion for ALL rounds)

**Round 1 - High-Level Confirmation:**
- Propose a skill name and one-line description based on your analysis
- Ask user to confirm, rename, or rewrite the description
- Propose high-level goals and specific success criteria for the entire skill

**Round 2 - Structure & Scope:**
- Present the ordered step list you identified as a numbered list. Tell the
  user you will drill into each step next.
- If the workflow requires arguments (parameters), suggest them based on
  observed inputs. Ensure you understand what a caller must provide.
- Clarify execution context: **inline** (runs in current conversation, good for
  mid-process steering) vs **forked** (runs as sub-agent with isolated context,
  good for self-contained tasks).
- Ask where to save the skill:
  - *This repo* (`.claude/skills/<name>/SKILL.md`) — for project-specific workflows
  - *Personal* (`~/.claude/skills/<name>/SKILL.md`) — cross-repo reusable

**Round 3 - Step Breakdown (one round per major step, especially if >3 steps):**
For each non-obvious step, ask:
- What artifacts does this step produce that later steps consume?
- What proves this step succeeded and we can advance?
- Should we pause for user confirmation before proceeding? (especially for
  irreversible actions: merge, send messages, destructive ops)
- Are any steps independent and safe to run in parallel?
- What execution mode: Direct (default), Task agent, Teammate (parallel), or
  [human] (user must act)?
- Any hard rules or constraints: things that MUST or MUST NOT happen?

**Round 4 - Triggers & Edge Cases:**
- Confirm WHEN this skill should auto-invoke (trigger phrases + contexts)
- Provide concrete examples of user messages that should trigger it
- Ask about gotchas, edge cases, or failure modes to watch for
- STOP interviewing once information is complete. Do NOT over-ask for simple
  1-2 step processes.

### Phase 3: Generate & Write SKILL.md

File structure (YAML frontmatter + markdown):

```markdown
---
name: {{skill-name}}
description: {{one-line description}}
allowed-tools:
  - Bash(pattern1:*)
  - Edit(pattern2)
  - Read
when_to_use: |
  Use when the user wants to {{detailed scenario}}.
  Examples: 'trigger phrase 1', 'example user message 2'.
argument-hint: "[arg1] [arg2]"
arguments:
  - arg_name1
  - arg_name2
context: fork    # OMIT for inline; set to 'fork' only for self-contained
---

# {{Skill Title}}

## Inputs
- `$arg_name1`: Description of this input

## Goal
Clearly-stated goal with specific artifacts or completion criteria.

## Steps

### 1. Step Name
Specific actionable instructions. Include exact commands when applicable.

**Success criteria:** ALWAYS include this. List of conditions that mean the step
is complete and we can proceed.

(Optional per-step annotations:)
- **Execution**: Direct | Task agent | Teammate | [human]
- **Artifacts**: PR number, commit SHA, etc. (data for later steps)
- **Human checkpoint**: Pause + ask before proceeding (irreversible actions)
- **Rules**: Hard constraints, especially from user corrections in session

### 2. Next Step Name
...
```

**Frontmatter rules:**
- `allowed-tools`: Use minimum-scope patterns (e.g. `Bash(gh:*)` not bare `Bash`)
- `context: fork` ONLY for self-contained skills needing no mid-process steering
- `when_to_use` is CRITICAL: start with "Use when..." and include trigger phrases
- `arguments` and `argument-hint`: ONLY if the skill takes parameters; use
  `$name` placeholders in the body for substitution

**Step structure tips:**
- Parallelizable steps use sub-numbering (3a, 3b)
- User-action steps get `[human]` in the title
- Keep simple skills simple (2-step skills don't need every annotation)

### Phase 4: Confirm & Save

1. Render the COMPLETE SKILL.md inside a ```yaml or ```markdown code block so
   the user can review it with syntax highlighting
2. Ask a concise confirmation via AskUserQuestion: "Does this SKILL.md look
   good to save?" — do NOT elaborate in the question body
3. After user confirms, write the file:
   - Create the skill directory if needed (`mkdir -p`)
   - Write `SKILL.md` at the chosen path
   - The skill loader (load_skills_dir) auto-discovers the file on next session

**After save, tell the user:**
- Exact save path
- Invocation syntax: `/{{skill-name}} [arguments]`
- Remind them they can edit SKILL.md directly at any time to refine it
)";

// ============================================================
// Skillify Bundled Factory
// ============================================================

/// Full bundled skillify skill with structured interview flow.
/// This is heavier than the root-level simplified skillify.
[[nodiscard]] inline SkillDefinition make_bundled_skillify_skill() {
    return SkillDefinition{
        .name = "skillify",
        .description =
            "Capture this session's repeatable workflow as a reusable skill. "
            "Call at the end of the process you want to capture with an optional description.",
        .trigger_patterns = {
            R"(skillif(?:y|ied))",
            R"(create\s+(?:a\s+)?skill)",
            R"(make.*(?:into|as)\s+(?:a\s+)?skill)",
            R"(save.*(?:as|into)\s+(?:a\s+)?skill)",
            R"(new\s+skill\s+from)",
            R"(capture\s+(?:this\s+)?(?:workflow|process|pattern))",
            R"(turn\s+(?:this\s+)?into\s+(?:a\s+)?skill)",
        },
        .content = std::string(SKILLIFY_PROMPT),
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

// ============================================================
// Skillify Helpers - delegate file discovery to load_skills_dir
// ============================================================

/// Given a skill name and save location, return the filesystem path where
/// the SKILL.md should be written. Does NOT create files — caller handles I/O.
[[nodiscard]] inline std::filesystem::path build_skill_save_path(
    std::string_view skill_name,
    bool is_project_local,
    const std::filesystem::path& project_root,
    const std::filesystem::path& home_dir
) {
    std::filesystem::path base = is_project_local
        ? project_root / ".claude" / "skills"
        : home_dir / ".claude" / "skills";
    return base / std::string(skill_name) / "SKILL.md";
}

/// Rescan skill directories after writing a new skill. Delegates to
/// load_skills_dir so skillify does not duplicate parsing logic.
[[nodiscard]] inline std::vector<SkillManifest> rescan_skills() {
    std::vector<SkillManifest> all;
    for (const auto& path : get_skills_search_paths()) {
        auto found = load_skills_directory(path);
        all.insert(all.end(),
                   std::make_move_iterator(found.begin()),
                   std::make_move_iterator(found.end()));
    }
    return all;
}

} // namespace cc::skills::bundled
