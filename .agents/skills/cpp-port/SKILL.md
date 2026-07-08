---
name: cpp-port
description: |
  Run a TS→CPP faithful-port round for the cpp_migration/ tree. Drives the
  cpp-port-round workflow: audit-driven gap discovery → 3-judge panel →
  implementation → build+ctest+golden verification. Call as `/cpp-port`
  with optional scope args.
---

# `/cpp-port` — TS → CPP Faithful-Port Automation

Run one round of the ultracode-style `cpp-port-round` workflow against the
`cpp_migration/` tree. Every round autonomously:

1. loads `docs/audit_round7_full_report.json` (the 608-feature structured
   cross-reference) and filters by scope,
2. runs a 3-lens judge panel (feasibility / priority / risk) over the
   shortlist,
3. implements the top-K doable gaps in parallel (default K=1),
4. builds **both** Debug + Release presets with `-j 8`,
5. runs a scoped `ctest -R … --output-on-failure --test-timeout 30`,
6. refreshes goldens with `UPDATE_GOLDENS=1` if mismatched,
7. spawns an adversarial debugger agent on any fresh test regression,
8. optionally groups changes into atomic conventional commits and pushes.

## Invocation

```
/cpp-port                                  # top-P0 visible gap, default K=1, no-auto-commit
/cpp-port P0                               # scope: P0 only
/cpp-port P0+P1                            # scope: any severity in {P0,P1}
/cpp-port subsys:UI-Messages               # scope: one audit subsystem
/cpp-port ts:src/components/Logo           # scope: TS path substring match
/cpp-port cpp:ui/messages/                 # scope: CPP path substring match
/cpp-port all --parallelism 3              # K=3 parallel implementations
/cpp-port P0 --commit auto --push          # commit atomically + push on PASS
```

## Argument grammar (whitespace-separated)

- `all`, `P0`, `P1`, `P2`, `P3` or `A+B` severity union
- `subsys:<name>` — filter to one audit subsystem
  (`UI-Audit-Report|UI-Messages|UI-Prompt|UI-Dialogs|DesignTokens|CppMigrationAudit`)
- `ts:<path-prefix>` — `ts_path` substring filter
- `cpp:<path-prefix>` — `cpp_path` substring filter
- `--parallelism <1..3>` — number of gaps to fan out in parallel
  (build/CTest stay sequential per gap to avoid ninja contention)
- `--commit auto` — auto-group changes into atomic conventional commits
  on any implementation that VERDICT=PASS (goldens separate from code,
  CMake module-additions separate)
- `--push` — `git push origin HEAD` after auto-commits succeed

## Ground rules (baked into every implementor agent)

1. **TS = authority.** Read `src/<ts_path>` first. Match exactly; leave
   `// TS REF: file:line` breadcrumbs in CPP where the port is non-obvious.
2. **Golden snapshots mandatory** for every renderer / dialog / layout
   change.
3. **Palette-driven colors only** — no hardcoded RGB; add a Palette/Role
   field if the token is missing.
4. **FTXUI lifetime caution**: dropping a Component after `c->Render()`
   = heap-use-after-free (see the `CompEl` fix pattern).
5. **Event-driven repaints only** — no constant-rate tickers (they cause
   idle cursor flicker).
6. **SLOC budget per module** < ~12K effective lines; thin module + impl
   unit split if over (Clang BMI / SLOC hard cap is well below `app.cppm`'s
   historical blow-up).
7. **Comments & docstrings in English.** Chinese reserved for user chat.

## Output shape

The workflow returns a structured report with:

- `attempted[]` — each gap with verdict / priority / risk / new-failures
- `remaining_ranked[]` — unimplemented but judged, ready for the next round
- `commit_plan` / `pushed` — commit summary if `--commit auto`/`--push`
- `budget_spent_tokens` — token cost of the round

## Pre-existing flake allow-list

These failing test names are always excluded from regression counting
(cross-checked against ctest T1 baseline):

- `FTXUIIntegration.VerboseIndicator`
- `ReplScreen.PromptInputParksHiddenNativeCursorAtCaret`
- `SandboxPermissionEvents.*` (4 variants)
- `AppRuntime.SkillsDialogDismissOrderDebug`
- `BridgeDaemon.PipesHeadlessChildStdinAndCapturesStdout`

Any other test failure → debugger agent spawned automatically.
