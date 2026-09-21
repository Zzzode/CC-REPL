# cpp_migration docs — reading note

**The TypeScript reference tree at `src/` was deleted on 2026-09-21.** The C++
tree under `cpp_migration/` was promoted to the repository root in the same
series of changes, so a path written as `cpp_migration/src/foo.cppm` in these
documents now reads as `src/foo.cppm`.

Every document in this directory predates that deletion and refers to files
that no longer exist. They are **historical records and are kept unedited** —
rewriting an audit report to match the present layout would destroy the
evidence of what was actually found and when. Read them as archaeology, not as
current documentation.

## What is still current

| File | Status |
|---|---|
| `decisions/design-decisions.md` | **Current.** Extracted from the C++ tree *before* the TS deletion: the ~800 comments that record a decision, a non-obvious constraint, or a cross-module coupling. This is the replacement for the design intent that used to live in the TS tree. Read this one first. |
| `error-handling-conventions.md` | **Current.** Conventions for the C++ tree; no TS dependency. |
| `unregistered-modules-decision-register.md` | **Stale.** Last reviewed 2026-06-12; every file it lists as unregistered has since been registered or deleted. Kept for provenance only. |
| `why-no-di.md` | **Current rationale, stale examples.** The argument (why this tree does not use a DI container) holds; some cited paths have moved. |
| `M7-dialog-system-architecture.md` | **Partly current.** The dialog-system design still describes the shipped architecture; the `cpp_migration/` prefix and some module paths have moved. |

## Historical (TS paths are dangling by design)

- `2026-06-15-migration-gap-remediation-plan.md`
- `2026-06-18-independent-completeness-audit.md`
- `2026-09-16-retained-feature-gap-audit.md`
- `2026-09-19-anthropic-decoupling-plan.md`
- `migration-audit-report.md`
- `audit_round7_full_report.json` — 38 `ts_path` values, 8 of them absolute
  macOS paths under `/Users/bytedance/`. This was the primary input to the
  archived `cpp-port-round` workflow.

The `TS REF:` breadcrumbs still present in the C++ source are in the same
category: they are now unpinned references. Where one recorded a *reason* rather
than a location, it is captured in `decisions/design-decisions.md`.
