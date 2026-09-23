---
name: rfc
description: |
  Govern the full Loom RFC lifecycle, Kubernetes-KEP style — drafting,
  stage gates (provisional → accepted → implementable → implemented),
  production-readiness review, per-phase graduation criteria, implementation
  tracking, supersession and withdrawal. Invoke as `/rfc` when creating a new
  RFC, changing an RFC's status, starting/completing an implementation phase,
  or reviewing a PR that implements one.
---

# `/rfc` — Loom RFC Process

Loom's RFCs live in **`docs/rfcs/NNNN-kebab-slug.md`** (four digits, zero
padded). The process is modeled on the Kubernetes Enhancement Proposal
workflow: an RFC is a *contract with gates*, not a design essay. Code does
not merge ahead of its RFC's gate.

This skill governs **both content and implementation**:

1. drafting a new RFC from the template,
2. moving an RFC through its stage gates,
3. reviewing an RFC before approval (design + production-readiness),
4. tracking implementation phase by phase with measurable graduation
   criteria,
5. superseding, withdrawing, or rejecting an RFC.

All RFC text, code comments and commit messages are in **English** (project
rule).

## 1. Lifecycle and gates

```
              file lands            reviewers approve            first code merges
provisional ───────────────▶ accepted ───────────────▶ implementable ─────────────▶ implemented
     │                            │                          │  (phases A, B, …)       ▲
     │                            │                          │                         │
     └─▶ withdrawn / rejected ◀───┴──────────────────────────┴──▶ deferred             │
                                                                                       │
                                                                  replaced / superseded (terminal)
```

| Stage | Meaning | Hard requirements to ENTER |
|---|---|---|
| `provisional` | Problem statement and rough proposal; number assigned. | File exists, passes `tools/rfc/rfc_lint.py`, owner named, Goals/Non-goals/Motivation drafted. |
| `accepted` | The problem, target shape and trade-offs are agreed; not yet cleared to build. | All template sections answered (explicit "N/A, because …" is allowed), Alternatives compared, reviewer(s) recorded, open questions resolved or enumerated. |
| `implementable` | Cleared to write code. | Per-phase plan with **measurable** graduation criteria, PRR checklist filled, rollback story, tracking issue, reviewer approval. |
| `implemented` | Every phase complete and independently verified. | All phases `done`, metrics recorded, docs/CLAUDE.md updated, lint/CI gates green, no unresolved `TODO(rfc)`. |
| `deferred` | Accepted but deliberately not scheduled. | Reason + revisit condition stated. |
| `rejected` / `withdrawn` | Not pursued. | Rationale kept; number never reused. |
| `replaced` | Superseded by another RFC. | `superseded-by: NNNN` set in both directions. |

**Gate rule:** no production-code commit implementing an RFC may merge while
its status is below `implementable`. Exploratory spikes are allowed only on a
branch and must not change `src/` on master. Implementation commits cite the
RFC: `RFC-NNNN` in the commit body.

Large phased RFCs (example: RFC 0001, phases A–F) use the **phase table**
in the template. The RFC can be `implementable` while individual phases are
`proposed`; a phase moves to `done` only when its graduation criteria are
measured and recorded (see §4).

## 2. How to invoke this skill

### 2.1 Creating an RFC (`/rfc new "<title>"`)

1. Determine the next number:
   `ls docs/rfcs/*.md | sed 's#.*/##' | sort | tail -1` and increment.
2. Copy `templates/rfc-template.md` to
   `docs/rfcs/NNNN-<kebab-slug>.md`.
3. Fill frontmatter (`rfc`, `title`, `status: provisional`, `owners`,
   `created`).
4. Draft at least: Summary, Motivation (with **measured evidence**, not
   vibes), Goals, Non-Goals, Proposal, Alternatives.
5. Run `python3 tools/rfc/rfc_lint.py` — it must pass.
6. Commit as `docs(rfc): add RFC NNNN <title> (provisional)` and open the
   tracking issue. Docs-only changes do not trigger macos CI.

Never renumber or reuse a retired number.

### 2.2 Stage transition (`/rfc accept|implement|defer|reject|supersede NNNN`)

1. Re-read the current RFC end to end; do not trust the requester's summary.
2. Check every requirement for the target stage in §1; list what is missing
   and stop if anything fails — do not promote an incomplete RFC.
3. For `accepted`/`implementable`, walk the review checklists (§3) and demand
   concrete answers. "TBD" is not an answer at `implementable`.
4. Update frontmatter (`status`, `last-reviewed`, `reviewers`) and, for
   `implementable`, the phase/graduation table and implementation history
   header. Lint, commit.

### 2.3 Starting / completing an implementation phase (`/rfc phase NNNN <id> start|done`)

- **start:** RFC is `implementable`; append an Implementation History row
  (date, phase, branch/commit, owner). Confirm the phase's prerequisites are
  met (earlier phases done or explicitly parallel).
- **done:** execute the phase's graduation criteria and **record the measured
  numbers** in the row (PSS in MB, wall time, fan-out, ctest totals, lint
  results). Then flip the phase table cell to `done`. A phase is not done on
  assertion; it is done on recorded evidence.

### 2.4 Reviewing an implementation PR for an RFC

Before approving, verify in addition to the normal review:

- commit body cites `RFC-NNNN` and the right phase;
- diff matches that phase's declared scope (no silent scope creep — extra
  work either belongs to another phase or the RFC is amended first);
- phase graduation criteria are satisfied with measurements;
- PRR items touched by the diff were honored (tests, rollback, docs);
- dual preset `-Werror` + serial ctest green; default Ninja parallelism was
  never throttled to manage memory (standing rule — split TUs instead).

## 3. Review checklists

### 3.1 Design review (gate to `accepted`)

- [ ] Problem is supported by measured evidence (graph data, PSS, timings,
      failing CI), not anecdote.
- [ ] Goals and Non-Goals are explicit and falsifiable.
- [ ] Target dependency direction stated; no upward edges introduced
      (verify with the Tarjan graph analysis, not by inspection).
- [ ] Alternatives section seriously compares >=2 alternatives including
      "do nothing", with why they lose.
- [ ] Cross-module shape hazards considered — Loom couplings are often
      string- or shape-based and break silently (see
      `docs/decisions/design-decisions.md`: tag formats, registry keys,
      wire shapes). Name every such coupling the RFC touches.
- [ ] Module names remain decoupled from file paths; renames do not imply
      importer rewrites.
- [ ] Backward compatibility of persisted data / wire protocol / debug
      traces addressed.
- [ ] Open questions listed with owners.

### 3.2 Production-readiness review (gate to `implementable`)

Adapted from the Kubernetes Production Readiness Review. Answer each row;
"N/A" needs a reason. See `templates/prr-checklist.md` for the full form.

- **Correctness & tests** — new behaviour gets new tests; golden suites
  (`test_ui_*`, `test_dialog_*`, `test_prompt_dialog`, truecolor) considered;
  known flake list respected; serial ctest is the signal.
- **Build system** — BMI PSS of affected producers measured before/after;
  no concurrency caps; god-interface inline-body count only decreases; no new
  textual third-party includes in module units after RFC 0001 Phase A.
- **Rollback** — atomic commits per phase; each phase independently revertible;
  interface changes go through PIMPL/erasure so importers do not flag-day.
- **Observability** — session traces (`messages.jsonl`, `dump-prompts/`) and
  any new diagnostics still work; build metrics recorded in the RFC.
- **Documentation** — `CLAUDE.md` updated when conventions change; design
  decisions catalog updated for non-obvious constraints.
- **Deletion** — prefer deleting dead code; list what the change makes
  obsolete and delete it in the same phase.
- **Deprecation** — if a module/shape is replaced, state migration and
  removal plan; silent shape drift is a bug.

## 4. Loom-specific graduation metrics

Every non-trivial phase must state how it will be measured. The canonical
instruments:

- **BMI PSS (MB)** per affected producer TU, sampled from
  `/proc/<pid>/smaps_rollup` `Pss:` while compiling (PSS, never RSS).
- **Cold/warm build wall time** on the macos-14 CI runner (3 vCPU / 14 GB)
  at default Ninja parallelism.
- **Recompile fan-out** — number of modules recompiled when one interface
  changes; body edits must eventually cost one object file.
- **Graph invariants** — output of `tools/rfc/rfc_lint.py` plus the Tarjan
  analysis: module graph stays a DAG; named directory SCCs only shrink.
- **ctest totals** — expected count stated (baseline 1706 at 2026-09-23);
  deletions must reconcile the number exactly.
- **Dual presets** — debug and release `-Werror` clean.

## 5. File format contract

- YAML frontmatter: `rfc`, `title`, `status`, `owners`, `reviewers`,
  `created`, `last-reviewed`, optional `supersedes` / `superseded-by`,
  `tracking`.
- Filename `NNNN-slug.md`; `NNNN` must equal the `rfc:` field.
- Required headings for an `implementable` RFC are enforced by
  `tools/rfc/rfc_lint.py`.
- Implementation History is an append-only table; never rewrite past rows,
  add a correction row instead.

## 6. Supporting files

- `templates/rfc-template.md` — start every new RFC from this (KEP-shaped).
- `templates/prr-checklist.md` — production-readiness questionnaire to paste
  into an RFC at the implementable gate.

## 7. Style rules specific to Loom RFCs

- Quantify first, propose second (this project's debates are settled by
  `/proc` measurements and graph scripts).
- Prefer mechanical, independently shippable phases over big-bang rewrites.
- Never propose `-j` throttling, header-file reversion, or splitting a
  library across a real SCC as a build-memory fix (all three are decided
  and lose — see RFC 0001 Section 8).
- User-visible behavioural RFCs must describe truecolor golden impact.
