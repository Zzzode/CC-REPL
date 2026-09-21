---
name: cpp-port
pattern: "^/cpp-port\\b"
---

> **ARCHIVED (2026-09-21).** The TypeScript reference tree at `src/` was deleted;
> this project is now a pure C++ codebase. The methodology below ("TS = authority,
> read `src/<ts_path>` first, leave `// TS REF:` breadcrumbs") can no longer run as
> written, and the `ts:<path-prefix>` scope token is unsatisfiable.
>
> What survives and is still useful: the build/verify commands, the golden
> `UPDATE_GOLDENS=1` procedure, and the pre-existing flake allow-list. For the
> design intent that used to live in the TS tree, see
> `cpp_migration/docs/decisions/design-decisions.md` — extracted before deletion.
>
> Kept for that remainder rather than deleted. Do not run a "port round" against
> a tree that no longer exists.

Handle the `/cpp-port` slash command by launching the `cpp-port-round`
workflow. Parse scope/switches from the raw command string and forward
as `args`.

Arguments parsed from `<raw>` (whitespace-separated):
- scope token: `all | P0 | P1 | P2 | P3 | <sev>+<sev> | subsys:<s> | ts:<p> | cpp:<p>`
- `--parallelism N` → args.parallelism = N (int, clamped 1..3)
- `--commit auto` → args.commit_mode = 'auto'
- `--push` → args.push = true

Default when `<raw>` is just `/cpp-port`: `scope='all'`, `parallelism=1`,
no auto-commit, no push.

Launch:
```js
Workflow({
  name: 'cpp-port-round',
  title: `cpp-port round: ${scope}`,
  args: { scope, parallelism, commit_mode, push },
});
```
