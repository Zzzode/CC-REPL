# RFC NNNN — <Title>

- **Status:** provisional | accepted | implementable | implemented | deferred | rejected | withdrawn | replaced
- **RFC:** NNNN (must match the `NNNN` in this file's name)
- **Created:** YYYY-MM-DD
- **Last reviewed:** YYYY-MM-DD
- **Owners:** @github-handle (exactly one accountable owner)
- **Reviewers:** @github-handle, …
- **Tracking:** issue #NNNN (required at `implementable`)
- **Supersedes:** NNNN (optional)
- **Superseded by:** NNNN (optional, set when replaced)

## Summary

One paragraph: what changes and why. A reader should understand the proposal
without reading anything else.

## Motivation

Why the status quo is inadequate. **Lead with measured evidence** — graph
statistics, BMI PSS, build wall time, failing CI, fan-out counts. Anecdote is
not sufficient.

### Evidence

| Metric | Current | Target | How measured |
|---|---|---|---|
| <e.g. producer BMI PSS> |  |  |  |
| <e.g. macos-14 cold build> |  |  |  |
| <e.g. directory SCC count> |  |  |  |

## Goals

- G1. Falsifiable outcome this RFC commits to.
- G2. …

## Non-Goals

- What is explicitly out of scope, including tempting adjacent cleanups.

## Proposal

The design. Prefer diagrams for dependency/layering changes:

```
<layer> ──▶ <lower layer>
```

Cover:

- module / target / namespace surface (new modules, moved modules, deleted
  modules — module names stay decoupled from file paths);
- data / wire / persisted shapes touched (these couplings are string- or
  shape-based in this codebase and break silently — name every one, see
  `docs/decisions/design-decisions.md`);
- interface vs implementation-unit placement;
- migration order and compatibility strategy (no flag-days: PIMPL / erasure
  / re-export shims);
- debug traces, settings and user-visible behaviour impact;
- truecolor golden impact (or explicit "none, because …").

### Detailed design

<Subsections as needed.>

## Phases and graduation criteria

Required at the `implementable` gate. Each phase is independently shippable
and revertible. Criteria must be measurable; "code compiles" is not a
criterion.

| Phase | Title | Scope | Status | Graduation criteria (measured) |
|---|---|---|---|---|
| A |  |  | proposed | <numbers: PSS before→after, wall time, fan-out, lint result> |
| B |  |  | proposed |  |
| … |  |  |  |  |

Status values per phase: `proposed` · `in progress` · `done` · `dropped`
(dropped requires a rationale row in Implementation History).

## Production Readiness Review

Paste and answer `templates/prr-checklist.md` here before requesting
`implementable`. Every "No"/"N/A" needs a reason.

## Rollout and rollback

- How each phase ships independently.
- Exact rollback for each phase (revert boundary, data compatibility).
- Ordering constraints between phases.

## Drawbacks

What genuinely gets worse or more complex if this is adopted.

## Alternatives considered

At minimum: do-nothing, and two materially different approaches. State why
each loses. Pre-decided losing moves specific to Loom (do not re-litigate
without new data): build `-j` throttling, reverting to textual headers,
splitting a library across a real SCC.

## Testing and verification plan

- New / modified tests; expected ctest total (baseline 1706 @ 2026-09-23 —
  reconcile every deletion exactly).
- Golden suites touched and UPDATE_GOLDENS review plan.
- Dual preset `-Werror`; serial ctest.
- CI architecture-lint expectations.

## Documentation impact

- [ ] `CLAUDE.md` section(s) to update
- [ ] `docs/decisions/design-decisions.md` entries to add
- [ ] Code comments / module doc headers

## Open questions

| Question | Owner | Resolved by |
|---|---|---|
|  |  |  |

## Implementation History

Append-only. Never rewrite past rows; add a correction row instead.

| Date | Phase | Event | Commit / PR | Evidence (metrics, test totals) |
|---|---|---|---|---|
| YYYY-MM-DD | — | RFC opened (provisional) |  | — |
