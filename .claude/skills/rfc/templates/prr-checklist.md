<!--
  Production Readiness Review questionnaire — Loom RFCs.
  Paste this into the RFC before requesting status: implementable.
  Modeled on Kubernetes PRR. Answer every row; "N/A" needs a reason.
-->

## Production Readiness Review — RFC NNNN

### 1. Correctness and tests

- [ ] Every new behaviour has a new test (unit / module / e2e named).
- [ ] Failure paths covered (empty input, error result, timeout, abort).
- [ ] Golden suites assessed: `test_ui_*`, `test_dialog_*`,
      `test_prompt_dialog`, `test_cost_threshold_dialog`; if rendering
      changes, goldens regenerated and **manually reviewed** (not blindly
      accepted).
- [ ] Known timing flake list respected; no new wall-clock assertions with
      tight upper bounds under scheduler contention.
- [ ] Expected serial ctest total stated; deletions reconcile exactly.
- [ ] String/shape-based cross-module couplings (registry keys, tag formats,
      wire fields) changed on BOTH emitter and consumer sides;
      `docs/decisions/design-decisions.md` consulted.

**Notes:**

### 2. Build system and module discipline

- [ ] Affected producer-TU BMI PSS measured before AND after (PSS from
      `/proc/<pid>/smaps_rollup`, not RSS), numbers recorded.
- [ ] No Ninja concurrency reduction anywhere; memory handled by TU
      splitting / type erasure.
- [ ] Interface files gain declarations, not definitions (RFC 0001 Phase C
      direction); god-interface inline-body count does not increase.
- [ ] After RFC 0001 Phase A: no new textual standard-library or third-party
      includes in module units (`import std;` / `cc.third_party.*` used).
- [ ] Named partitions used only for PIMPL internals, not for cross-area
      layering.
- [ ] No new upward dependency edges; directory-level SCCs do not grow
      (architecture graph lint result attached).

**Notes:**

### 3. Rollback and compatibility

- [ ] Commits are atomic per phase; each phase revertible independently.
- [ ] No flag-day interface changes: PIMPL / type erasure / re-export shims
      keep importers building during migration.
- [ ] Persisted data compatibility considered (`~/.loom/sessions`,
      settings cascade, config schema) with migration or read-tolerance.
- [ ] Wire-protocol compatibility (`wire_anthropic` / `wire_openai`) —
      field additions are additive; removals justified.

**Notes:**

### 4. Observability

- [ ] Session traces remain valid: `messages.jsonl` block coverage and
      `dump-prompts/<id>.jsonl` request/response dumps.
- [ ] New background workers / threads are event-driven; no constant-rate
      render ticker (UI rule); teardown/abort paths defined.
- [ ] New diagnostics log through the existing debug channels; no new ad-hoc
      print paths.
- [ ] Build/performance metrics from this PRR recorded in the RFC.

**Notes:**

### 5. Documentation and deletion

- [ ] `CLAUDE.md` updated if conventions, build layout, or paths change.
- [ ] Non-obvious decisions added to `docs/decisions/design-decisions.md`.
- [ ] Dead code made obsolete by the work is deleted in the SAME phase
- [ ] Deprecated modules/shapes have a stated removal trigger and migration
      note; no silent shape drift.
- [ ] All comments/docs in English.

**Notes:**

### 6. Platform readiness (macos-14 / Linux)

- [ ] Debug + release presets build `-Werror` clean on the Linux dev box.
- [ ] macos-14 CI green at default parallelism (3 vCPU / 14 GB); no swap.
- [ ] Termios / signals / Apple-only guards correct for both platforms.
- [ ] truecolor (`COLORTERM=truecolor TERM=xterm-256color`) golden path
      intact.
- [ ] Offline dependency cache unaffected; no new network fetch required.

**Notes:**

### Review sign-off

| Role | Reviewer | Date | Verdict |
|---|---|---|---|
| Design |  |  | approve / request changes |
| Production readiness |  |  | approve / request changes |
| Code (per phase) |  |  | approve / request changes |
