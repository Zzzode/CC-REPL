---
name: cpp-port
pattern: "^/cpp-port\\b"
---

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
